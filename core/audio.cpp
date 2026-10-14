#include "audio.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace bb {

AudioRingBuffer::AudioRingBuffer(std::size_t capacity_frames, std::uint32_t channels)
    : capacity_frames_(capacity_frames), channels_(channels), buffer_(capacity_frames * channels, static_cast<std::int16_t>(0)) {}

void AudioRingBuffer::reconfigure(std::size_t capacity_frames, std::uint32_t channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_frames_ = capacity_frames;
    channels_ = channels;
    buffer_.assign(capacity_frames * channels, static_cast<std::int16_t>(0));
    read_pos_ = 0;
    write_pos_ = 0;
    count_frames_ = 0;
}

std::size_t AudioRingBuffer::available_read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_frames_;
}

std::size_t AudioRingBuffer::available_write() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_frames_ - count_frames_;
}

void AudioRingBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    read_pos_ = 0;
    write_pos_ = 0;
    count_frames_ = 0;
    std::fill(buffer_.begin(), buffer_.end(), static_cast<std::int16_t>(0));
}

AudioStats AudioRingBuffer::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::size_t AudioRingBuffer::write(const std::int16_t *frames, std::size_t count) {
    if (!frames || count == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t space = capacity_frames_ - count_frames_;
    std::size_t to_write = std::min(count, space);
    if (to_write < count) {
        ++stats_.overflow_count;
    }

    if (to_write == 0) return 0;

    std::size_t first_chunk = std::min(to_write, capacity_frames_ - write_pos_);
    std::size_t second_chunk = to_write - first_chunk;

    std::memcpy(&buffer_[write_pos_ * channels_], frames, first_chunk * channels_ * sizeof(std::int16_t));
    if (second_chunk > 0) {
        std::memcpy(&buffer_[0], frames + (first_chunk * channels_), second_chunk * channels_ * sizeof(std::int16_t));
    }

    write_pos_ = (write_pos_ + to_write) % capacity_frames_;
    count_frames_ += to_write;
    stats_.total_frames_written += to_write;
    return to_write;
}

std::size_t AudioRingBuffer::read(std::int16_t *out_frames, std::size_t count) {
    if (!out_frames || count == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t to_read = std::min(count, count_frames_);
    if (to_read < count) {
        ++stats_.underflow_count;
        // Preenche com silêncio o que não pôde ser lido
        std::memset(out_frames + (to_read * channels_), 0, (count - to_read) * channels_ * sizeof(std::int16_t));
    }

    if (to_read > 0) {
        std::size_t first_chunk = std::min(to_read, capacity_frames_ - read_pos_);
        std::size_t second_chunk = to_read - first_chunk;

        std::memcpy(out_frames, &buffer_[read_pos_ * channels_], first_chunk * channels_ * sizeof(std::int16_t));
        if (second_chunk > 0) {
            std::memcpy(out_frames + (first_chunk * channels_), &buffer_[0], second_chunk * channels_ * sizeof(std::int16_t));
        }

        read_pos_ = (read_pos_ + to_read) % capacity_frames_;
        count_frames_ -= to_read;
        stats_.total_frames_read += to_read;
    }

    return to_read;
}

AudioManager::AudioManager()
    : ring_buffer_(4096, 2) {}

bool AudioManager::init(const AudioConfig &config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
    ring_buffer_.reconfigure(config.buffer_frames, config.channels);
    return true;
}

void AudioManager::shutdown() {
    ring_buffer_.reset();
}

void AudioManager::set_master_volume(float volume) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    master_volume_ = std::clamp(volume, 0.0f, 1.0f);
}

float AudioManager::get_master_volume() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return master_volume_;
}

void AudioManager::set_mute(bool muted) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    muted_ = muted;
}

bool AudioManager::is_muted() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return muted_;
}

std::size_t AudioManager::submit_pcm16(const std::int16_t *interleaved_samples, std::size_t frames) {
    if (!interleaved_samples || frames == 0) return 0;

    float vol = 1.0f;
    bool is_mute = false;
    std::uint32_t ch = 2;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        vol = master_volume_;
        is_mute = muted_;
        ch = config_.channels;
    }

    if (is_mute || vol <= 0.0001f) {
        std::vector<std::int16_t> silence(frames * ch, static_cast<std::int16_t>(0));
        return ring_buffer_.write(silence.data(), frames);
    }

    if (std::abs(vol - 1.0f) < 0.0001f) {
        return ring_buffer_.write(interleaved_samples, frames);
    }

    std::vector<std::int16_t> scaled(frames * ch);
    for (std::size_t i = 0; i < frames * ch; ++i) {
        float sample = static_cast<float>(interleaved_samples[i]) * vol;
        scaled[i] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
    }
    return ring_buffer_.write(scaled.data(), frames);
}

std::size_t AudioManager::drain_pcm16(std::int16_t *out_samples, std::size_t frames) {
    return ring_buffer_.read(out_samples, frames);
}

MemoryError AudioManager::submit_hle_audio(const GuestMemory &mem, std::uint64_t guest_addr, std::size_t frames) {
    std::uint32_t ch = 2;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        ch = config_.channels;
    }
    std::size_t bytes = frames * ch * sizeof(std::int16_t);
    std::vector<std::int16_t> temp(frames * ch);
    MemoryError err = mem.load(guest_addr, temp.data(), bytes);
    if (err != MemoryError::ok) return err;

    submit_pcm16(temp.data(), frames);
    return MemoryError::ok;
}

std::vector<std::int16_t> AudioManager::generate_sine_wave(float freq_hz, float duration_s, float amplitude) {
    std::uint32_t rate = 48000;
    std::uint32_t ch = 2;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        rate = config_.sample_rate;
        ch = config_.channels;
    }

    std::size_t total_frames = static_cast<std::size_t>(std::round(static_cast<float>(rate) * duration_s));
    std::vector<std::int16_t> out(total_frames * ch);
    constexpr double two_pi = 6.28318530717958647692;

    for (std::size_t i = 0; i < total_frames; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(rate);
        double sample_val = std::sin(two_pi * static_cast<double>(freq_hz) * t) * static_cast<double>(amplitude);
        std::int16_t s16 = static_cast<std::int16_t>(std::clamp(sample_val * 32767.0, -32768.0, 32767.0));
        for (std::uint32_t c = 0; c < ch; ++c) {
            out[i * ch + c] = s16;
        }
    }
    return out;
}

AudioStats AudioManager::get_stats() const {
    return ring_buffer_.get_stats();
}

} // namespace bb
