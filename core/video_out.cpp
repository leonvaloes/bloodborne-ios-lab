#include "video_out.h"
#include <limits>

namespace bb {
VideoStatus VideoOut::register_buffer(std::uint32_t index, VideoBuffer b) {
    if (index >= buffers_.size() || !b.width || !b.height || !b.bytes_per_pixel ||
        b.bytes_per_pixel > 16 || b.pitch < b.width || b.width > 16384 || b.height > 16384 ||
        b.pitch > 16384) return VideoStatus::invalid;
    if (occupied_[index]) return VideoStatus::occupied;
    const auto bytes = std::uint64_t(b.pitch) * b.height * b.bytes_per_pixel;
    if (b.address > UINT64_MAX - bytes) return VideoStatus::overflow;
    // Verifica começo e fim; buffers não contíguos falham ao tentar acessá-los mais tarde.
    std::uint8_t sample;
    if (memory_.load(b.address, &sample, 1) != MemoryError::ok ||
        memory_.load(b.address + bytes - 1, &sample, 1) != MemoryError::ok)
        return VideoStatus::unmapped;
    buffers_[index] = b;
    occupied_[index] = true;
    return VideoStatus::ok;
}
VideoStatus VideoOut::unregister_buffer(std::uint32_t index) {
    if (index >= buffers_.size() || !occupied_[index]) return VideoStatus::missing;
    occupied_[index] = false;
    buffers_[index] = {};
    return VideoStatus::ok;
}
const VideoBuffer* VideoOut::buffer(std::uint32_t index) const {
    return index < buffers_.size() && occupied_[index] ? &buffers_[index] : nullptr;
}
VideoStatus VideoOut::submit_flip(std::uint32_t index, std::uint64_t argument) {
    if (!buffer(index)) return VideoStatus::missing;
    if (sequence_ == UINT64_MAX) return VideoStatus::overflow;
    last_ = {index, argument, ++sequence_};
    sink_.on_flip(last_);
    return VideoStatus::ok;
}
}
