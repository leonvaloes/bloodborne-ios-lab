#include "ngs2_hle.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace bb {

namespace {

constexpr double NGS2_PI     = 3.14159265358979323846;
constexpr double NGS2_TWO_PI = 6.28318530717958647692;

inline std::int16_t clamp_s16(float val) {
    if (val > 32767.0f) return 32767;
    if (val < -32768.0f) return -32768;
    return static_cast<std::int16_t>(std::round(val));
}

} // namespace

// ============================================================================
// Ngs2Manager Lifecycle and Singleton
// ============================================================================

Ngs2Manager::Ngs2Manager() = default;

Ngs2Manager::~Ngs2Manager() {
    destroy_all_racks();
}

Ngs2Manager& Ngs2Manager::instance() {
    static Ngs2Manager s_instance;
    return s_instance;
}

std::shared_ptr<Ngs2Manager::Rack> Ngs2Manager::get_rack(SceNgs2RackHandle handle) const {
    if (handle <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = racks_.find(handle);
    if (it == racks_.end()) return nullptr;
    return it->second;
}

// ============================================================================
// Rack Management
// ============================================================================

std::int32_t Ngs2Manager::create_rack(const SceNgs2RackParam *param, SceNgs2RackHandle *out_handle) {
    if (!param || !out_handle) {
        return SCE_NGS2_ERROR_INVALID_POINTER;
    }
    if (param->maxVoices == 0) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (param->maxVoices > SCE_NGS2_MAX_VOICES_PER_RACK) {
        return SCE_NGS2_ERROR_VOICE_LIMIT_EXCEEDED;
    }
    if (param->channels != 1 && param->channels != 2) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (param->blockSize == 0 || param->blockSize > SCE_NGS2_MAX_BLOCK_SIZE) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (param->sampleRate != SCE_NGS2_DEFAULT_SAMPLE_RATE) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(manager_mutex_);
    if (racks_.size() >= SCE_NGS2_MAX_RACKS) {
        return SCE_NGS2_ERROR_OUT_OF_MEMORY;
    }

    SceNgs2RackHandle handle = next_rack_handle_++;
    if (next_rack_handle_ <= 0) next_rack_handle_ = 1;
    while (racks_.find(handle) != racks_.end()) {
        handle = next_rack_handle_++;
        if (next_rack_handle_ <= 0) next_rack_handle_ = 1;
    }

    auto rack = std::make_shared<Rack>();
    rack->handle = handle;
    rack->param = *param;
    rack->voices.resize(param->maxVoices);
    for (std::uint32_t i = 0; i < param->maxVoices; ++i) {
        rack->voices[i].id = i;
        rack->voices[i].state = SCE_NGS2_VOICE_STATE_STOPPED;
        rack->voices[i].volume = 1.0f;
        rack->voices[i].pan = 0.0f;
        rack->voices[i].pitch = 1.0f;
        rack->voices[i].has_position_3d = false;
        rack->voices[i].min_distance = 1.0f;
        rack->voices[i].max_distance = 100.0f;
        rack->voices[i].rolloff = 1.0f;
        rack->voices[i].source_type = VoiceSourceType::None;
        rack->voices[i].loop = false;
        rack->voices[i].playback_pos = 0.0;
        rack->voices[i].synthetic_freq = 440.0f;
        rack->voices[i].synthetic_amplitude = 1.0f;
        rack->voices[i].synthetic_phase = 0.0;
        rack->voices[i].frames_rendered = 0;
    }
    rack->total_blocks_rendered = 0;
    rack->is_destroyed = false;

    racks_[handle] = rack;
    *out_handle = handle;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::destroy_rack(SceNgs2RackHandle handle) {
    if (handle <= 0) {
        return SCE_NGS2_ERROR_INVALID_HANDLE;
    }

    std::shared_ptr<Rack> rack;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto it = racks_.find(handle);
        if (it == racks_.end()) {
            return SCE_NGS2_ERROR_INVALID_HANDLE;
        }
        rack = it->second;
        racks_.erase(it);
    }

    if (rack) {
        std::lock_guard<std::mutex> rack_lock(rack->rack_mutex);
        rack->is_destroyed = true;
        for (auto &v : rack->voices) {
            v.state = SCE_NGS2_VOICE_STATE_STOPPED;
            v.samples.clear();
            v.sample_count = 0;
            v.playback_pos = 0.0;
            v.synthetic_phase = 0.0;
        }
    }

    return SCE_NGS2_OK;
}

bool Ngs2Manager::is_rack_valid(SceNgs2RackHandle handle) const {
    if (handle <= 0) return false;
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto it = racks_.find(handle);
    return (it != racks_.end() && it->second && !it->second->is_destroyed);
}

std::size_t Ngs2Manager::active_racks_count() const {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    std::size_t count = 0;
    for (const auto &kv : racks_) {
        if (kv.second && !kv.second->is_destroyed) {
            ++count;
        }
    }
    return count;
}

void Ngs2Manager::destroy_all_racks() {
    std::unordered_map<SceNgs2RackHandle, std::shared_ptr<Rack>> racks_to_destroy;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        racks_to_destroy = std::move(racks_);
        racks_.clear();
    }
    for (auto &kv : racks_to_destroy) {
        if (kv.second) {
            std::lock_guard<std::mutex> rack_lock(kv.second->rack_mutex);
            kv.second->is_destroyed = true;
            for (auto &v : kv.second->voices) {
                v.state = SCE_NGS2_VOICE_STATE_STOPPED;
                v.samples.clear();
            }
        }
    }
}

std::int32_t Ngs2Manager::set_rack_audio_out_port(SceNgs2RackHandle rack_handle, std::int32_t audio_out_port) {
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;

    rack->param.audioOutPort = audio_out_port;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::get_rack_audio_out_port(SceNgs2RackHandle rack_handle, std::int32_t *out_port) const {
    if (!out_port) return SCE_NGS2_ERROR_INVALID_POINTER;
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;

    *out_port = rack->param.audioOutPort;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::get_rack_info(SceNgs2RackHandle rack_handle, SceNgs2RackInfo *out_info) const {
    if (!out_info) return SCE_NGS2_ERROR_INVALID_POINTER;
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;

    out_info->handle = rack->handle;
    out_info->max_voices = rack->param.maxVoices;
    out_info->channels = rack->param.channels;
    out_info->block_size = rack->param.blockSize;
    out_info->sample_rate = rack->param.sampleRate;
    out_info->audio_out_port = rack->param.audioOutPort;
    out_info->total_blocks_rendered = rack->total_blocks_rendered;

    std::uint32_t active = 0;
    for (const auto &v : rack->voices) {
        if (v.state == SCE_NGS2_VOICE_STATE_PLAYING) {
            ++active;
        }
    }
    out_info->active_voices = active;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::get_voice_info(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, SceNgs2VoiceInfo *out_info) const {
    if (!out_info) return SCE_NGS2_ERROR_INVALID_POINTER;
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    const auto &v = rack->voices[voice_index];
    out_info->state = v.state;
    out_info->volume = v.volume;
    out_info->pan = v.pan;
    out_info->pitch = v.pitch;
    out_info->loop = v.loop;
    out_info->has_position_3d = v.has_position_3d;
    out_info->position_3d = v.position_3d;
    out_info->min_distance = v.min_distance;
    out_info->max_distance = v.max_distance;
    out_info->rolloff = v.rolloff;
    out_info->frames_rendered = v.frames_rendered;
    return SCE_NGS2_OK;
}

// ============================================================================
// Voice Playback Control
// ============================================================================

std::int32_t Ngs2Manager::voice_play(SceNgs2RackHandle rack_handle, std::uint32_t voice_index) {
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    if (v.state == SCE_NGS2_VOICE_STATE_STOPPED) {
        v.playback_pos = 0.0;
        v.synthetic_phase = 0.0;
    }
    v.state = SCE_NGS2_VOICE_STATE_PLAYING;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_stop(SceNgs2RackHandle rack_handle, std::uint32_t voice_index) {
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    v.state = SCE_NGS2_VOICE_STATE_STOPPED;
    v.playback_pos = 0.0;
    v.synthetic_phase = 0.0;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_pause(SceNgs2RackHandle rack_handle, std::uint32_t voice_index) {
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    if (v.state == SCE_NGS2_VOICE_STATE_PLAYING) {
        v.state = SCE_NGS2_VOICE_STATE_PAUSED;
    }
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_get_state(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, std::int32_t *out_state) const {
    if (!out_state) return SCE_NGS2_ERROR_INVALID_POINTER;
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    *out_state = rack->voices[voice_index].state;
    return SCE_NGS2_OK;
}

// ============================================================================
// Voice Parameter Modulation (Volume, Pan, Pitch, 3D Spatial)
// ============================================================================

std::int32_t Ngs2Manager::voice_set_volume(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float volume) {
    if (std::isnan(volume) || std::isinf(volume) || volume < 0.0f || volume > 10.0f) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    rack->voices[voice_index].volume = volume;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_set_pan(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float pan) {
    if (std::isnan(pan) || std::isinf(pan) || pan < -1.0f || pan > 1.0f) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    rack->voices[voice_index].pan = pan;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_set_pitch(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float pitch) {
    if (std::isnan(pitch) || std::isinf(pitch) || pitch <= 0.0f || pitch > 16.0f) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    rack->voices[voice_index].pitch = pitch;
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_set_position_3d(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float x, float y, float z) {
    if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
        std::isinf(x) || std::isinf(y) || std::isinf(z)) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    v.has_position_3d = true;
    v.position_3d = { x, y, z };
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_set_distance_attenuation(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                                        float min_distance, float max_distance, float rolloff) {
    if (std::isnan(min_distance) || std::isnan(max_distance) || std::isnan(rolloff) ||
        std::isinf(min_distance) || std::isinf(max_distance) || std::isinf(rolloff) ||
        min_distance < 0.0f || max_distance < min_distance || rolloff < 0.0f) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    v.min_distance = min_distance;
    v.max_distance = max_distance;
    v.rolloff = rolloff;
    return SCE_NGS2_OK;
}

// ============================================================================
// Voice Waveform & Buffer Configuration
// ============================================================================

std::int32_t Ngs2Manager::voice_set_buffer(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                           const std::int16_t *pcm_data, std::size_t num_frames,
                                           std::uint32_t channels, bool loop) {
    if (!pcm_data) {
        return SCE_NGS2_ERROR_INVALID_POINTER;
    }
    if (num_frames == 0) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (channels != 1 && channels != 2) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    v.source_type = VoiceSourceType::PcmBuffer;
    v.buffer_channels = channels;
    v.sample_count = num_frames;
    v.loop = loop;
    v.playback_pos = 0.0;
    v.samples.resize(num_frames * channels);
    for (std::size_t i = 0; i < num_frames * channels; ++i) {
        v.samples[i] = static_cast<float>(pcm_data[i]) / 32768.0f;
    }
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_set_buffer_float(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                                 const float *pcm_data, std::size_t num_frames,
                                                 std::uint32_t channels, bool loop) {
    if (!pcm_data) {
        return SCE_NGS2_ERROR_INVALID_POINTER;
    }
    if (num_frames == 0) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (channels != 1 && channels != 2) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    v.source_type = VoiceSourceType::PcmBuffer;
    v.buffer_channels = channels;
    v.sample_count = num_frames;
    v.loop = loop;
    v.playback_pos = 0.0;
    v.samples.assign(pcm_data, pcm_data + (num_frames * channels));
    return SCE_NGS2_OK;
}

std::int32_t Ngs2Manager::voice_set_buffer_hle(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                               const GuestMemory &mem, std::uint64_t guest_addr,
                                               std::size_t num_bytes, std::uint32_t channels, bool loop) {
    if (guest_addr == 0) {
        return SCE_NGS2_ERROR_INVALID_POINTER;
    }
    if (num_bytes == 0) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (channels != 1 && channels != 2) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    std::size_t frame_bytes = sizeof(std::int16_t) * channels;
    if ((num_bytes % frame_bytes) != 0) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }

    std::size_t num_frames = num_bytes / frame_bytes;
    std::size_t total_samples = num_frames * channels;
    std::vector<std::int16_t> temp(total_samples);

    MemoryError err = mem.load(guest_addr, temp.data(), num_bytes);
    if (err != MemoryError::ok) {
        return SCE_NGS2_ERROR_INVALID_POINTER;
    }

    return voice_set_buffer(rack_handle, voice_index, temp.data(), num_frames, channels, loop);
}

std::int32_t Ngs2Manager::voice_set_synthetic(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                              float freq_hz, float amplitude, bool loop) {
    if (std::isnan(freq_hz) || std::isinf(freq_hz) || freq_hz <= 0.0f || freq_hz > 24000.0f) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    if (std::isnan(amplitude) || std::isinf(amplitude) || amplitude < 0.0f || amplitude > 10.0f) {
        return SCE_NGS2_ERROR_INVALID_PARAM;
    }
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::lock_guard<std::mutex> lock(rack->rack_mutex);
    if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;
    if (voice_index >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_PARAM;

    auto &v = rack->voices[voice_index];
    v.source_type = VoiceSourceType::SyntheticSine;
    v.synthetic_freq = freq_hz;
    v.synthetic_amplitude = amplitude;
    v.synthetic_phase = 0.0;
    v.loop = loop;
    return SCE_NGS2_OK;
}

// ============================================================================
// Voice Rack Synthesis & Rendering Cycle
// ============================================================================

std::int32_t Ngs2Manager::render_rack(SceNgs2RackHandle rack_handle,
                                      std::int32_t override_audio_out_port,
                                      std::int16_t *out_pcm,
                                      std::size_t max_frames,
                                      std::size_t *frames_rendered,
                                      AudioOutManager *audio_out_mgr) {
    auto rack = get_rack(rack_handle);
    if (!rack) return SCE_NGS2_ERROR_INVALID_HANDLE;

    std::uint32_t block_size = 0;
    std::uint32_t channels = 0;
    std::int32_t target_port = -1;
    std::vector<std::int16_t> pcm_out;

    {
        std::lock_guard<std::mutex> lock(rack->rack_mutex);
        if (rack->is_destroyed) return SCE_NGS2_ERROR_INVALID_HANDLE;

        block_size = rack->param.blockSize;
        channels = rack->param.channels;
        target_port = (override_audio_out_port >= 0) ? override_audio_out_port : rack->param.audioOutPort;

        std::vector<float> mix_left(block_size, 0.0f);
        std::vector<float> mix_right(block_size, 0.0f);

        for (auto &v : rack->voices) {
            if (v.state != SCE_NGS2_VOICE_STATE_PLAYING) {
                continue;
            }

            float eff_volume = v.volume;
            float eff_pan = v.pan;

            // Atenuação de distância e posicionamento 3D
            if (v.has_position_3d) {
                float dx = v.position_3d.x;
                float dy = v.position_3d.y;
                float dz = v.position_3d.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

                float dist_factor = 1.0f;
                if (dist > v.min_distance) {
                    if (dist >= v.max_distance) {
                        dist_factor = 0.0f;
                    } else {
                        dist_factor = v.min_distance / (v.min_distance + v.rolloff * (dist - v.min_distance));
                    }
                }
                eff_volume *= std::clamp(dist_factor, 0.0f, 1.0f);

                if (dist > 0.0001f) {
                    eff_pan = std::clamp(dx / dist, -1.0f, 1.0f);
                }
            }

            // Ganhos estéreo com pan equal-power
            float gain_l = eff_volume;
            float gain_r = eff_volume;
            if (channels == 2) {
                float angle = static_cast<float>((eff_pan + 1.0f) * 0.25 * NGS2_PI);
                gain_l = eff_volume * std::cos(angle);
                gain_r = eff_volume * std::sin(angle);
            }

            // Síntese / geração das amostras do bloco
            for (std::uint32_t f = 0; f < block_size; ++f) {
                float s_left = 0.0f;
                float s_right = 0.0f;

                if (v.source_type == VoiceSourceType::PcmBuffer) {
                    if (v.sample_count == 0 || v.samples.empty()) {
                        v.state = SCE_NGS2_VOICE_STATE_STOPPED;
                        break;
                    }
                    if (v.playback_pos >= static_cast<double>(v.sample_count)) {
                        if (v.loop) {
                            v.playback_pos = std::fmod(v.playback_pos, static_cast<double>(v.sample_count));
                        } else {
                            v.state = SCE_NGS2_VOICE_STATE_STOPPED;
                            v.playback_pos = 0.0;
                            break;
                        }
                    }

                    std::size_t idx0 = static_cast<std::size_t>(v.playback_pos);
                    std::size_t idx1 = idx0 + 1;
                    float frac = static_cast<float>(v.playback_pos - static_cast<double>(idx0));

                    if (v.buffer_channels == 1) {
                        float samp0 = v.samples[idx0];
                        float samp1 = (idx1 < v.sample_count) ? v.samples[idx1] : (v.loop ? v.samples[0] : samp0);
                        float samp = samp0 + frac * (samp1 - samp0);
                        s_left = samp;
                        s_right = samp;
                    } else {
                        float l0 = v.samples[idx0 * 2 + 0];
                        float r0 = v.samples[idx0 * 2 + 1];
                        float l1 = (idx1 < v.sample_count) ? v.samples[idx1 * 2 + 0] : (v.loop ? v.samples[0] : l0);
                        float r1 = (idx1 < v.sample_count) ? v.samples[idx1 * 2 + 1] : (v.loop ? v.samples[1] : r0);
                        s_left = l0 + frac * (l1 - l0);
                        s_right = r0 + frac * (r1 - r0);
                    }

                    v.playback_pos += v.pitch;
                    v.frames_rendered++;
                } else if (v.source_type == VoiceSourceType::SyntheticSine) {
                    float samp = static_cast<float>(std::sin(v.synthetic_phase) * v.synthetic_amplitude);
                    s_left = samp;
                    s_right = samp;

                    double phase_inc = (NGS2_TWO_PI * static_cast<double>(v.synthetic_freq) * static_cast<double>(v.pitch)) /
                                       static_cast<double>(rack->param.sampleRate);
                    v.synthetic_phase += phase_inc;
                    if (v.synthetic_phase >= NGS2_TWO_PI) {
                        v.synthetic_phase = std::fmod(v.synthetic_phase, NGS2_TWO_PI);
                    }
                    v.frames_rendered++;
                }

                if (channels == 2) {
                    mix_left[f]  += s_left * gain_l;
                    mix_right[f] += s_right * gain_r;
                } else {
                    mix_left[f]  += (s_left + s_right) * 0.5f * eff_volume;
                }
            }
        }

        // Conversão final do sinal mixado para PCM16 com saturação segura
        pcm_out.resize(block_size * channels);
        if (channels == 2) {
            for (std::uint32_t f = 0; f < block_size; ++f) {
                pcm_out[f * 2 + 0] = clamp_s16(mix_left[f] * 32767.0f);
                pcm_out[f * 2 + 1] = clamp_s16(mix_right[f] * 32767.0f);
            }
        } else {
            for (std::uint32_t f = 0; f < block_size; ++f) {
                pcm_out[f] = clamp_s16(mix_left[f] * 32767.0f);
            }
        }

        rack->total_blocks_rendered++;
    }

    // Copia amostras sintetizadas para o buffer do host se requisitado
    if (out_pcm && max_frames > 0) {
        std::size_t to_copy = std::min(static_cast<std::size_t>(block_size), max_frames);
        std::memcpy(out_pcm, pcm_out.data(), to_copy * channels * sizeof(std::int16_t));
        if (frames_rendered) {
            *frames_rendered = to_copy;
        }
    }

    // Submissão direta à porta do AudioOutManager (fora do mutex do rack para evitar deadlocks)
    if (target_port > 0) {
        AudioOutManager &aom = audio_out_mgr ? *audio_out_mgr : AudioOutManager::instance();
        std::int32_t aom_res = aom.output(target_port, pcm_out.data(), block_size);
        if (aom_res != SCE_OK) {
            return aom_res;
        }
    }

    return SCE_NGS2_OK;
}

// ============================================================================
// SCE C HLE Wrappers
// ============================================================================

std::int32_t sceNgs2RackCreate(const SceNgs2RackParam *param, SceNgs2RackHandle *outHandle) {
    return Ngs2Manager::instance().create_rack(param, outHandle);
}

std::int32_t sceNgs2RackCreate(std::uint32_t maxVoices, std::uint32_t channels,
                               std::uint32_t blockSize, SceNgs2RackHandle *outHandle) {
    SceNgs2RackParam param{};
    param.maxVoices = maxVoices;
    param.channels = channels;
    param.blockSize = blockSize;
    param.sampleRate = SCE_NGS2_DEFAULT_SAMPLE_RATE;
    param.audioOutPort = -1;
    return Ngs2Manager::instance().create_rack(&param, outHandle);
}

std::int32_t sceNgs2RackDestroy(SceNgs2RackHandle handle) {
    return Ngs2Manager::instance().destroy_rack(handle);
}

std::int32_t sceNgs2RackGetInfo(SceNgs2RackHandle handle, SceNgs2RackInfo *outInfo) {
    return Ngs2Manager::instance().get_rack_info(handle, outInfo);
}

std::int32_t sceNgs2RackSetAudioOutPort(SceNgs2RackHandle handle, std::int32_t audioOutPort) {
    return Ngs2Manager::instance().set_rack_audio_out_port(handle, audioOutPort);
}

std::int32_t sceNgs2RackGetAudioOutPort(SceNgs2RackHandle handle, std::int32_t *outPort) {
    return Ngs2Manager::instance().get_rack_audio_out_port(handle, outPort);
}

std::int32_t sceNgs2VoicePlay(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex) {
    return Ngs2Manager::instance().voice_play(rackHandle, voiceIndex);
}

std::int32_t sceNgs2VoicePlay(SceNgs2VoiceHandle voiceHandle) {
    return sceNgs2VoicePlay(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle));
}

std::int32_t sceNgs2VoiceStop(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex) {
    return Ngs2Manager::instance().voice_stop(rackHandle, voiceIndex);
}

std::int32_t sceNgs2VoiceStop(SceNgs2VoiceHandle voiceHandle) {
    return sceNgs2VoiceStop(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle));
}

std::int32_t sceNgs2VoicePause(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex) {
    return Ngs2Manager::instance().voice_pause(rackHandle, voiceIndex);
}

std::int32_t sceNgs2VoicePause(SceNgs2VoiceHandle voiceHandle) {
    return sceNgs2VoicePause(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle));
}

std::int32_t sceNgs2VoiceGetState(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, std::int32_t *outState) {
    return Ngs2Manager::instance().voice_get_state(rackHandle, voiceIndex, outState);
}

std::int32_t sceNgs2VoiceGetState(SceNgs2VoiceHandle voiceHandle, std::int32_t *outState) {
    return sceNgs2VoiceGetState(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle), outState);
}

std::int32_t sceNgs2VoiceGetInfo(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, SceNgs2VoiceInfo *outInfo) {
    return Ngs2Manager::instance().get_voice_info(rackHandle, voiceIndex, outInfo);
}

std::int32_t sceNgs2VoiceGetInfo(SceNgs2VoiceHandle voiceHandle, SceNgs2VoiceInfo *outInfo) {
    return sceNgs2VoiceGetInfo(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle), outInfo);
}

std::int32_t sceNgs2VoiceSetVolume(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float volume) {
    return Ngs2Manager::instance().voice_set_volume(rackHandle, voiceIndex, volume);
}

std::int32_t sceNgs2VoiceSetVolume(SceNgs2VoiceHandle voiceHandle, float volume) {
    return sceNgs2VoiceSetVolume(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle), volume);
}

std::int32_t sceNgs2VoiceSetPan(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float pan) {
    return Ngs2Manager::instance().voice_set_pan(rackHandle, voiceIndex, pan);
}

std::int32_t sceNgs2VoiceSetPan(SceNgs2VoiceHandle voiceHandle, float pan) {
    return sceNgs2VoiceSetPan(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle), pan);
}

std::int32_t sceNgs2VoiceSetPitch(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float pitch) {
    return Ngs2Manager::instance().voice_set_pitch(rackHandle, voiceIndex, pitch);
}

std::int32_t sceNgs2VoiceSetPitch(SceNgs2VoiceHandle voiceHandle, float pitch) {
    return sceNgs2VoiceSetPitch(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle), pitch);
}

std::int32_t sceNgs2VoiceSetPosition3D(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float x, float y, float z) {
    return Ngs2Manager::instance().voice_set_position_3d(rackHandle, voiceIndex, x, y, z);
}

std::int32_t sceNgs2VoiceSetPosition3D(SceNgs2VoiceHandle voiceHandle, float x, float y, float z) {
    return sceNgs2VoiceSetPosition3D(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle), x, y, z);
}

std::int32_t sceNgs2VoiceSetDistanceAttenuation(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                                float minDistance, float maxDistance, float rolloff) {
    return Ngs2Manager::instance().voice_set_distance_attenuation(rackHandle, voiceIndex, minDistance, maxDistance, rolloff);
}

std::int32_t sceNgs2VoiceSetDistanceAttenuation(SceNgs2VoiceHandle voiceHandle,
                                                float minDistance, float maxDistance, float rolloff) {
    return sceNgs2VoiceSetDistanceAttenuation(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle),
                                              minDistance, maxDistance, rolloff);
}

std::int32_t sceNgs2VoiceSetBuffer(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                   const std::int16_t *pcmData, std::size_t numFrames,
                                   std::uint32_t channels, bool loop) {
    return Ngs2Manager::instance().voice_set_buffer(rackHandle, voiceIndex, pcmData, numFrames, channels, loop);
}

std::int32_t sceNgs2VoiceSetBuffer(SceNgs2VoiceHandle voiceHandle,
                                   const std::int16_t *pcmData, std::size_t numFrames,
                                   std::uint32_t channels, bool loop) {
    return sceNgs2VoiceSetBuffer(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle),
                                 pcmData, numFrames, channels, loop);
}

std::int32_t sceNgs2VoiceSetBufferFloat(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                        const float *pcmData, std::size_t numFrames,
                                        std::uint32_t channels, bool loop) {
    return Ngs2Manager::instance().voice_set_buffer_float(rackHandle, voiceIndex, pcmData, numFrames, channels, loop);
}

std::int32_t sceNgs2VoiceSetBufferFloat(SceNgs2VoiceHandle voiceHandle,
                                        const float *pcmData, std::size_t numFrames,
                                        std::uint32_t channels, bool loop) {
    return sceNgs2VoiceSetBufferFloat(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle),
                                      pcmData, numFrames, channels, loop);
}

std::int32_t sceNgs2VoiceSetBufferHle(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                      const GuestMemory &mem, std::uint64_t guest_addr,
                                      std::size_t numBytes, std::uint32_t channels, bool loop) {
    return Ngs2Manager::instance().voice_set_buffer_hle(rackHandle, voiceIndex, mem, guest_addr, numBytes, channels, loop);
}

std::int32_t sceNgs2VoiceSetBufferHle(SceNgs2VoiceHandle voiceHandle,
                                      const GuestMemory &mem, std::uint64_t guest_addr,
                                      std::size_t numBytes, std::uint32_t channels, bool loop) {
    return sceNgs2VoiceSetBufferHle(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle),
                                    mem, guest_addr, numBytes, channels, loop);
}

std::int32_t sceNgs2VoiceSetSyntheticWaveform(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                              float freqHz, float amplitude, bool loop) {
    return Ngs2Manager::instance().voice_set_synthetic(rackHandle, voiceIndex, freqHz, amplitude, loop);
}

std::int32_t sceNgs2VoiceSetSyntheticWaveform(SceNgs2VoiceHandle voiceHandle,
                                              float freqHz, float amplitude, bool loop) {
    return sceNgs2VoiceSetSyntheticWaveform(sceNgs2VoiceHandleGetRack(voiceHandle), sceNgs2VoiceHandleGetIndex(voiceHandle),
                                            freqHz, amplitude, loop);
}

std::int32_t sceNgs2RackRender(SceNgs2RackHandle rackHandle) {
    return Ngs2Manager::instance().render_rack(rackHandle);
}

std::int32_t sceNgs2RackRender(SceNgs2RackHandle rackHandle, std::int32_t audioOutPort) {
    return Ngs2Manager::instance().render_rack(rackHandle, audioOutPort);
}

std::int32_t sceNgs2RackRender(SceNgs2RackHandle rackHandle, std::int16_t *outPcm,
                               std::size_t maxFrames, std::size_t *framesRendered) {
    return Ngs2Manager::instance().render_rack(rackHandle, -1, outPcm, maxFrames, framesRendered);
}

} // namespace bb
