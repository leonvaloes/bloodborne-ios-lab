#include "video_out.h"
#include <cstdio>
#include <cstring>
#include <limits>

namespace bb {
VideoStatus VideoOut::register_buffer(std::uint32_t index, VideoBuffer b) {
    if (index >= buffers_.size() || !b.width || !b.height || !b.bytes_per_pixel ||
        b.bytes_per_pixel > 16 || b.pitch < b.width || b.width > 16384 || b.height > 16384 ||
        b.pitch > 16384) return VideoStatus::invalid;
    if (occupied_[index]) return VideoStatus::occupied;
    constexpr std::uint64_t max_frame_bytes = 64ull * 1024 * 1024;
    const auto row_bytes = std::uint64_t(b.pitch) * b.bytes_per_pixel;
    if (row_bytes > max_frame_bytes / b.height) return VideoStatus::overflow;
    const auto bytes = row_bytes * b.height;
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

const char* video_conversion_status_to_string(VideoConversionStatus status) {
    switch (status) {
        case VideoConversionStatus::ok: return "ok";
        case VideoConversionStatus::unsupported_format: return "unsupported_format";
        case VideoConversionStatus::unsupported_tiling: return "unsupported_tiling";
        case VideoConversionStatus::invalid_dimensions: return "invalid_dimensions";
        case VideoConversionStatus::buffer_too_small: return "buffer_too_small";
        case VideoConversionStatus::unmapped_memory: return "unmapped_memory";
        case VideoConversionStatus::null_data: return "null_data";
        default: return "unknown";
    }
}

bool VideoFrameConverter::detile_display2d_thin(
    const std::uint8_t* src,
    std::uint8_t* dst,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t src_pitch_pixels,
    std::uint32_t dst_pitch_pixels,
    bool swap_rb
) {
    if (!src || !dst || width == 0 || height == 0 || src_pitch_pixels < width || dst_pitch_pixels < width) {
        return false;
    }
    const std::uint32_t aligned_pitch = (src_pitch_pixels + 7U) & ~7U;
    const std::uint32_t tiles_per_row = aligned_pitch / 8U;

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t ty = y / 8U;
        const std::uint32_t ly = y % 8U;
        const std::uint32_t dst_row_byte = y * dst_pitch_pixels * 4U;

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t tx = x / 8U;
            const std::uint32_t lx = x % 8U;

            const std::uint32_t tile_idx = ty * tiles_per_row + tx;
            const std::uint32_t pixel_in_tile = ly * 8U + lx;
            const std::uint32_t src_pixel_idx = tile_idx * 64U + pixel_in_tile;
            const std::uint32_t src_byte = src_pixel_idx * 4U;
            const std::uint32_t dst_byte = dst_row_byte + x * 4U;

            if (swap_rb) {
                dst[dst_byte + 0] = src[src_byte + 2]; // B <- R
                dst[dst_byte + 1] = src[src_byte + 1]; // G <- G
                dst[dst_byte + 2] = src[src_byte + 0]; // R <- B
                dst[dst_byte + 3] = src[src_byte + 3]; // A <- A
            } else {
                std::memcpy(&dst[dst_byte], &src[src_byte], 4U);
            }
        }
    }
    return true;
}

bool VideoFrameConverter::detile_morton_8x8(
    const std::uint8_t* src,
    std::uint8_t* dst,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t src_pitch_pixels,
    std::uint32_t dst_pitch_pixels,
    bool swap_rb
) {
    if (!src || !dst || width == 0 || height == 0 || src_pitch_pixels < width || dst_pitch_pixels < width) {
        return false;
    }
    const std::uint32_t aligned_pitch = (src_pitch_pixels + 7U) & ~7U;
    const std::uint32_t tiles_per_row = aligned_pitch / 8U;

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t ty = y / 8U;
        const std::uint32_t ly = y % 8U;
        const std::uint32_t dst_row_byte = y * dst_pitch_pixels * 4U;

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t tx = x / 8U;
            const std::uint32_t lx = x % 8U;

            const std::uint32_t tile_idx = ty * tiles_per_row + tx;
            const std::uint32_t pixel_in_tile =
                (lx & 1U) | ((ly & 1U) << 1U) |
                (((lx >> 1U) & 1U) << 2U) | (((ly >> 1U) & 1U) << 3U) |
                (((lx >> 2U) & 1U) << 4U) | (((ly >> 2U) & 1U) << 5U);

            const std::uint32_t src_pixel_idx = tile_idx * 64U + pixel_in_tile;
            const std::uint32_t src_byte = src_pixel_idx * 4U;
            const std::uint32_t dst_byte = dst_row_byte + x * 4U;

            if (swap_rb) {
                dst[dst_byte + 0] = src[src_byte + 2];
                dst[dst_byte + 1] = src[src_byte + 1];
                dst[dst_byte + 2] = src[src_byte + 0];
                dst[dst_byte + 3] = src[src_byte + 3];
            } else {
                std::memcpy(&dst[dst_byte], &src[src_byte], 4U);
            }
        }
    }
    return true;
}

bool VideoFrameConverter::is_supported_pixel_format(std::uint32_t format) {
    switch (format) {
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB:
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8:
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8_SRGB:
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8:
        case 0x82000000:
        case 0x84000000:
        case 0xA2000000:
        case 0xC1060000:
            return true;
        default:
            return false;
    }
}

std::uint32_t VideoFrameConverter::get_pixel_format_bpp(std::uint32_t format) {
    if (format == 0xC1060000) return 8;
    return 4;
}

bool VideoFrameConverter::tile_display2d_thin(
    const std::uint8_t* src,
    std::uint8_t* dst,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t src_pitch_pixels,
    std::uint32_t dst_pitch_pixels,
    bool swap_rb
) {
    if (!src || !dst || width == 0 || height == 0 || src_pitch_pixels < width || dst_pitch_pixels < width) {
        return false;
    }
    const std::uint32_t aligned_pitch = (dst_pitch_pixels + 7U) & ~7U;
    const std::uint32_t tiles_per_row = aligned_pitch / 8U;

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t ty = y / 8U;
        const std::uint32_t ly = y % 8U;
        const std::uint32_t src_row_byte = y * src_pitch_pixels * 4U;

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t tx = x / 8U;
            const std::uint32_t lx = x % 8U;

            const std::uint32_t tile_idx = ty * tiles_per_row + tx;
            const std::uint32_t pixel_in_tile = ly * 8U + lx;
            const std::uint32_t dst_pixel_idx = tile_idx * 64U + pixel_in_tile;
            const std::uint32_t dst_byte = dst_pixel_idx * 4U;
            const std::uint32_t src_byte = src_row_byte + x * 4U;

            if (swap_rb) {
                dst[dst_byte + 0] = src[src_byte + 2];
                dst[dst_byte + 1] = src[src_byte + 1];
                dst[dst_byte + 2] = src[src_byte + 0];
                dst[dst_byte + 3] = src[src_byte + 3];
            } else {
                std::memcpy(&dst[dst_byte], &src[src_byte], 4U);
            }
        }
    }
    return true;
}

bool VideoFrameConverter::tile_morton_8x8(
    const std::uint8_t* src,
    std::uint8_t* dst,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t src_pitch_pixels,
    std::uint32_t dst_pitch_pixels,
    bool swap_rb
) {
    if (!src || !dst || width == 0 || height == 0 || src_pitch_pixels < width || dst_pitch_pixels < width) {
        return false;
    }
    const std::uint32_t aligned_pitch = (dst_pitch_pixels + 7U) & ~7U;
    const std::uint32_t tiles_per_row = aligned_pitch / 8U;

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t ty = y / 8U;
        const std::uint32_t ly = y % 8U;
        const std::uint32_t src_row_byte = y * src_pitch_pixels * 4U;

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t tx = x / 8U;
            const std::uint32_t lx = x % 8U;

            const std::uint32_t tile_idx = ty * tiles_per_row + tx;
            const std::uint32_t pixel_in_tile =
                (lx & 1U) | ((ly & 1U) << 1U) |
                (((lx >> 1U) & 1U) << 2U) | (((ly >> 1U) & 1U) << 3U) |
                (((lx >> 2U) & 1U) << 4U) | (((ly >> 2U) & 1U) << 5U);

            const std::uint32_t dst_pixel_idx = tile_idx * 64U + pixel_in_tile;
            const std::uint32_t dst_byte = dst_pixel_idx * 4U;
            const std::uint32_t src_byte = src_row_byte + x * 4U;

            if (swap_rb) {
                dst[dst_byte + 0] = src[src_byte + 2];
                dst[dst_byte + 1] = src[src_byte + 1];
                dst[dst_byte + 2] = src[src_byte + 0];
                dst[dst_byte + 3] = src[src_byte + 3];
            } else {
                std::memcpy(&dst[dst_byte], &src[src_byte], 4U);
            }
        }
    }
    return true;
}

VideoConversionStatus VideoFrameConverter::convert(const HostFrameBuffer& src, PresentationFrame& out) {
    out = {};
    out.buffer_index = src.buffer_index;
    out.flip_sequence = src.flip_sequence;
    out.original_format = src.pixel_format;
    out.original_tiling_mode = src.tiling_mode;
    out.was_tiled = (src.tiling_mode == 0 || src.is_tiled);

    if (!src.valid || src.pixel_data.empty()) {
        out.status = VideoConversionStatus::null_data;
        out.diagnostic = "Dados de pixel vazios ou quadro marcado invalido";
        return out.status;
    }

    if (src.width == 0 || src.height == 0 || src.pitch < src.width ||
        src.width > 16384 || src.height > 16384 || src.pitch > 16384) {
        out.status = VideoConversionStatus::invalid_dimensions;
        out.diagnostic = "Dimensoes ou passo invalidos: width=" + std::to_string(src.width) +
                         " height=" + std::to_string(src.height) + " pitch=" + std::to_string(src.pitch);
        return out.status;
    }

    const std::uint32_t expected_bpp = get_pixel_format_bpp(src.pixel_format);
    if (src.bytes_per_pixel != expected_bpp) {
        out.status = VideoConversionStatus::unsupported_format;
        out.diagnostic = "Bytes por pixel (" + std::to_string(src.bytes_per_pixel) +
                         ") incompativel com formato esperado (" + std::to_string(expected_bpp) + " bpp)";
        return out.status;
    }

    // Validação estrita de formato de pixel
    bool swap_rb = false;
    bool is_fp16 = (src.pixel_format == 0xC1060000);
    switch (src.pixel_format) {
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB:
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8:
        case 0x84000000:
            swap_rb = false;
            break;
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8_SRGB:
        case ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8:
        case 0x82000000:
        case 0xA2000000:
            swap_rb = true;
            break;
        case 0xC1060000: // RGBA16Float
            swap_rb = false;
            break;
        default: {
            char hex_buf[32];
            std::snprintf(hex_buf, sizeof(hex_buf), "0x%08X", src.pixel_format);
            out.status = VideoConversionStatus::unsupported_format;
            out.diagnostic = std::string("Formato de pixel Orbis ") + hex_buf +
                             " nao suportado; suportados sao BGRA8 (0x80000000/0x84000000), RGBA8 (0x80000001/0x82000000/0xA2000000) e RGBA16Float (0xC1060000)";
            return out.status;
        }
    }

    // Validação estrita de modo de tiling
    if (src.tiling_mode > 1) {
        out.status = VideoConversionStatus::unsupported_tiling;
        out.diagnostic = "Modo de tiling " + std::to_string(src.tiling_mode) +
                         " nao suportado; suportados sao 0 (Display2DThin 8x8) e 1 (Linear)";
        return out.status;
    }

    const std::uint32_t bpp = expected_bpp;
    const std::uint32_t out_pitch = src.width;
    const std::size_t out_bytes = static_cast<std::size_t>(out_pitch) * bpp * src.height;
    out.pixel_data.resize(out_bytes);

    if (src.tiling_mode == 0 || src.is_tiled) {
        // Tiled: Display2DThin 8x8
        const std::uint32_t aligned_pitch = (src.pitch + 7U) & ~7U;
        const std::uint32_t aligned_height = (src.height + 7U) & ~7U;
        const std::size_t min_tiled_bytes = static_cast<std::size_t>(aligned_pitch) * bpp * aligned_height;
        if (src.pixel_data.size() < min_tiled_bytes) {
            out.status = VideoConversionStatus::buffer_too_small;
            out.diagnostic = "Buffer tiled insuficiente: requer " + std::to_string(min_tiled_bytes) +
                             " bytes, fornecido " + std::to_string(src.pixel_data.size());
            out.pixel_data.clear();
            return out.status;
        }

        if (bpp == 4) {
            if (!detile_display2d_thin(src.pixel_data.data(), out.pixel_data.data(),
                                       src.width, src.height, src.pitch, out_pitch, swap_rb)) {
                out.status = VideoConversionStatus::invalid_dimensions;
                out.diagnostic = "Falha durante o des-tiling Display2DThin";
                out.pixel_data.clear();
                return out.status;
            }
        } else {
            // bpp == 8 (RGBA16Float Display2DThin 8x8 micro-tiled)
            const std::uint32_t tiles_per_row = aligned_pitch / 8U;
            for (std::uint32_t y = 0; y < src.height; ++y) {
                const std::uint32_t ty = y / 8U;
                const std::uint32_t ly = y % 8U;
                const std::uint32_t dst_row_byte = y * out_pitch * 8U;
                for (std::uint32_t x = 0; x < src.width; ++x) {
                    const std::uint32_t tx = x / 8U;
                    const std::uint32_t lx = x % 8U;
                    const std::uint32_t tile_idx = ty * tiles_per_row + tx;
                    const std::uint32_t pixel_in_tile = ly * 8U + lx;
                    const std::uint32_t src_byte = (tile_idx * 64U + pixel_in_tile) * 8U;
                    const std::uint32_t dst_byte = dst_row_byte + x * 8U;
                    std::memcpy(&out.pixel_data[dst_byte], &src.pixel_data[src_byte], 8U);
                }
            }
        }
    } else {
        // Linear: tiling_mode == 1
        const std::size_t min_linear_bytes = static_cast<std::size_t>(src.pitch) * bpp * src.height;
        if (src.pixel_data.size() < min_linear_bytes) {
            out.status = VideoConversionStatus::buffer_too_small;
            out.diagnostic = "Buffer linear insuficiente: requer " + std::to_string(min_linear_bytes) +
                             " bytes, fornecido " + std::to_string(src.pixel_data.size());
            out.pixel_data.clear();
            return out.status;
        }

        for (std::uint32_t y = 0; y < src.height; ++y) {
            const std::uint32_t src_row_byte = y * src.pitch * bpp;
            const std::uint32_t dst_row_byte = y * out_pitch * bpp;

            if (swap_rb && bpp == 4) {
                for (std::uint32_t x = 0; x < src.width; ++x) {
                    const std::uint32_t sb = src_row_byte + x * 4U;
                    const std::uint32_t db = dst_row_byte + x * 4U;
                    out.pixel_data[db + 0] = src.pixel_data[sb + 2]; // B <- R
                    out.pixel_data[db + 1] = src.pixel_data[sb + 1]; // G <- G
                    out.pixel_data[db + 2] = src.pixel_data[sb + 0]; // R <- B
                    out.pixel_data[db + 3] = src.pixel_data[sb + 3]; // A <- A
                }
            } else {
                std::memcpy(&out.pixel_data[dst_row_byte], &src.pixel_data[src_row_byte], src.width * bpp);
            }
        }
    }

    out.valid = true;
    out.width = src.width;
    out.height = src.height;
    out.pitch = out_pitch;
    out.bytes_per_pixel = bpp;
    out.pixel_format = is_fp16 ? 0xC1060000 : ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB;
    out.status = VideoConversionStatus::ok;
    out.diagnostic = "Conversao de video concluida com sucesso (tiling=" +
                     std::to_string(src.tiling_mode) + ", bpp=" + std::to_string(bpp) +
                     ", swap_rb=" + (swap_rb ? "true" : "false") + ")";
    return out.status;
}
}
