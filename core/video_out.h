#pragma once

#include "guest_memory.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>

namespace bb {
enum class VideoStatus { ok, invalid, occupied, missing, unmapped, overflow };

struct VideoBuffer {
    std::uint64_t address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t bytes_per_pixel = 0;
};

struct FlipEvent {
    std::uint32_t buffer_index = 0;
    std::uint64_t argument = 0;
    std::uint64_t sequence = 0;
};

class VideoOutSink {
public:
    virtual ~VideoOutSink() = default;
    virtual void on_flip(const FlipEvent& event) = 0;
};

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

// Informação de frame renderizado para apresentação via Metal/display
struct HostFrameBuffer {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t bytes_per_pixel = 0;
    std::vector<std::uint8_t> pixel_data; // Dados efetivamente lidos da memória guest
    std::uint32_t buffer_index = 0;
    std::uint64_t flip_sequence = 0;
};

// Gerenciador de saída de vídeo desacoplado do guest memory (para HostBridge)
// Modo sintético registra metadados, mas nunca produz pixels nem frame válido.
class VideoOutManager {
public:
    explicit VideoOutManager(std::uint32_t max_buffers = 16) : max_buffers_(max_buffers) {
        internal_buffers_.resize(max_buffers);
        internal_occupied_.resize(max_buffers, false);
    }
    ~VideoOutManager() = default;

    // Anexa guest memory e sink para habilitar validação completa via VideoOut
    void attach_guest_memory(const GuestMemory* memory, VideoOutSink* sink) {
        detach_guest_memory();
        if (memory && sink) {
            memory_ = memory;
            video_out_ = std::make_unique<VideoOut>(*memory, *sink);
        }
    }
    void detach_guest_memory() {
        video_out_.reset();
        memory_ = nullptr;
        internal_last_flip_ = {};
        internal_sequence_ = 0;
        internal_has_new_frame_ = false;
        std::fill(internal_occupied_.begin(), internal_occupied_.end(), false);
        std::fill(internal_buffers_.begin(), internal_buffers_.end(), VideoBuffer{});
    }

    // Registra buffer (valida com guest memory se anexado)
    VideoStatus register_buffer(std::uint32_t index, VideoBuffer buffer) {
        if (index >= max_buffers_) return VideoStatus::invalid;
        std::size_t bytes = 0;
        const auto status = frame_size(buffer, bytes);
        if (status != VideoStatus::ok) return status;
        if (buffer.address > std::numeric_limits<std::uint64_t>::max() - bytes)
            return VideoStatus::overflow;
        if (video_out_) {
            return video_out_->register_buffer(index, buffer);
        }
        if (internal_occupied_[index]) return VideoStatus::occupied;
        internal_buffers_[index] = buffer;
        internal_occupied_[index] = true;
        return VideoStatus::ok;
    }

    VideoStatus unregister_buffer(std::uint32_t index) {
        if (index >= max_buffers_) return VideoStatus::missing;
        if (video_out_) {
            return video_out_->unregister_buffer(index);
        }
        if (!internal_occupied_[index]) return VideoStatus::missing;
        internal_occupied_[index] = false;
        internal_buffers_[index] = {};
        return VideoStatus::ok;
    }

    // Submete flip (guest pode chamar via HLE)
    VideoStatus submit_flip(std::uint32_t index, std::uint64_t argument) {
        if (index >= max_buffers_) return VideoStatus::missing;
        if (video_out_) {
            auto status = video_out_->submit_flip(index, argument);
            if (status == VideoStatus::ok) {
                // Atualiza estado interno para has_new_frame() funcionar
                internal_last_flip_ = {index, argument, video_out_->last_flip().sequence};
                internal_has_new_frame_ = true;
            }
            return status;
        }
        if (!internal_occupied_[index]) return VideoStatus::missing;
        if (internal_sequence_ == UINT64_MAX) return VideoStatus::overflow;
        internal_last_flip_ = {index, argument, ++internal_sequence_};
        internal_has_new_frame_ = true;
        return VideoStatus::ok;
    }

    // Consulta buffer
    const VideoBuffer* buffer(std::uint32_t index) const {
        if (index >= max_buffers_) return nullptr;
        if (video_out_) {
            return video_out_->buffer(index);
        }
        return internal_occupied_[index] ? &internal_buffers_[index] : nullptr;
    }

    // Só retorna um frame válido quando todos os bytes foram lidos da memória guest.
    HostFrameBuffer current_display_buffer() const {
        HostFrameBuffer result;
        read_current_frame_pixels(result);
        return result;
    }

    bool read_current_frame_pixels(HostFrameBuffer& out_frame) const {
        out_frame = {};
        if (!video_out_ || !memory_ || !internal_last_flip_.sequence) return false;
        const auto index = internal_last_flip_.buffer_index;
        const VideoBuffer* buf = video_out_->buffer(index);
        if (!buf) return false;
        std::size_t bytes = 0;
        if (frame_size(*buf, bytes) != VideoStatus::ok ||
            buf->address > std::numeric_limits<std::uint64_t>::max() - bytes) return false;
        HostFrameBuffer frame;
        frame.pixel_data.resize(bytes);
        if (memory_->load(buf->address, frame.pixel_data.data(), bytes) != MemoryError::ok) return false;
        frame.valid = true;
        frame.width = buf->width;
        frame.height = buf->height;
        frame.pitch = buf->pitch;
        frame.bytes_per_pixel = buf->bytes_per_pixel;
        frame.buffer_index = index;
        frame.flip_sequence = internal_last_flip_.sequence;
        out_frame = std::move(frame);
        return true;
    }

    std::uint64_t flip_count() const {
        return video_out_ ? video_out_->flip_count() : internal_sequence_;
    }
    FlipEvent last_flip() const {
        return video_out_ ? video_out_->last_flip() : internal_last_flip_;
    }
    bool has_new_frame() const { return internal_has_new_frame_; }
    bool consume_new_frame() {
        const bool has = internal_has_new_frame_;
        internal_has_new_frame_ = false;
        return has;
    }

    // Acesso ao VideoOut interno (para leitura de pixels do guest memory)
    const VideoOut* video_out() const { return video_out_.get(); }
    bool has_guest_memory() const { return video_out_ != nullptr; }

private:
    static VideoStatus frame_size(const VideoBuffer& buf, std::size_t& bytes) {
        if (!buf.width || !buf.height || !buf.bytes_per_pixel || buf.bytes_per_pixel > 16 ||
            buf.pitch < buf.width || buf.width > 16384 || buf.height > 16384 || buf.pitch > 16384)
            return VideoStatus::invalid;
        constexpr std::uint64_t limit = 64ull * 1024 * 1024;
        const auto row = static_cast<std::uint64_t>(buf.pitch) * buf.bytes_per_pixel;
        if (row > limit / buf.height) return VideoStatus::overflow;
        bytes = static_cast<std::size_t>(row * buf.height);
        return VideoStatus::ok;
    }
    std::uint32_t max_buffers_;
    const GuestMemory* memory_ = nullptr;
    std::unique_ptr<VideoOut> video_out_;
    std::vector<VideoBuffer> internal_buffers_;
    std::vector<bool> internal_occupied_;
    std::uint64_t internal_sequence_ = 0;
    FlipEvent internal_last_flip_{};
    mutable bool internal_has_new_frame_ = false;
};
} // namespace bb
