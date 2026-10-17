#pragma once

#include "guest_memory.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <vector>

namespace bb {
enum class VideoStatus { ok, invalid, occupied, missing, unmapped, overflow };

struct VideoBuffer {
    std::uint64_t address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t bytes_per_pixel = 0;
    std::uint32_t pixel_format = 0; // Orbis PixelFormat (ex: 0x80000000 = A8R8G8B8Srgb)
    std::uint32_t tiling_mode = 1;  // 0 = Tile (micro-tiled Display2DThin), 1 = Linear
};

// Estrutura canônica de atributos de buffer Orbis (40 bytes / 0x28 bytes)
struct BufferAttribute {
    std::uint32_t pixel_format = 0;
    std::uint32_t tiling_mode = 0;
    std::int32_t aspect_ratio = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch_in_pixel = 0;
    std::uint32_t option = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
};
static_assert(sizeof(BufferAttribute) == 40, "BufferAttribute deve ter exatamente 40 bytes");

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
    std::uint32_t pixel_format = 0;
    std::uint32_t tiling_mode = 1;
    bool is_tiled = false;
    std::vector<std::uint8_t> pixel_data; // Dados efetivamente lidos da memória guest
    std::uint32_t buffer_index = 0;
    std::uint64_t flip_sequence = 0;
};

// Formatos de pixel Orbis VideoOut canônicos
constexpr std::uint32_t ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB = 0x80000000;
constexpr std::uint32_t ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8      = 0x00000000;
constexpr std::uint32_t ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8_SRGB = 0x80000001;
constexpr std::uint32_t ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8      = 0x00000001;
constexpr std::uint32_t ORBIS_VIDEO_OUT_PIXEL_FORMAT_R10_G10_B10_A2   = 0x00000002;
constexpr std::uint32_t ORBIS_VIDEO_OUT_PIXEL_FORMAT_R10_G10_B10_A2_SRGB = 0x80000002;

// Modos de tiling Orbis VideoOut
constexpr std::uint32_t ORBIS_VIDEO_OUT_TILING_MODE_TILE   = 0; // Display2DThin micro-tile 8x8
constexpr std::uint32_t ORBIS_VIDEO_OUT_TILING_MODE_LINEAR = 1; // Linear

enum class VideoConversionStatus {
    ok = 0,
    unsupported_format = 1,
    unsupported_tiling = 2,
    invalid_dimensions = 3,
    buffer_too_small = 4,
    unmapped_memory = 5,
    null_data = 6
};

const char* video_conversion_status_to_string(VideoConversionStatus status);

// Frame convertido e linearizado pronto para upload em textura de apresentação Metal/display
struct PresentationFrame {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;                 // Passo de linha em pixels no buffer linear
    std::uint32_t bytes_per_pixel = 4;
    std::uint32_t pixel_format = ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB; // BGRA8 final padrão
    std::uint32_t original_format = 0;
    std::uint32_t original_tiling_mode = 1;
    bool was_tiled = false;
    std::vector<std::uint8_t> pixel_data;     // Dados lineares BGRA8
    std::uint32_t buffer_index = 0;
    std::uint64_t flip_sequence = 0;
    VideoConversionStatus status = VideoConversionStatus::ok;
    std::string diagnostic;
};

// Conversor determinístico de layout (untiling Display2DThin / Morton 2D) e formatos de cor
class VideoFrameConverter {
public:
    static VideoConversionStatus convert(const HostFrameBuffer& src, PresentationFrame& out);

    static bool detile_display2d_thin(
        const std::uint8_t* src,
        std::uint8_t* dst,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t src_pitch_pixels,
        std::uint32_t dst_pitch_pixels,
        bool swap_rb = false
    );

    static bool detile_morton_8x8(
        const std::uint8_t* src,
        std::uint8_t* dst,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t src_pitch_pixels,
        std::uint32_t dst_pitch_pixels,
        bool swap_rb = false
    );

    static bool tile_display2d_thin(
        const std::uint8_t* src,
        std::uint8_t* dst,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t src_pitch_pixels,
        std::uint32_t dst_pitch_pixels,
        bool swap_rb = false
    );

    static bool tile_morton_8x8(
        const std::uint8_t* src,
        std::uint8_t* dst,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t src_pitch_pixels,
        std::uint32_t dst_pitch_pixels,
        bool swap_rb = false
    );

    static bool is_supported_pixel_format(std::uint32_t format);
    static std::uint32_t get_pixel_format_bpp(std::uint32_t format);
};

// Gerenciador de saída de vídeo desacoplado do guest memory (para HostBridge)
// Modo sintético registra metadados, mas nunca produz pixels nem frame válido.
class VideoOutManager {
public:
    explicit VideoOutManager(std::uint32_t max_buffers = 16)
        : max_buffers_(std::min<std::uint32_t>(max_buffers, 16)) {
        internal_buffers_.resize(max_buffers_);
        internal_occupied_.resize(max_buffers_, false);
    }
    ~VideoOutManager() = default;

    // Anexa guest memory e sink para habilitar validação completa via VideoOut
    void attach_guest_memory(const GuestMemory* memory, VideoOutSink* sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        video_out_.reset();
        memory_ = nullptr;
        internal_last_flip_ = {};
        internal_sequence_ = 0;
        internal_has_new_frame_ = false;
        std::fill(internal_occupied_.begin(), internal_occupied_.end(), false);
        std::fill(internal_buffers_.begin(), internal_buffers_.end(), VideoBuffer{});
        if (memory && sink) {
            memory_ = memory;
            video_out_ = std::make_unique<VideoOut>(*memory, *sink);
        }
    }
    void detach_guest_memory() {
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (video_out_) {
            auto status = video_out_->submit_flip(index, argument);
            if (status == VideoStatus::ok) {
                internal_last_flip_ = video_out_->last_flip();
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        frame.pixel_format = buf->pixel_format;
        frame.tiling_mode = buf->tiling_mode;
        frame.is_tiled = (buf->tiling_mode == 0);
        frame.buffer_index = index;
        frame.flip_sequence = internal_last_flip_.sequence;
        out_frame = std::move(frame);
        return true;
    }

    std::uint64_t flip_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return video_out_ ? video_out_->flip_count() : internal_sequence_;
    }
    FlipEvent last_flip() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return video_out_ ? video_out_->last_flip() : internal_last_flip_;
    }
    bool has_new_frame() const { return internal_has_new_frame_.load(std::memory_order_acquire); }
    bool consume_new_frame() {
        return internal_has_new_frame_.exchange(false, std::memory_order_acq_rel);
    }

    // Acesso ao VideoOut interno (para leitura de pixels do guest memory)
    const VideoOut* video_out() const { return video_out_.get(); }
    bool has_guest_memory() const { return video_out_ != nullptr; }

    bool read_presentation_frame(PresentationFrame& out_frame) const {
        out_frame = {};
        HostFrameBuffer raw_frame;
        if (!read_current_frame_pixels(raw_frame)) {
            std::lock_guard<std::mutex> lock(mutex_);
            out_frame.valid = false;
            out_frame.status = (!video_out_ || !memory_) ? VideoConversionStatus::unmapped_memory : VideoConversionStatus::null_data;
            out_frame.diagnostic = (!video_out_ || !memory_) ? "Guest memory desconectada ou indisponivel" : "Nenhum frame valido para apresentacao";
            last_conversion_status_ = out_frame.status;
            last_conversion_diagnostic_ = out_frame.diagnostic;
            conversion_failure_count_++;
            return false;
        }
        auto status = VideoFrameConverter::convert(raw_frame, out_frame);
        std::lock_guard<std::mutex> lock(mutex_);
        last_conversion_status_ = status;
        last_conversion_diagnostic_ = out_frame.diagnostic;
        if (status == VideoConversionStatus::ok) {
            conversion_success_count_++;
            return true;
        } else {
            conversion_failure_count_++;
            return false;
        }
    }

    PresentationFrame current_presentation_frame() const {
        PresentationFrame result;
        read_presentation_frame(result);
        return result;
    }

    VideoConversionStatus last_conversion_status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_conversion_status_;
    }

    std::string last_conversion_diagnostic() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_conversion_diagnostic_;
    }

    std::uint64_t conversion_success_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return conversion_success_count_;
    }

    std::uint64_t conversion_failure_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return conversion_failure_count_;
    }

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
    mutable std::mutex mutex_;
    std::uint32_t max_buffers_;
    const GuestMemory* memory_ = nullptr;
    std::unique_ptr<VideoOut> video_out_;
    std::vector<VideoBuffer> internal_buffers_;
    std::vector<bool> internal_occupied_;
    std::uint64_t internal_sequence_ = 0;
    FlipEvent internal_last_flip_{};
    mutable std::atomic<bool> internal_has_new_frame_{false};
    mutable VideoConversionStatus last_conversion_status_{VideoConversionStatus::ok};
    mutable std::string last_conversion_diagnostic_{};
    mutable std::uint64_t conversion_success_count_{0};
    mutable std::uint64_t conversion_failure_count_{0};
};
} // namespace bb
