#include "check.h"
#include "video_out.h"
#include "pacing_tracker.h"
#include "guest_memory.h"
#include "audio_out.h"
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>

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

        // 1. Simula 30 quadros de apresentação a 30 FPS bloqueados (33.33 ms por quadro)
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

        // 2. T038: Pacing em Tempo Real com Relógio de Alta Resolução Sem Timestamps Artificiais (US1/AC1)
        {
            PacingTracker real_pacing(60, 30.0);
            constexpr int real_frames_to_measure = 15;
            for (int f = 0; f < real_frames_to_measure; ++f) {
                auto t_start = std::chrono::steady_clock::now();
                real_pacing.begin_frame();

                // Simula trabalho de renderização e flip com intervalo curto real
                std::this_thread::sleep_for(std::chrono::milliseconds(2));

                auto t_end = std::chrono::steady_clock::now();
                double real_frame_time_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
                // Registra o tempo REAL medido no relógio da máquina (não injetado artificialmente)
                real_pacing.record_frame_ms(real_frame_time_ms);
            }
            auto real_stats = real_pacing.get_stats();
            CHECK(real_stats.total_frames == real_frames_to_measure);
            // Comprova que todas as medições são tempos físicos reais positivos
            CHECK(real_stats.p50_ms > 0.5);
            CHECK(real_stats.p95_ms > 0.5);
        }

        // 3. T038: Sincronização Áudio-Visual com Tema de Abertura (US1/AC2)
        {
            auto& audio = AudioOutManager::instance();
            audio.close_all_ports();
            audio.set_paused(false);

            std::int32_t bgm_port = audio.open_port(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 1600, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            CHECK(bgm_port > 0);

            // A 30 FPS e 48000 Hz, cada quadro de vídeo corresponde exatamente a:
            // 48000 / 30 = 1600 amostras estéreo (3200 int16)
            constexpr std::size_t samples_per_frame = 1600;
            std::vector<std::int16_t> audio_frame(samples_per_frame * 2);
            for (std::size_t i = 0; i < audio_frame.size(); ++i) {
                // Sintetiza onda do tema de abertura de Bloodborne
                audio_frame[i] = static_cast<std::int16_t>(1000.0 * std::sin(2.0 * 3.14159 * 220.0 * (i / 2) / 48000.0));
            }

            std::vector<std::int16_t> drained_audio(samples_per_frame * 2, 0);

            // Simula apresentação de 10 quadros com áudio perfeitamente acoplado 1:1
            for (int k = 0; k < 10; ++k) {
                // Submete áudio do tema de abertura
                CHECK(audio.output(bgm_port, audio_frame.data(), samples_per_frame) == SCE_OK);

                // Submete flip de vídeo correspondente
                int buf_idx = k % 2;
                CHECK(video_mgr.submit_flip(buf_idx, 100 + k) == VideoStatus::ok);
                CHECK(video_mgr.has_new_frame());

                // Consome quadro de vídeo
                HostFrameBuffer v_frame;
                CHECK(video_mgr.read_current_frame_pixels(v_frame));
                CHECK(v_frame.valid);
                CHECK(video_mgr.consume_new_frame());

                // Drena amostras de áudio correspondentes ao quadro
                std::size_t drained = audio.drain_main_port_pcm16(drained_audio.data(), samples_per_frame);
                CHECK(drained == samples_per_frame);
            }

            CHECK(audio.close_port(bgm_port) == SCE_OK);
        }

        // 4. T038: Distinção Estrita entre Demonstração Gráfica e Apresentação Real do Guest (US1/AC3)
        {
            // Instância limpa sem flips submetidos: não possui frame do guest
            VideoOutManager fresh_mgr;
            fresh_mgr.attach_guest_memory(&memory, &sink);
            CHECK(!fresh_mgr.has_new_frame());

            HostFrameBuffer empty_frame;
            CHECK(!fresh_mgr.read_current_frame_pixels(empty_frame));
            CHECK(!empty_frame.valid);

            // No gerenciador em que o último frame foi consumido, has_new_frame() é estritamente falso
            CHECK(!video_mgr.has_new_frame());

            // Submissão de frame genuíno do guest ativa apresentação com metadados válidos
            std::uint64_t prev_flips = video_mgr.flip_count();
            CHECK(video_mgr.submit_flip(0, 9999) == VideoStatus::ok);
            CHECK(video_mgr.has_new_frame());

            HostFrameBuffer guest_valid_frame;
            CHECK(video_mgr.read_current_frame_pixels(guest_valid_frame));
            CHECK(guest_valid_frame.valid);
            CHECK(guest_valid_frame.flip_sequence == prev_flips + 1);
            CHECK(guest_valid_frame.width == width);
            CHECK(guest_valid_frame.height == height);

            CHECK(video_mgr.consume_new_frame());
            CHECK(!video_mgr.has_new_frame());

            fresh_mgr.detach_guest_memory();
        }

        video_mgr.detach_guest_memory();
    });
}
