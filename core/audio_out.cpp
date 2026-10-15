#include "audio_out.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

namespace bb {

static inline std::int16_t clamp_s16(float val) {
    if (val > 32767.0f) return 32767;
    if (val < -32768.0f) return -32768;
    return static_cast<std::int16_t>(std::round(val));
}

AudioOutManager::AudioOutManager() = default;

AudioOutManager::~AudioOutManager() {
    close_all_ports();
}

AudioOutManager& AudioOutManager::instance() {
    static AudioOutManager s_instance;
    return s_instance;
}

std::uint64_t AudioOutManager::get_current_time_us() const {
    if (time_provider_) {
        return time_provider_();
    }
    auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

void AudioOutManager::set_time_provider(TimeProvider provider) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    time_provider_ = std::move(provider);
}

void AudioOutManager::reset_time_provider() {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    time_provider_ = nullptr;
}

std::int32_t AudioOutManager::open_port(std::int32_t user_id, std::int32_t type, std::int32_t index,
                                        std::uint32_t len, std::uint32_t freq, std::uint32_t param) {
    if (type < SCE_AUDIO_OUT_PORT_TYPE_MAIN || type > SCE_AUDIO_OUT_PORT_TYPE_AUX) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PARAM;
    }
    if (index < 0 || index >= 32) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PARAM;
    }
    if (len == 0 || len > MAX_SAMPLES_PER_BLOCK) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PARAM;
    }
    if (freq != DEFAULT_SAMPLE_RATE) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PARAM;
    }

    std::uint32_t channels = 0;
    switch (param) {
    case SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO:
        channels = 2;
        break;
    case SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO:
        channels = 1;
        break;
    case SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH:
        channels = 8;
        break;
    default:
        return SCE_AUDIO_OUT_ERROR_UNSUPPORTED_FORMAT;
    }

    std::lock_guard<std::mutex> lock(manager_mutex_);

    std::size_t active_count = 0;
    for (const auto &kv : ports_) {
        if (kv.second && kv.second->is_open) {
            ++active_count;
            if (kv.second->type == type && kv.second->index == index) {
                return SCE_AUDIO_OUT_ERROR_BUSY;
            }
        }
    }

    if (active_count >= MAX_CONCURRENT_PORTS) {
        return SCE_AUDIO_OUT_ERROR_PORT_FULL;
    }

    std::int32_t handle = next_handle_++;
    if (next_handle_ <= 0) next_handle_ = 1;
    while (ports_.find(handle) != ports_.end() && ports_[handle] && ports_[handle]->is_open) {
        handle = next_handle_++;
        if (next_handle_ <= 0) next_handle_ = 1;
    }

    auto entry = std::make_unique<PortEntry>();
    entry->handle = handle;
    entry->user_id = user_id;
    entry->type = type;
    entry->index = index;
    entry->len = len;
    entry->freq = freq;
    entry->format = param;
    entry->channels = channels;
    entry->volume_left = SCE_AUDIO_OUT_VOLUME_0dB;
    entry->volume_right = SCE_AUDIO_OUT_VOLUME_0dB;
    entry->total_frames_output = 0;
    entry->last_output_time_us = 0;
    entry->is_open = true;

    std::size_t ring_capacity = std::max<std::size_t>(static_cast<std::size_t>(len) * 8, 4096);
    entry->ring_buffer = std::make_unique<AudioRingBuffer>(ring_capacity, channels);

    ports_[handle] = std::move(entry);
    return handle;
}

std::int32_t AudioOutManager::close_port(std::int32_t handle) {
    if (handle <= 0) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    std::unique_ptr<PortEntry> port_to_delete;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto it = ports_.find(handle);
        if (it == ports_.end() || !it->second || !it->second->is_open) {
            return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
        }
        {
            std::lock_guard<std::mutex> port_lock(it->second->mutex);
            it->second->is_open = false;
            if (it->second->ring_buffer) {
                it->second->ring_buffer->reset();
            }
        }
        port_to_delete = std::move(it->second);
        ports_.erase(it);
    }
    return SCE_OK;
}

bool AudioOutManager::is_port_open(std::int32_t handle) const {
    if (handle <= 0) return false;
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    return (it != ports_.end() && it->second && it->second->is_open);
}

std::size_t AudioOutManager::open_ports_count() const {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    std::size_t count = 0;
    for (const auto &kv : ports_) {
        if (kv.second && kv.second->is_open) {
            ++count;
        }
    }
    return count;
}

void AudioOutManager::close_all_ports() {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for (auto &kv : ports_) {
        if (kv.second) {
            std::lock_guard<std::mutex> port_lock(kv.second->mutex);
            kv.second->is_open = false;
            if (kv.second->ring_buffer) {
                kv.second->ring_buffer->reset();
            }
        }
    }
    ports_.clear();
}

std::int32_t AudioOutManager::output(std::int32_t handle, const void *ptr, std::size_t frames) {
    if (!ptr) return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;
    if (handle <= 0) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end() || !it->second || !it->second->is_open) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    }
    PortEntry *port = it->second.get();
    std::lock_guard<std::mutex> port_lock(port->mutex);
    if (!port->is_open || !port->ring_buffer) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    }

    std::size_t count = (frames > 0) ? frames : static_cast<std::size_t>(port->len);
    if (count == 0) return SCE_OK;

    const auto *src = static_cast<const std::int16_t*>(ptr);
    std::uint32_t ch = port->channels;
    std::int32_t vl = port->volume_left;
    std::int32_t vr = port->volume_right;

    if (vl == SCE_AUDIO_OUT_VOLUME_0dB && vr == SCE_AUDIO_OUT_VOLUME_0dB) {
        port->ring_buffer->write(src, count);
    } else if (vl <= 0 && vr <= 0) {
        std::vector<std::int16_t> silence(count * ch, static_cast<std::int16_t>(0));
        port->ring_buffer->write(silence.data(), count);
    } else {
        float gain_l = static_cast<float>(vl) / static_cast<float>(SCE_AUDIO_OUT_VOLUME_0dB);
        float gain_r = static_cast<float>(vr) / static_cast<float>(SCE_AUDIO_OUT_VOLUME_0dB);
        std::vector<std::int16_t> scaled(count * ch);

        if (ch == 2) {
            for (std::size_t i = 0; i < count; ++i) {
                scaled[i * 2 + 0] = clamp_s16(static_cast<float>(src[i * 2 + 0]) * gain_l);
                scaled[i * 2 + 1] = clamp_s16(static_cast<float>(src[i * 2 + 1]) * gain_r);
            }
        } else if (ch == 1) {
            float gain = (gain_l + gain_r) * 0.5f;
            for (std::size_t i = 0; i < count; ++i) {
                scaled[i] = clamp_s16(static_cast<float>(src[i]) * gain);
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                scaled[i * ch + 0] = clamp_s16(static_cast<float>(src[i * ch + 0]) * gain_l);
                scaled[i * ch + 1] = clamp_s16(static_cast<float>(src[i * ch + 1]) * gain_r);
                for (std::uint32_t c = 2; c < ch; ++c) {
                    scaled[i * ch + c] = src[i * ch + c];
                }
            }
        }
        port->ring_buffer->write(scaled.data(), count);
    }

    port->total_frames_output += count;
    port->last_output_time_us = get_current_time_us();
    return SCE_OK;
}

std::int32_t AudioOutManager::output_hle(std::int32_t handle, const GuestMemory &mem, std::uint64_t guest_addr, std::size_t frames) {
    if (guest_addr == 0) return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;
    if (handle <= 0) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    std::size_t count = 0;
    std::uint32_t ch = 2;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto it = ports_.find(handle);
        if (it == ports_.end() || !it->second || !it->second->is_open) {
            return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
        }
        count = (frames > 0) ? frames : static_cast<std::size_t>(it->second->len);
        ch = it->second->channels;
    }

    if (count == 0) return SCE_OK;
    std::size_t bytes = count * ch * sizeof(std::int16_t);
    std::vector<std::int16_t> temp(count * ch);
    MemoryError err = mem.load(guest_addr, temp.data(), bytes);
    if (err != MemoryError::ok) {
        return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;
    }

    return output(handle, temp.data(), count);
}

std::int32_t AudioOutManager::set_volume(std::int32_t handle, std::int32_t flag, const std::int32_t *vol) {
    if (!vol) return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;
    if (handle <= 0) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (flag == 0 || (flag & ~SCE_AUDIO_OUT_VOLUME_FLAG_STEREO) != 0) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end() || !it->second || !it->second->is_open) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    }
    PortEntry *port = it->second.get();
    std::lock_guard<std::mutex> port_lock(port->mutex);
    if (!port->is_open) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    int idx = 0;
    if (flag & SCE_AUDIO_OUT_VOLUME_FLAG_LEFT) {
        port->volume_left = std::max(0, vol[idx++]);
    }
    if (flag & SCE_AUDIO_OUT_VOLUME_FLAG_RIGHT) {
        port->volume_right = std::max(0, vol[idx++]);
    }
    return SCE_OK;
}

std::int32_t AudioOutManager::set_volume(std::int32_t handle, std::int32_t flag, int32_t *vol) {
    return set_volume(handle, flag, static_cast<const std::int32_t*>(vol));
}

std::int32_t AudioOutManager::set_volume_float(std::int32_t handle, float left, float right) {
    auto vl = static_cast<std::int32_t>(std::clamp(left, 0.0f, 2.0f) * static_cast<float>(SCE_AUDIO_OUT_VOLUME_0dB));
    auto vr = static_cast<std::int32_t>(std::clamp(right, 0.0f, 2.0f) * static_cast<float>(SCE_AUDIO_OUT_VOLUME_0dB));
    std::int32_t vols[2] = {vl, vr};
    return set_volume(handle, SCE_AUDIO_OUT_VOLUME_FLAG_STEREO, vols);
}

std::int32_t AudioOutManager::get_port_info(std::int32_t handle, SceAudioOutPortInfo *info) const {
    if (!info) return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;
    if (handle <= 0) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end() || !it->second || !it->second->is_open) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    }
    const PortEntry *port = it->second.get();
    std::lock_guard<std::mutex> port_lock(port->mutex);
    if (!port->is_open) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    info->status = port->is_open ? 1 : 0;
    info->port_type = port->type;
    info->index = port->index;
    info->samples_per_block = port->len;
    info->sample_rate = port->freq;
    info->channels = port->channels;
    info->format = port->format;
    info->volume_left = port->volume_left;
    info->volume_right = port->volume_right;
    info->total_frames_output = port->total_frames_output;
    if (port->ring_buffer) {
        auto stats = port->ring_buffer->get_stats();
        info->underflow_count = stats.underflow_count;
        info->overflow_count = stats.overflow_count;
    } else {
        info->underflow_count = 0;
        info->overflow_count = 0;
    }
    info->last_output_time_us = port->last_output_time_us;
    return SCE_OK;
}

std::int32_t AudioOutManager::get_last_output_time(std::int32_t handle, std::uint64_t *output_time) const {
    if (!output_time) return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;
    if (handle <= 0) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end() || !it->second || !it->second->is_open) {
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    }
    const PortEntry *port = it->second.get();
    std::lock_guard<std::mutex> port_lock(port->mutex);
    if (!port->is_open) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    *output_time = port->last_output_time_us;
    return SCE_OK;
}

std::size_t AudioOutManager::drain_port_pcm16(std::int32_t handle, std::int16_t *out_samples, std::size_t frames) {
    if (!out_samples || frames == 0 || handle <= 0) return 0;

    // Manter o lock do gerenciador até terminar a leitura: close_port pode
    // destruir a PortEntry assim que o lock for liberado.
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end() || !it->second || !it->second->is_open) return 0;
    auto& port = *it->second;
    std::lock_guard<std::mutex> port_lock(port.mutex);
    if (!port.is_open || !port.ring_buffer) return 0;
    return port.ring_buffer->read(out_samples, frames);
}

std::size_t AudioOutManager::drain_main_port_pcm16(std::int16_t *out_samples, std::size_t frames) {
    if (!out_samples || frames == 0) return 0;

    // Saída do host é S16 estéreo: converter os canais do guest sem
    // ultrapassar o buffer do callback (que tem capacidade 2 * frames).
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for (const auto& kv : ports_) {
        if (!kv.second || !kv.second->is_open ||
            kv.second->type != SCE_AUDIO_OUT_PORT_TYPE_MAIN) continue;
        auto& port = *kv.second;
        std::lock_guard<std::mutex> port_lock(port.mutex);
        if (!port.is_open || !port.ring_buffer) continue;
        if (port.channels == 2) return port.ring_buffer->read(out_samples, frames);
        if (port.channels != 1 && port.channels != 8) continue;
        if (frames > std::vector<std::int16_t>().max_size() / port.channels) return 0;
        std::vector<std::int16_t> input(frames * port.channels);
        const auto received = port.ring_buffer->read(input.data(), frames);
        for (std::size_t i = 0; i < received; ++i) {
            out_samples[2 * i] = input[i * port.channels];
            out_samples[2 * i + 1] = input[i * port.channels + (port.channels == 1 ? 0 : 1)];
        }
        return received;
    }
    return 0;
}

AudioRingBuffer* AudioOutManager::get_port_ring_buffer(std::int32_t handle) {
    if (handle <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end() || !it->second || !it->second->is_open) return nullptr;
    return it->second->ring_buffer.get();
}

std::int32_t sceAudioOutInit() {
    return SCE_OK;
}

std::int32_t sceAudioOutOpen(std::int32_t userId, std::int32_t type, std::int32_t index,
                             std::uint32_t len, std::uint32_t freq, std::uint32_t param) {
    return AudioOutManager::instance().open_port(userId, type, index, len, freq, param);
}

std::int32_t sceAudioOutClose(std::int32_t handle) {
    return AudioOutManager::instance().close_port(handle);
}

std::int32_t sceAudioOutOutput(std::int32_t handle, const void *ptr) {
    return AudioOutManager::instance().output(handle, ptr, 0);
}

std::int32_t sceAudioOutOutputHle(std::int32_t handle, const GuestMemory &mem, std::uint64_t guest_addr) {
    return AudioOutManager::instance().output_hle(handle, mem, guest_addr, 0);
}

std::int32_t sceAudioOutSetVolume(std::int32_t handle, std::int32_t flag, const std::int32_t *vol) {
    return AudioOutManager::instance().set_volume(handle, flag, vol);
}

std::int32_t sceAudioOutSetVolume(std::int32_t handle, std::int32_t flag, int32_t *vol) {
    return AudioOutManager::instance().set_volume(handle, flag, static_cast<const std::int32_t*>(vol));
}

std::int32_t sceAudioOutGetPortInfo(std::int32_t handle, SceAudioOutPortInfo *info) {
    return AudioOutManager::instance().get_port_info(handle, info);
}

std::int32_t sceAudioOutGetPortState(std::int32_t handle, SceAudioOutPortInfo *info) {
    return AudioOutManager::instance().get_port_info(handle, info);
}

std::int32_t sceAudioOutGetLastOutputTime(std::int32_t handle, std::uint64_t *output_time) {
    return AudioOutManager::instance().get_last_output_time(handle, output_time);
}

} // namespace bb
