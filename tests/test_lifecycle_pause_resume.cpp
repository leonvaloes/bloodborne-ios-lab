#include "check.h"
#include "game_boot_coordinator.h"
#include "audio_out.h"
#include "guest_scheduler.h"
#include <chrono>

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
    });
}
