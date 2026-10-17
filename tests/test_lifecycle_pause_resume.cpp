#include "check.h"
#include "game_boot_coordinator.h"
#include "audio_out.h"
#include "guest_scheduler.h"
#include "save_data.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>

int main() {
    return run([] {
        using namespace bb;

        GameBootCoordinator coord;
        CHECK(!coord.is_paused());

        // 1. Simulação de ciclo de vida: pausa ao ir para segundo plano (sceneDidEnterBackground)
        coord.pause();
        CHECK(coord.is_paused());
        CHECK(AudioOutManager::instance().is_paused());

        // Durante a pausa, chamadas a step() retornam 0 passos sem consumir CPU
        auto t0 = std::chrono::high_resolution_clock::now();
        std::uint64_t executed_steps = coord.step(10000);
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        CHECK(executed_steps == 0);
        CHECK(elapsed_us < 5000.0); // Retorno praticamente instantâneo (< 5 ms)

        // 2. Simulação de retorno ao primeiro plano (sceneWillEnterForeground)
        coord.resume();
        CHECK(!coord.is_paused());
        CHECK(!AudioOutManager::instance().is_paused());

        // 3. Teste de ciclo de vida do escalonador cooperativo
        GuestMemory mem(16 * 1024 * 1024);
        mem.map(0x1000, 0x1000, Permission::read | Permission::write);
        mem.map(0x4000, 0x1000, Permission::read | Permission::write);
        const std::uint8_t loop_code[] = { 0x90, 0xeb, 0xfe }; // nop; jmp $
        mem.store(0x1000, loop_code, sizeof(loop_code));

        GuestScheduler sched(mem);
        GuestScheduler::Id tid = sched.create(0x1000, 0x4000, 0x5000, 0x2000);
        CHECK(tid > 0);
        CHECK(sched.start(tid) == ORBIS_OK);
        CHECK(sched.state(tid) == GuestScheduler::State::ready);

        // Executa alguns passos no escalonador
        auto res = sched.run(10);
        CHECK(res.steps == 10);

        // Coloca a thread para dormir (estado waiting)
        CHECK(sched.sleep(tid, 500) == ORBIS_OK);
        CHECK(sched.state(tid) == GuestScheduler::State::waiting);

        // Retomada da thread
        sched.advance(600);
        CHECK(sched.resume(tid) == ORBIS_OK);
        CHECK(sched.state(tid) == GuestScheduler::State::ready);

        // 4. Verificação de supressão de áudio durante a pausa
        AudioOutManager& audio = AudioOutManager::instance();
        std::int32_t port = audio.open_port(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(port > 0);

        std::vector<std::int16_t> test_samples(512, 1234);
        CHECK(sceAudioOutOutput(port, test_samples.data()) == SCE_OK);

        coord.pause();
        std::vector<std::int16_t> drained_samples(512, 0);
        std::size_t drained = audio.drain_main_port_pcm16(drained_samples.data(), 256);
        CHECK(drained == 0); // Em pausa, drenagem de áudio é zerada

        coord.resume();
        drained = audio.drain_main_port_pcm16(drained_samples.data(), 256);
        CHECK(drained == 256);

        CHECK(sceAudioOutClose(port) == SCE_OK);

        // 5. Teste de pausa cooperativa durante um run() / step() em curso (T044)
        {
            GuestMemory pause_mem(16 * 1024 * 1024);
            pause_mem.map(0x1000, 0x1000, Permission::read | Permission::write);
            pause_mem.map(0x4000, 0x1000, Permission::read | Permission::write);
            const std::uint8_t inf_loop[] = { 0x90, 0xeb, 0xfe }; // nop; jmp $
            pause_mem.store(0x1000, inf_loop, sizeof(inf_loop));

            GuestScheduler pause_sched(pause_mem);
            GuestScheduler::Id loop_tid = pause_sched.create(0x1000, 0x4000, 0x5000, 0x2000);
            CHECK(loop_tid > 0);
            CHECK(pause_sched.start(loop_tid) == ORBIS_OK);

            std::atomic<bool> pause_flag{false};
            pause_sched.set_pause_checker([&]() { return pause_flag.load(std::memory_order_relaxed); });

            std::atomic<bool> thread_started{false};
            std::atomic<std::uint64_t> executed_steps{0};
            std::thread worker([&]() {
                thread_started.store(true, std::memory_order_release);
                auto res = pause_sched.run(50000000); // 50M steps solicitados
                executed_steps.store(res.steps, std::memory_order_release);
            });

            while (!thread_started.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pause_flag.store(true, std::memory_order_release);
            worker.join();

            // A thread encerrou cooperativamente muito antes de atingir 50M de passos
            CHECK(executed_steps.load(std::memory_order_acquire) < 50000000);
            CHECK(executed_steps.load(std::memory_order_acquire) > 0);
        }

        // 6. Teste de tratamento de terminação do app (UIApplicationWillTerminate) (T044)
        {
            const std::string term_test_dir = "build-Release/test_lifecycle_term_dir";
            std::error_code ec;
            std::filesystem::remove_all(term_test_dir, ec);
            std::filesystem::create_directories(term_test_dir, ec);

            SaveDataManager sm;
            CHECK(sm.initialize(term_test_dir, 1024 * 1024));
            sm.set_default_title_id("CUSA03173");

            std::string mount_pt;
            uint32_t status = 0;
            CHECK(sm.mount(0, "CUSA03173", "SPRJ0001",
                           SCE_SAVEDATA_MOUNT_MODE_RDWR | SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                           4096, mount_pt, status) == SCE_SAVEDATA_OK);

            std::vector<uint8_t> progress_payload = { 0x10, 0x20, 0x30, 0x40 };
            CHECK(sm.write_file(mount_pt, "progress.dat", progress_payload.data(), progress_payload.size(), true) == SCE_SAVEDATA_OK);

            // Simulação de terminação abrupta controlada do app:
            // Pausa do coordenador, commit/umount de todos os slots ativos e desligamento
            coord.pause();
            CHECK(coord.is_paused());

            for (std::size_t i = 0; i < SaveDataManager::MAX_SLOTS; ++i) {
                const auto *slot = sm.get_slot(static_cast<int>(i));
                if (slot && slot->used && !slot->read_only) {
                    CHECK(sm.umount(slot->mount_point, true) == SCE_SAVEDATA_OK);
                }
            }
            sm.terminate();

            // Verificação pós-terminação: arquivo persistido íntegro no diretório host
            SaveDataManager sm_check;
            CHECK(sm_check.initialize(term_test_dir, 1024 * 1024));
            sm_check.set_default_title_id("CUSA03173");
            std::string mount_check;
            uint32_t st_check = 0;
            CHECK(sm_check.mount(0, "CUSA03173", "SPRJ0001", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, mount_check, st_check) == SCE_SAVEDATA_OK);
            CHECK(st_check == SCE_SAVEDATA_MOUNT_STATUS_EXISTING);

            std::vector<uint8_t> readback;
            CHECK(sm_check.read_file(mount_check, "progress.dat", readback) == SCE_SAVEDATA_OK);
            CHECK(readback == progress_payload);
            CHECK(sm_check.umount(mount_check, false) == SCE_SAVEDATA_OK);
            sm_check.terminate();

            std::filesystem::remove_all(term_test_dir, ec);
        }

        // 7. T112: Distinção de motivos de pausa (User, Lifecycle, Disconnect) e corrida boot/background (FR-010)
        {
            GameBootCoordinator pause_coord;
            CHECK(!pause_coord.is_paused());
            CHECK(pause_coord.pause_reasons() == 0);

            // 7a. Pausa manual do usuário (PauseReason::user)
            pause_coord.add_pause_reason(PauseReason::user);
            CHECK(pause_coord.is_paused());
            CHECK(pause_coord.has_pause_reason(PauseReason::user));

            // Simula transição foreground do ciclo de vida tentando retomar
            // A pausa do usuário DEVE ser preservada (não retoma!)
            pause_coord.clear_pause_reason(PauseReason::lifecycle);
            CHECK(pause_coord.is_paused());
            CHECK(pause_coord.has_pause_reason(PauseReason::user));
            CHECK(pause_coord.step(100) == 0);

            // 7b. Acúmulo de múltiplos motivos de pausa: Usuário + Desconexão
            pause_coord.add_pause_reason(PauseReason::disconnect);
            CHECK(pause_coord.is_paused());
            CHECK(pause_coord.has_pause_reason(PauseReason::user));
            CHECK(pause_coord.has_pause_reason(PauseReason::disconnect));

            // Usuário retoma (limpa PauseReason::user), mas controle ainda desconectado: permanece pausado
            pause_coord.clear_pause_reason(PauseReason::user);
            CHECK(pause_coord.is_paused());
            CHECK(!pause_coord.has_pause_reason(PauseReason::user));
            CHECK(pause_coord.has_pause_reason(PauseReason::disconnect));

            // Controle reconecta: agora todos os motivos foram limpos, coordenador retoma
            pause_coord.clear_pause_reason(PauseReason::disconnect);
            CHECK(!pause_coord.is_paused());
            CHECK(pause_coord.pause_reasons() == 0);

            // 7c. Corrida Boot / Background:
            // Boot conclui enquanto app está em background (PauseReason::lifecycle)
            pause_coord.add_pause_reason(PauseReason::lifecycle);
            CHECK(pause_coord.is_paused());
            // Execução de passos de CPU do guest em background é estritamente bloqueada
            CHECK(pause_coord.step(1000) == 0);

            // App retorna para o foreground: limpa lifecycle e retoma
            pause_coord.clear_pause_reason(PauseReason::lifecycle);
            CHECK(!pause_coord.is_paused());
        }

        // 8. T049: Desacoplamento da CPU Guest do ciclo de desenho da UI e pausa cooperativa (SC-001)
        {
            GuestMemory cpu_mem(16 * 1024 * 1024);
            cpu_mem.map(0x1000, 0x1000, Permission::read | Permission::write);
            cpu_mem.map(0x4000, 0x1000, Permission::read | Permission::write);
            // Instruções canônicas de loop da CPU guest: nop; jmp $
            const std::uint8_t loop_code[] = { 0x90, 0xeb, 0xfe };
            cpu_mem.store(0x1000, loop_code, sizeof(loop_code));

            GuestScheduler cpu_sched(cpu_mem);
            GuestScheduler::Id loop_tid = cpu_sched.create(0x1000, 0x4000, 0x5000, 0x2000);
            CHECK(loop_tid > 0);
            CHECK(cpu_sched.start(loop_tid) == ORBIS_OK);

            std::atomic<bool> worker_running{true};
            std::atomic<bool> pause_cpu{false};
            std::atomic<std::uint64_t> total_cpu_steps{0};
            std::atomic<std::uint32_t> loop_iterations{0};

            cpu_sched.set_pause_checker([&]() {
                return pause_cpu.load(std::memory_order_relaxed);
            });

            // Thread trabalhadora simulando o dispatch queue _guestExecutionQueue
            std::thread cpu_worker([&]() {
                while (worker_running.load(std::memory_order_acquire)) {
                    if (!pause_cpu.load(std::memory_order_relaxed)) {
                        auto res = cpu_sched.run(1000);
                        total_cpu_steps.fetch_add(res.steps, std::memory_order_relaxed);
                        loop_iterations.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            });

            struct AutoJoiner {
                std::atomic<bool>& running;
                std::thread& th;
                ~AutoJoiner() {
                    running.store(false, std::memory_order_release);
                    if (th.joinable()) th.join();
                }
            } joiner{worker_running, cpu_worker};

            // Simula thread principal de UI executando ciclos de renderização concorrentemente
            std::atomic<std::uint32_t> ui_frames_drawn{0};
            double max_ui_latency_ms = 0.0;
            for (int f = 0; f < 30; ++f) {
                auto ui_t0 = std::chrono::high_resolution_clock::now();
                ui_frames_drawn.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                auto ui_t1 = std::chrono::high_resolution_clock::now();
                double ui_elapsed_ms = std::chrono::duration<double, std::milli>(ui_t1 - ui_t0).count();
                if (ui_elapsed_ms > max_ui_latency_ms) {
                    max_ui_latency_ms = ui_elapsed_ms;
                }
            }

            // Comprova que o loop de UI desenhou continuamente sem travamento (< 50 ms por frame)
            CHECK(ui_frames_drawn.load() == 30);
            CHECK(max_ui_latency_ms < 50.0);
            // Comprova que a CPU guest avançou concorrentemente de forma assíncrona
            CHECK(total_cpu_steps.load() > 5000);
            CHECK(loop_iterations.load() > 0);

            // Simula acionamento de pausa cooperativa (PauseReasonUser / Lifecycle)
            pause_cpu.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::uint64_t steps_after_pause = total_cpu_steps.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            // Nenhum passo adicional é executado durante a pausa
            CHECK(total_cpu_steps.load(std::memory_order_acquire) == steps_after_pause);

            // Retoma a execução
            pause_cpu.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK(total_cpu_steps.load(std::memory_order_acquire) > steps_after_pause);
        }

        // 9. T050: Tratamento de interrupção AVAudioSession e reinício sem estalos ou áudio preso (US5/AC2)
        {
            auto& audio_mgr = AudioOutManager::instance();
            audio_mgr.close_all_ports();
            CHECK(!audio_mgr.is_paused());

            std::int32_t p_main = audio_mgr.open_port(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            std::int32_t p_bgm = audio_mgr.open_port(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            CHECK(p_main > 0 && p_bgm > 0);

            // Submete blocos de áudio ativos nas duas portas
            std::vector<std::int16_t> main_samples(256 * 2, 2000);
            std::vector<std::int16_t> bgm_samples(256 * 2, 1000);
            CHECK(audio_mgr.output(p_main, main_samples.data(), 256) == SCE_OK);
            CHECK(audio_mgr.output(p_bgm, bgm_samples.data(), 256) == SCE_OK);

            // 9a. Simula AVAudioSessionInterruptionTypeBegan: pausa e purga de buffers
            audio_mgr.set_paused(true);
            audio_mgr.purge_inactive_buffers();
            CHECK(audio_mgr.is_paused());

            // Tentativa de saída durante interrupção é absorvida sem erro nem acúmulo
            CHECK(audio_mgr.output(p_main, main_samples.data(), 256) == SCE_OK);
            CHECK(audio_mgr.output(p_bgm, bgm_samples.data(), 256) == SCE_OK);

            // Drenagem durante interrupção retorna silêncio absoluto (0 frames)
            std::vector<std::int16_t> drain_interrupted(256 * 2, -1);
            std::size_t drained_count = audio_mgr.drain_main_port_pcm16(drain_interrupted.data(), 256);
            CHECK(drained_count == 0);

            // 9b. Simula AVAudioSessionInterruptionTypeEnded com AVAudioSessionInterruptionOptionShouldResume
            audio_mgr.set_paused(false);
            CHECK(!audio_mgr.is_paused());

            // Como os buffers anteriores foram purgados na interrupção, não há áudio preso da sessão anterior
            drained_count = audio_mgr.drain_main_port_pcm16(drain_interrupted.data(), 256);
            CHECK(drained_count == 0);

            // Nova submissão pós-retomada flui perfeitamente com mixagem limpa (MAIN 3000 + BGM 1500 = 4500)
            std::vector<std::int16_t> fresh_main(256 * 2, 3000);
            std::vector<std::int16_t> fresh_bgm(256 * 2, 1500);
            CHECK(audio_mgr.output(p_main, fresh_main.data(), 256) == SCE_OK);
            CHECK(audio_mgr.output(p_bgm, fresh_bgm.data(), 256) == SCE_OK);

            std::vector<std::int16_t> drain_resumed(256 * 2, 0);
            drained_count = audio_mgr.drain_main_port_pcm16(drain_resumed.data(), 256);
            CHECK(drained_count == 256);
            for (std::size_t i = 0; i < 256 * 2; ++i) {
                CHECK(drain_resumed[i] == 4500);
            }

            CHECK(audio_mgr.close_port(p_main) == SCE_OK);
            CHECK(audio_mgr.close_port(p_bgm) == SCE_OK);
        }
    });
}
