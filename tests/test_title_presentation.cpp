#include "check.h"
#include "video_out.h"
#include "pacing_tracker.h"
#include "guest_memory.h"
#include <vector>
#include <chrono>
#include <thread>

int main() {
    return run([] {
        using namespace bb;

        // Aloca 24 MB para comportar double-buffering de quadros 1080p (2 x ~8.3 MB)
        constexpr std::size_t mem_size = 24 * 1024 * 1024;
        GuestMemory memory(mem_size);
        CHECK(memory.map(0x10000, mem_size - 0x10000, read | write) == MemoryError::ok);

        struct PresentationSink : VideoOutSink {
            unsigned presented_flips = 0;
            void on_flip(const FlipEvent&) override {
                ++presented_flips;
            }
        } sink;

        VideoOutManager video_mgr;
        video_mgr.attach_guest_memory(&memory, &sink);

        // Configura framebuffer de título (1920x1080 BGRA8)
        constexpr std::uint32_t width = 1920;
        constexpr std::uint32_t height = 1080;
        constexpr std::uint32_t bpp = 4;
        constexpr std::uint32_t pitch = width;
        const std::size_t frame_bytes = static_cast<std::size_t>(pitch) * height * bpp;

        // Dois buffers de apresentação (double buffering)
        std::uint64_t buf0_addr = 0x20000;
        std::uint64_t buf1_addr = buf0_addr + frame_bytes;
        CHECK(buf1_addr + frame_bytes <= mem_size);

        CHECK(video_mgr.register_buffer(0, VideoBuffer{buf0_addr, width, height, pitch, bpp, 0x80000000, 1}) == VideoStatus::ok);
        CHECK(video_mgr.register_buffer(1, VideoBuffer{buf1_addr, width, height, pitch, bpp, 0x80000000, 1}) == VideoStatus::ok);

        PacingTracker pacing(60);
        HostFrameBuffer host_frame;

        // Simula 30 quadros de apresentação a 30 FPS bloqueados (33.33 ms por quadro)
        constexpr int total_simulated_frames = 30;
        for (int i = 0; i < total_simulated_frames; ++i) {
            pacing.begin_frame();

            // Alterna entre buffers 0 e 1
            int active_buf = i % 2;
            std::uint64_t active_addr = (active_buf == 0) ? buf0_addr : buf1_addr;

            // Preenche cor de fundo com padrão de logo/título Bloodborne (ARGB)
            std::uint32_t clear_color = 0xFF101018 + (i & 0xF);
            CHECK(memory.store(active_addr, &clear_color, sizeof(clear_color)) == MemoryError::ok);

            CHECK(video_mgr.submit_flip(active_buf, i + 1) == VideoStatus::ok);
            CHECK(video_mgr.has_new_frame());

            // Consome quadro na camada de apresentação (equivalente ao MTKView draw)
            CHECK(video_mgr.read_current_frame_pixels(host_frame));
            CHECK(host_frame.valid);
            CHECK(host_frame.width == width);
            CHECK(host_frame.height == height);
            CHECK(host_frame.flip_sequence == static_cast<std::uint64_t>(i + 1));
            CHECK(video_mgr.consume_new_frame());

            // Simula o intervalo de 33.33 ms (30 FPS estáveis)
            pacing.record_frame_ms(TARGET_FRAME_TIME_MS_30FPS);
        }

        CHECK(sink.presented_flips == total_simulated_frames);
        CHECK(video_mgr.flip_count() == total_simulated_frames);

        auto stats = pacing.get_stats();
        CHECK(stats.total_frames >= total_simulated_frames);
        // O p50 deve ser exatamente ~33.33 ms
        CHECK(stats.p50_ms >= 30.0 && stats.p50_ms <= 36.0);
        CHECK(stats.p95_ms >= 30.0 && stats.p95_ms <= 36.0);
        CHECK(pacing.is_on_target_30fps());

        video_mgr.detach_guest_memory();
    });
}
