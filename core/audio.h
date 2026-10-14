#ifndef BB_AUDIO_H
#define BB_AUDIO_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <memory>
#include "guest_memory.h"

namespace bb {

struct AudioConfig {
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::size_t buffer_frames{4096};
};

struct AudioStats {
    std::uint64_t total_frames_written{0};
    std::uint64_t total_frames_read{0};
    std::uint32_t underflow_count{0};
    std::uint32_t overflow_count{0};
};

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(std::size_t capacity_frames = 4096, std::uint32_t channels = 2);

    void reconfigure(std::size_t capacity_frames, std::uint32_t channels);
    std::size_t write(const std::int16_t *frames, std::size_t count);
    std::size_t read(std::int16_t *out_frames, std::size_t count);

    std::size_t available_read() const;
    std::size_t available_write() const;
    void reset();

    AudioStats get_stats() const;

private:
    std::size_t capacity_frames_;
    std::uint32_t channels_;
    std::vector<std::int16_t> buffer_;
    std::size_t read_pos_{0};
    std::size_t write_pos_{0};
    std::size_t count_frames_{0};
    mutable std::mutex mutex_;
    AudioStats stats_{};
};

class Atrac9Decoder;

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    bool init(const AudioConfig &config = AudioConfig{});
    void shutdown();

    void set_master_volume(float volume);
    float get_master_volume() const;
    void set_mute(bool muted);
    bool is_muted() const;

    // Submissão de áudio pelo guest
    std::size_t submit_pcm16(const std::int16_t *interleaved_samples, std::size_t frames);
    std::size_t drain_pcm16(std::int16_t *out_samples, std::size_t frames);

    // HLE: Submete áudio a partir da memória do guest
    MemoryError submit_hle_audio(const GuestMemory &mem, std::uint64_t guest_addr, std::size_t frames);

    // ATRAC9 Decoding & Stream Submission
    bool init_atrac9(const std::uint8_t config[4]);
    std::size_t submit_atrac9(const std::uint8_t *stream, std::size_t size);
    MemoryError submit_hle_atrac9(const GuestMemory &mem, std::uint64_t guest_addr, std::size_t size, const std::uint8_t config[4] = nullptr);
    bool has_atrac9_decoder() const;
    Atrac9Decoder *atrac9_decoder();

    // Gerador de onda senoidal de teste para calibração
    std::vector<std::int16_t> generate_sine_wave(float freq_hz, float duration_s, float amplitude = 0.5f);

    AudioStats get_stats() const;

private:
    AudioConfig config_{};
    AudioRingBuffer ring_buffer_;
    float master_volume_{1.0f};
    bool muted_{false};
    mutable std::mutex config_mutex_;
    std::unique_ptr<Atrac9Decoder> atrac9_decoder_;
};

} // namespace bb

#endif // BB_AUDIO_H
