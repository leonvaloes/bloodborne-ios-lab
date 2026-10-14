#pragma once
#include "guest_memory.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace bb {
enum class VideoStatus { ok, invalid, occupied, missing, unmapped, overflow };
struct VideoBuffer { std::uint64_t address = 0; std::uint32_t width = 0, height = 0, pitch = 0, bytes_per_pixel = 0; };
struct FlipEvent { std::uint32_t buffer_index = 0; std::uint64_t argument = 0, sequence = 0; };
class VideoOutSink {
public:
    virtual ~VideoOutSink() = default;
    virtual void on_flip(const FlipEvent& event) = 0;
};
// Modelo de gerenciamento: sem sincronização vertical, conversão de pixel ou apresentação.
class VideoOut {
public:
    explicit VideoOut(const GuestMemory& memory, VideoOutSink& sink) : memory_(memory), sink_(sink) {}
    VideoStatus register_buffer(std::uint32_t index, VideoBuffer buffer);
    VideoStatus unregister_buffer(std::uint32_t index);
    VideoStatus submit_flip(std::uint32_t index, std::uint64_t argument);
    const VideoBuffer* buffer(std::uint32_t index) const;
    std::uint64_t flip_count() const { return sequence_; }
    FlipEvent last_flip() const { return last_; }
private:
    const GuestMemory& memory_;
    VideoOutSink& sink_;
    std::array<VideoBuffer, 16> buffers_{};
    std::array<bool, 16> occupied_{};
    std::uint64_t sequence_ = 0;
    FlipEvent last_{};
};
}
