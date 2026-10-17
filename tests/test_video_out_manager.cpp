#include "check.h"
#include "video_out.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace {
struct Sink : bb::VideoOutSink {
    unsigned flips = 0;
    void on_flip(const bb::FlipEvent&) override { ++flips; }
};
}

int main() { return run([] {
    bb::GuestMemory memory(4096);
    CHECK(memory.map(0x1000, 4096, bb::read | bb::write) == bb::MemoryError::ok);
    Sink sink;
    bb::VideoOutManager manager;
    bb::VideoOutManager capped(17);
    CHECK(capped.register_buffer(16, {0x1000, 2, 2, 2, 4}) == bb::VideoStatus::invalid);
    bb::HostFrameBuffer frame;
    const bb::VideoBuffer buffer{0x1000, 2, 2, 2, 4};
    CHECK(manager.register_buffer(0, buffer) == bb::VideoStatus::ok);
    CHECK(manager.submit_flip(0, 7) == bb::VideoStatus::ok);
    CHECK(manager.has_new_frame());
    CHECK(!manager.read_current_frame_pixels(frame));
    CHECK(!frame.valid && frame.pixel_data.empty()); // modo sintético nunca fabrica pixels
    CHECK(!manager.current_display_buffer().valid);
    CHECK(manager.has_new_frame()); // consultas não consomem o flip
    CHECK(manager.consume_new_frame());
    CHECK(!manager.consume_new_frame());

    manager.attach_guest_memory(&memory, &sink);
    CHECK(manager.flip_count() == 0 && !manager.has_new_frame());
    CHECK(manager.register_buffer(0, buffer) == bb::VideoStatus::ok);
    const std::array<std::uint8_t, 16> pixels{{1, 2, 3, 4, 5, 6, 7, 8,
                                                 9, 10, 11, 12, 13, 14, 15, 16}};
    CHECK(memory.store(0x1000, pixels.data(), pixels.size()) == bb::MemoryError::ok);
    CHECK(manager.submit_flip(0, 9) == bb::VideoStatus::ok);
    CHECK(sink.flips == 1 && manager.flip_count() == 1);
    CHECK(manager.read_current_frame_pixels(frame));
    CHECK(frame.valid && frame.flip_sequence == 1 && frame.pixel_data.size() == pixels.size());
    CHECK(!frame.is_tiled && frame.tiling_mode == 1);
    CHECK(std::equal(frame.pixel_data.begin(), frame.pixel_data.end(), pixels.begin()));
    CHECK(manager.has_new_frame());
    CHECK(memory.protect(0x1000, bb::write) == bb::MemoryError::ok);
    CHECK(!manager.read_current_frame_pixels(frame));
    CHECK(!frame.valid && frame.pixel_data.empty());
    CHECK(!manager.current_display_buffer().valid);
    CHECK(manager.has_new_frame());
    CHECK(manager.consume_new_frame());
    CHECK(memory.protect(0x1000, bb::read | bb::write) == bb::MemoryError::ok);

    // Buffer tiled (tiling_mode = 0) propaga flag is_tiled
    CHECK(manager.unregister_buffer(0) == bb::VideoStatus::ok);
    const bb::VideoBuffer tiled_buffer{0x1000, 2, 2, 2, 4, 0x80000000, 0};
    CHECK(manager.register_buffer(0, tiled_buffer) == bb::VideoStatus::ok);
    CHECK(manager.submit_flip(0, 11) == bb::VideoStatus::ok);
    CHECK(manager.read_current_frame_pixels(frame));
    CHECK(frame.valid && frame.is_tiled && frame.tiling_mode == 0 && frame.pixel_format == 0x80000000);
    CHECK(manager.consume_new_frame());
    // Pitch é contado em pixels, não em bytes: a leitura inclui padding
    // entre linhas, e a apresentação BGRA8 usa pitch * 4 por linha.
    CHECK(manager.register_buffer(1, {0x1100, 2, 2, 3, 4}) == bb::VideoStatus::ok);
    const std::array<std::uint8_t, 24> padded{{
        1, 2, 3, 4, 5, 6, 7, 8, 0xaa, 0xbb, 0xcc, 0xdd,
        9, 10, 11, 12, 13, 14, 15, 16, 0xee, 0xff, 0x17, 0x18
    }};
    CHECK(memory.store(0x1100, padded.data(), padded.size()) == bb::MemoryError::ok);
    CHECK(manager.submit_flip(1, 10) == bb::VideoStatus::ok);
    CHECK(manager.read_current_frame_pixels(frame));
    CHECK(frame.pitch == 3 && frame.bytes_per_pixel == 4 && frame.pixel_data.size() == 24);
    CHECK(std::equal(frame.pixel_data.begin(), frame.pixel_data.end(), padded.begin()));
    CHECK(manager.consume_new_frame());
    manager.detach_guest_memory();
    CHECK(!manager.has_guest_memory() && !manager.has_new_frame());
    CHECK(manager.flip_count() == 0 && manager.last_flip().sequence == 0);
    CHECK(manager.buffer(0) == nullptr && !manager.current_display_buffer().valid);

    CHECK(manager.register_buffer(1, {0, 4096, 4096, 4096, 4}) == bb::VideoStatus::ok); // 64 MiB
    CHECK(manager.register_buffer(2, {0, 4097, 4096, 4096, 4}) == bb::VideoStatus::invalid);
    CHECK(manager.register_buffer(2, {0, 4097, 4096, 4097, 4}) == bb::VideoStatus::overflow);
    CHECK(manager.register_buffer(2, {UINT64_MAX - 1, 2, 2, 2, 4}) == bb::VideoStatus::overflow);
    CHECK(manager.submit_flip(1, 0) == bb::VideoStatus::ok);
    CHECK(!manager.current_display_buffer().valid);

    // =========================================================================
    // T167: Conversão de Formatos e Des-Tiling (Display2DThin e Morton 8x8)
    // =========================================================================
    {
        bb::GuestMemory conv_mem(64 * 1024);
        CHECK(conv_mem.map(0x2000, 32 * 1024, bb::read | bb::write) == bb::MemoryError::ok);
        Sink conv_sink;
        bb::VideoOutManager conv_mgr;
        conv_mgr.attach_guest_memory(&conv_mem, &conv_sink);

        // 1. Rejeição de memória desanexada / nula com diagnóstico
        bb::PresentationFrame pres;
        bb::VideoOutManager empty_mgr;
        CHECK(!empty_mgr.read_presentation_frame(pres));
        CHECK(!pres.valid && pres.status == bb::VideoConversionStatus::unmapped_memory);
        CHECK(!pres.diagnostic.empty());
        CHECK(empty_mgr.conversion_failure_count() == 1);
        CHECK(empty_mgr.last_conversion_status() == bb::VideoConversionStatus::unmapped_memory);

        // 2. Linear BGRA8 com padding de pitch
        // Imagem 4x2 com pitch=5 (4 pixels válidos + 1 pixel padding por linha)
        const bb::VideoBuffer linear_bgra{0x2000, 4, 2, 5, 4, bb::ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB, 1};
        CHECK(conv_mgr.register_buffer(0, linear_bgra) == bb::VideoStatus::ok);

        // Linha 0: (10, 20, 30, 255), (11, 21, 31, 255), (12, 22, 32, 255), (13, 23, 33, 255), pad(0)
        // Linha 1: (40, 50, 60, 255), (41, 51, 61, 255), (42, 52, 62, 255), (43, 53, 63, 255), pad(0)
        std::vector<std::uint8_t> bgra_data(5 * 2 * 4, 0);
        for (std::uint32_t y = 0; y < 2; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::size_t off = (y * 5 + x) * 4;
                bgra_data[off + 0] = static_cast<std::uint8_t>(10 + y * 30 + x); // B
                bgra_data[off + 1] = static_cast<std::uint8_t>(20 + y * 30 + x); // G
                bgra_data[off + 2] = static_cast<std::uint8_t>(30 + y * 30 + x); // R
                bgra_data[off + 3] = 255;                                         // A
            }
        }
        CHECK(conv_mem.store(0x2000, bgra_data.data(), bgra_data.size()) == bb::MemoryError::ok);
        CHECK(conv_mgr.submit_flip(0, 100) == bb::VideoStatus::ok);

        CHECK(conv_mgr.read_presentation_frame(pres));
        CHECK(pres.valid && pres.width == 4 && pres.height == 2 && pres.pitch == 4);
        CHECK(!pres.was_tiled && pres.pixel_format == bb::ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB);
        CHECK(pres.pixel_data.size() == 4 * 2 * 4);
        CHECK(conv_mgr.conversion_success_count() == 1);
        CHECK(conv_mgr.last_conversion_status() == bb::VideoConversionStatus::ok);

        // Verificar compactação do pitch linear (sem o pixel padding)
        for (std::uint32_t y = 0; y < 2; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::size_t dst_off = (y * 4 + x) * 4;
                CHECK(pres.pixel_data[dst_off + 0] == static_cast<std::uint8_t>(10 + y * 30 + x));
                CHECK(pres.pixel_data[dst_off + 1] == static_cast<std::uint8_t>(20 + y * 30 + x));
                CHECK(pres.pixel_data[dst_off + 2] == static_cast<std::uint8_t>(30 + y * 30 + x));
                CHECK(pres.pixel_data[dst_off + 3] == 255);
            }
        }

        // 3. Linear RGBA8 -> BGRA8 (swap R <-> B)
        CHECK(conv_mgr.unregister_buffer(0) == bb::VideoStatus::ok);
        const bb::VideoBuffer linear_rgba{0x2000, 2, 2, 2, 4, bb::ORBIS_VIDEO_OUT_PIXEL_FORMAT_R8_G8_B8_A8_SRGB, 1};
        CHECK(conv_mgr.register_buffer(0, linear_rgba) == bb::VideoStatus::ok);

        // Pixels RGBA:
        // (R=0xAA, G=0xBB, B=0xCC, A=0xFF) -> deve converter para BGRA (0xCC, 0xBB, 0xAA, 0xFF)
        const std::array<std::uint8_t, 16> rgba_raw{{
            0xAA, 0xBB, 0xCC, 0xFF,   0x11, 0x22, 0x33, 0xFF,
            0x44, 0x55, 0x66, 0xFF,   0x77, 0x88, 0x99, 0xFF
        }};
        CHECK(conv_mem.store(0x2000, rgba_raw.data(), rgba_raw.size()) == bb::MemoryError::ok);
        CHECK(conv_mgr.submit_flip(0, 101) == bb::VideoStatus::ok);

        CHECK(conv_mgr.read_presentation_frame(pres));
        CHECK(pres.valid && pres.width == 2 && pres.height == 2);
        CHECK(pres.pixel_data[0] == 0xCC && pres.pixel_data[1] == 0xBB && pres.pixel_data[2] == 0xAA && pres.pixel_data[3] == 0xFF);
        CHECK(pres.pixel_data[4] == 0x33 && pres.pixel_data[5] == 0x22 && pres.pixel_data[6] == 0x11 && pres.pixel_data[7] == 0xFF);
        CHECK(conv_mgr.conversion_success_count() == 2);

        // 4. Layout Tiled Display2DThin (micro-tile 8x8)
        // Criar imagem 16x8 (2 micro-tiles horizontais de 8x8)
        CHECK(conv_mgr.unregister_buffer(0) == bb::VideoStatus::ok);
        const bb::VideoBuffer tiled_buf{0x2000, 16, 8, 16, 4, bb::ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB, 0};
        CHECK(conv_mgr.register_buffer(0, tiled_buf) == bb::VideoStatus::ok);

        // Preencher 2 tiles de 8x8 pixels em layout Display2DThin
        // Cada tile tem 64 pixels contíguos ordenados por scanline local (ly * 8 + lx)
        // Tile 0 cobre x in [0..7], y in [0..7]
        // Tile 1 cobre x in [8..15], y in [0..7]
        std::vector<std::uint8_t> tiled_raw(16 * 8 * 4, 0);
        for (std::uint32_t ty = 0; ty < 1; ++ty) {
            for (std::uint32_t tx = 0; tx < 2; ++tx) {
                const std::uint32_t tile_idx = ty * 2 + tx;
                for (std::uint32_t ly = 0; ly < 8; ++ly) {
                    for (std::uint32_t lx = 0; lx < 8; ++lx) {
                        const std::uint32_t global_x = tx * 8 + lx;
                        const std::uint32_t global_y = ty * 8 + ly;
                        const std::size_t tiled_pixel = tile_idx * 64 + (ly * 8 + lx);
                        const std::size_t byte_off = tiled_pixel * 4;

                        // Valor determinístico derivado de (global_x, global_y)
                        tiled_raw[byte_off + 0] = static_cast<std::uint8_t>(global_x);      // B
                        tiled_raw[byte_off + 1] = static_cast<std::uint8_t>(global_y);      // G
                        tiled_raw[byte_off + 2] = static_cast<std::uint8_t>(global_x + 50); // R
                        tiled_raw[byte_off + 3] = 255;                                      // A
                    }
                }
            }
        }
        CHECK(conv_mem.store(0x2000, tiled_raw.data(), tiled_raw.size()) == bb::MemoryError::ok);
        CHECK(conv_mgr.submit_flip(0, 102) == bb::VideoStatus::ok);

        CHECK(conv_mgr.read_presentation_frame(pres));
        CHECK(pres.valid && pres.was_tiled && pres.width == 16 && pres.height == 8);
        CHECK(pres.pixel_data.size() == 16 * 8 * 4);

        // Conferir se o des-tiling reconstruiu a imagem linear perfeitamente
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 16; ++x) {
                const std::size_t lin_off = (y * 16 + x) * 4;
                CHECK(pres.pixel_data[lin_off + 0] == static_cast<std::uint8_t>(x));
                CHECK(pres.pixel_data[lin_off + 1] == static_cast<std::uint8_t>(y));
                CHECK(pres.pixel_data[lin_off + 2] == static_cast<std::uint8_t>(x + 50));
                CHECK(pres.pixel_data[lin_off + 3] == 255);
            }
        }

        // 5. Des-tiling Morton 8x8 autoral
        {
            std::vector<std::uint8_t> morton_src(8 * 8 * 4, 0);
            for (std::uint32_t ly = 0; ly < 8; ++ly) {
                for (std::uint32_t lx = 0; lx < 8; ++lx) {
                    const std::uint32_t morton_idx =
                        (lx & 1U) | ((ly & 1U) << 1U) |
                        (((lx >> 1U) & 1U) << 2U) | (((ly >> 1U) & 1U) << 3U) |
                        (((lx >> 2U) & 1U) << 4U) | (((ly >> 2U) & 1U) << 5U);
                    const std::size_t off = morton_idx * 4;
                    morton_src[off + 0] = static_cast<std::uint8_t>(lx * 10);
                    morton_src[off + 1] = static_cast<std::uint8_t>(ly * 10);
                    morton_src[off + 2] = 128;
                    morton_src[off + 3] = 255;
                }
            }
            std::vector<std::uint8_t> morton_dst(8 * 8 * 4, 0);
            CHECK(bb::VideoFrameConverter::detile_morton_8x8(morton_src.data(), morton_dst.data(), 8, 8, 8, 8, false));
            for (std::uint32_t y = 0; y < 8; ++y) {
                for (std::uint32_t x = 0; x < 8; ++x) {
                    const std::size_t off = (y * 8 + x) * 4;
                    CHECK(morton_dst[off + 0] == static_cast<std::uint8_t>(x * 10));
                    CHECK(morton_dst[off + 1] == static_cast<std::uint8_t>(y * 10));
                    CHECK(morton_dst[off + 2] == 128);
                    CHECK(morton_dst[off + 3] == 255);
                }
            }
        }

        // 6. Rejeição explícita de Formato de Vídeo não suportado com diagnóstico
        CHECK(conv_mgr.unregister_buffer(0) == bb::VideoStatus::ok);
        const bb::VideoBuffer hdr_buf{0x2000, 4, 4, 4, 4, 0x80000002, 1}; // R10G10B10A2
        CHECK(conv_mgr.register_buffer(0, hdr_buf) == bb::VideoStatus::ok);
        CHECK(conv_mgr.submit_flip(0, 103) == bb::VideoStatus::ok);
        CHECK(!conv_mgr.read_presentation_frame(pres));
        CHECK(!pres.valid && pres.status == bb::VideoConversionStatus::unsupported_format);
        CHECK(pres.diagnostic.find("0x80000002") != std::string::npos);
        CHECK(conv_mgr.last_conversion_status() == bb::VideoConversionStatus::unsupported_format);

        // 7. Rejeição explícita de Modo de Tiling desconhecido com diagnóstico
        CHECK(conv_mgr.unregister_buffer(0) == bb::VideoStatus::ok);
        bb::HostFrameBuffer bad_tiling_frame;
        bad_tiling_frame.valid = true;
        bad_tiling_frame.width = 4;
        bad_tiling_frame.height = 4;
        bad_tiling_frame.pitch = 4;
        bad_tiling_frame.bytes_per_pixel = 4;
        bad_tiling_frame.pixel_format = bb::ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB;
        bad_tiling_frame.tiling_mode = 3; // Modo de tiling inválido
        bad_tiling_frame.pixel_data.resize(4 * 4 * 4, 0);

        bb::PresentationFrame bad_tiling_pres;
        CHECK(bb::VideoFrameConverter::convert(bad_tiling_frame, bad_tiling_pres) == bb::VideoConversionStatus::unsupported_tiling);
        CHECK(!bad_tiling_pres.valid && bad_tiling_pres.diagnostic.find("Modo de tiling 3") != std::string::npos);

        // 8. Rejeição de buffer truncado / pequeno
        bb::HostFrameBuffer small_frame = bad_tiling_frame;
        small_frame.tiling_mode = 1;
        small_frame.pixel_data.resize(10); // Menos que 4 * 4 * 4 = 64 bytes
        bb::PresentationFrame small_pres;
        CHECK(bb::VideoFrameConverter::convert(small_frame, small_pres) == bb::VideoConversionStatus::buffer_too_small);
        CHECK(!small_pres.valid && small_pres.diagnostic.find("insuficiente") != std::string::npos);

        // 9. T167: Round-Trip Display2DThin e Morton 8x8 (Linear -> Tiled -> Linear)
        {
            constexpr std::uint32_t w = 16, h = 16;
            std::vector<std::uint8_t> linear_orig(w * h * 4);
            for (std::uint32_t i = 0; i < w * h * 4; ++i) {
                linear_orig[i] = static_cast<std::uint8_t>((i * 7 + 13) & 0xFF);
            }
            std::vector<std::uint8_t> tiled_buf(w * h * 4, 0);
            std::vector<std::uint8_t> linear_restored(w * h * 4, 0);

            // Display2DThin round-trip
            CHECK(bb::VideoFrameConverter::tile_display2d_thin(linear_orig.data(), tiled_buf.data(), w, h, w, w, false));
            CHECK(bb::VideoFrameConverter::detile_display2d_thin(tiled_buf.data(), linear_restored.data(), w, h, w, w, false));
            CHECK(std::equal(linear_orig.begin(), linear_orig.end(), linear_restored.begin()));

            // Morton 8x8 round-trip
            std::fill(tiled_buf.begin(), tiled_buf.end(), 0);
            std::fill(linear_restored.begin(), linear_restored.end(), 0);
            CHECK(bb::VideoFrameConverter::tile_morton_8x8(linear_orig.data(), tiled_buf.data(), w, h, w, w, false));
            CHECK(bb::VideoFrameConverter::detile_morton_8x8(tiled_buf.data(), linear_restored.data(), w, h, w, w, false));
            CHECK(std::equal(linear_orig.begin(), linear_orig.end(), linear_restored.begin()));
        }

        // 10. T167: Formatos adicionais e RGBA16Float (8 bpp)
        {
            CHECK(bb::VideoFrameConverter::is_supported_pixel_format(bb::ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB));
            CHECK(bb::VideoFrameConverter::is_supported_pixel_format(0x82000000));
            CHECK(bb::VideoFrameConverter::is_supported_pixel_format(0x84000000));
            CHECK(bb::VideoFrameConverter::is_supported_pixel_format(0xA2000000));
            CHECK(bb::VideoFrameConverter::is_supported_pixel_format(0xC1060000));
            CHECK(!bb::VideoFrameConverter::is_supported_pixel_format(0x99999999));
            CHECK(bb::VideoFrameConverter::get_pixel_format_bpp(0xC1060000) == 8);
            CHECK(bb::VideoFrameConverter::get_pixel_format_bpp(0x80000000) == 4);

            // Conversão de buffer RGBA16Float (8 bpp) Linear
            bb::HostFrameBuffer fp16_frame;
            fp16_frame.valid = true;
            fp16_frame.width = 4;
            fp16_frame.height = 4;
            fp16_frame.pitch = 4;
            fp16_frame.bytes_per_pixel = 8;
            fp16_frame.pixel_format = 0xC1060000;
            fp16_frame.tiling_mode = 1;
            fp16_frame.pixel_data.resize(4 * 4 * 8);
            for (std::size_t i = 0; i < fp16_frame.pixel_data.size(); ++i) {
                fp16_frame.pixel_data[i] = static_cast<std::uint8_t>(i & 0xFF);
            }

            bb::PresentationFrame fp16_pres;
            CHECK(bb::VideoFrameConverter::convert(fp16_frame, fp16_pres) == bb::VideoConversionStatus::ok);
            CHECK(fp16_pres.valid && fp16_pres.bytes_per_pixel == 8 && fp16_pres.pixel_format == 0xC1060000);
            CHECK(fp16_pres.pixel_data.size() == 4 * 4 * 8);
            CHECK(std::equal(fp16_frame.pixel_data.begin(), fp16_frame.pixel_data.end(), fp16_pres.pixel_data.begin()));

            // Conversão de 0x82000000 (RGBA8 com swap_rb)
            bb::HostFrameBuffer rgba_ext_frame;
            rgba_ext_frame.valid = true;
            rgba_ext_frame.width = 2;
            rgba_ext_frame.height = 2;
            rgba_ext_frame.pitch = 2;
            rgba_ext_frame.bytes_per_pixel = 4;
            rgba_ext_frame.pixel_format = 0x82000000;
            rgba_ext_frame.tiling_mode = 1;
            rgba_ext_frame.pixel_data = {
                10, 20, 30, 255,  40, 50, 60, 255,
                70, 80, 90, 255,  11, 22, 33, 255
            };
            bb::PresentationFrame rgba_ext_pres;
            CHECK(bb::VideoFrameConverter::convert(rgba_ext_frame, rgba_ext_pres) == bb::VideoConversionStatus::ok);
            CHECK(rgba_ext_pres.valid && rgba_ext_pres.pixel_data[0] == 30 && rgba_ext_pres.pixel_data[2] == 10);
        }
    }
}); }
