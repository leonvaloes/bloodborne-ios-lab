#include "check.h"
#include "audio_out.h"
#include "guest_memory.h"
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>

int main() { return run([] {
    using namespace bb;

    // Garante que o singleton padrão comece limpo
    AudioOutManager::instance().close_all_ports();

    // ========================================================================
    // 1. Validação de Parâmetros e Erros na Abertura (sceAudioOutOpen)
    // ========================================================================
    // Tipo de porta inválido (< 0 ou > AUX)
    CHECK(sceAudioOutOpen(0, -1, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
    CHECK(sceAudioOutOpen(0, 6, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

    // Índice fora do limite (< 0 ou >= 32)
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, -1, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 32, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

    // Granularidade do bloco (len) inválida (0 ou > 4096)
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 0, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 4097, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

    // Taxa de amostragem não suportada (!= 48000 Hz)
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 44100, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 0, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

    // Formato não suportado
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, 999) == SCE_AUDIO_OUT_ERROR_UNSUPPORTED_FORMAT);
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO) == SCE_AUDIO_OUT_ERROR_UNSUPPORTED_FORMAT);

    // ========================================================================
    // 2. Ciclo de Vida: Abertura, Unicidade (BUSY) e Fechamento
    // ========================================================================
    // Inicialização do subsistema
    CHECK(sceAudioOutInit() == SCE_OK);

    // Abertura com sucesso da porta principal (MAIN, index 0, 256 frames, 48kHz, S16_STEREO)
    std::int32_t handle_main = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(handle_main > 0);
    CHECK(AudioOutManager::instance().is_port_open(handle_main));

    // Tentativa de abrir novamente a mesma porta MAIN index 0 enquanto ocupada
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_BUSY);

    // Abertura de porta de outro tipo (BGM index 0) deve funcionar
    std::int32_t handle_bgm = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 512, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(handle_bgm > 0);
    CHECK(handle_bgm != handle_main);
    CHECK(AudioOutManager::instance().open_ports_count() == 2);

    // Fechamento de porta aberta com sucesso
    CHECK(sceAudioOutClose(handle_main) == SCE_OK);
    CHECK(!AudioOutManager::instance().is_port_open(handle_main));
    CHECK(AudioOutManager::instance().open_ports_count() == 1);

    // Fechamento de porta inexistente ou já fechada
    CHECK(sceAudioOutClose(handle_main) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    CHECK(sceAudioOutClose(-1) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    CHECK(sceAudioOutClose(99999) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    // Agora que MAIN 0 foi fechada, pode ser reaberta
    handle_main = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(handle_main > 0);
    CHECK(AudioOutManager::instance().open_ports_count() == 2);

    // Limpeza
    CHECK(sceAudioOutClose(handle_main) == SCE_OK);
    CHECK(sceAudioOutClose(handle_bgm) == SCE_OK);
    CHECK(AudioOutManager::instance().open_ports_count() == 0);

    // ========================================================================
    // 3. Limite de Portas Concorrentes (MAX_CONCURRENT_PORTS = 16)
    // ========================================================================
    std::vector<std::int32_t> handles;
    // Abre 16 portas concorrentes com combinações válidas de tipo e índice
    for (std::int32_t i = 0; i < 8; ++i) {
        std::int32_t h = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, i, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h > 0);
        handles.push_back(h);
    }
    for (std::int32_t i = 0; i < 8; ++i) {
        std::int32_t h = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_VOICE, i, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h > 0);
        handles.push_back(h);
    }
    CHECK(AudioOutManager::instance().open_ports_count() == 16);

    // Tentativa de abrir a 17ª porta concorrente deve falhar com PORT_FULL
    CHECK(sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_AUX, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO) == SCE_AUDIO_OUT_ERROR_PORT_FULL);

    // Libera uma porta e tenta novamente
    CHECK(sceAudioOutClose(handles.back()) == SCE_OK);
    handles.pop_back();
    CHECK(AudioOutManager::instance().open_ports_count() == 15);

    std::int32_t h_aux = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_AUX, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(h_aux > 0);
    handles.push_back(h_aux);
    CHECK(AudioOutManager::instance().open_ports_count() == 16);

    // Limpa todas as portas alocadas
    for (auto h : handles) {
        CHECK(sceAudioOutClose(h) == SCE_OK);
    }
    CHECK(AudioOutManager::instance().open_ports_count() == 0);

    // ========================================================================
    // 4. Submissão de Buffers (Host Pointer e GuestMemory) e Validação de Limites
    // ========================================================================
    std::int32_t p_out = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(p_out > 0);

    // Submissão com ponteiro nulo
    CHECK(sceAudioOutOutput(p_out, nullptr) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    // Submissão em handle inexistente
    std::int16_t dummy_pcm[4] = {1, 2, 3, 4};
    CHECK(sceAudioOutOutput(99999, dummy_pcm) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    // Prepara 256 frames estéreo conhecidos (512 amostras PCM16)
    std::vector<std::int16_t> host_pcm(256 * 2);
    for (std::size_t i = 0; i < host_pcm.size(); ++i) {
        host_pcm[i] = static_cast<std::int16_t>(i * 7 + 13);
    }

    CHECK(sceAudioOutOutput(p_out, host_pcm.data()) == SCE_OK);

    // Verifica status da porta via sceAudioOutGetPortInfo
    SceAudioOutPortInfo info{};
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.status == 1);
    CHECK(info.port_type == SCE_AUDIO_OUT_PORT_TYPE_MAIN);
    CHECK(info.index == 0);
    CHECK(info.samples_per_block == 256);
    CHECK(info.sample_rate == 48000);
    CHECK(info.channels == 2);
    CHECK(info.total_frames_output == 256);

    // Drena amostras do ring buffer da porta e confere dados bit a bit
    std::vector<std::int16_t> drained(256 * 2, 0);
    std::size_t drained_frames = AudioOutManager::instance().drain_port_pcm16(p_out, drained.data(), 256);
    CHECK(drained_frames == 256);
    CHECK(drained == host_pcm);

    // Submissão via GuestMemory (output_hle)
    GuestMemory mem(4096);
    CHECK(mem.map(0x1000, 4096, read | write) == MemoryError::ok);

    std::vector<std::int16_t> guest_pcm(256 * 2);
    for (std::size_t i = 0; i < guest_pcm.size(); ++i) {
        guest_pcm[i] = static_cast<std::int16_t>(i * 11 + 5);
    }
    CHECK(mem.store(0x1000, guest_pcm.data(), guest_pcm.size() * sizeof(std::int16_t)) == MemoryError::ok);

    CHECK(sceAudioOutOutputHle(p_out, mem, 0x1000) == SCE_OK);
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.total_frames_output == 512);

    drained_frames = AudioOutManager::instance().drain_port_pcm16(p_out, drained.data(), 256);
    CHECK(drained_frames == 256);
    CHECK(drained == guest_pcm);

    // Rejeição de endereço guest inválido (endereço zero ou unmapped)
    CHECK(sceAudioOutOutputHle(p_out, mem, 0) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    CHECK(sceAudioOutOutputHle(p_out, mem, 0x900000) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);

    // ========================================================================
    // 5. Ajuste de Volume e Atenuação Estéreo
    // ========================================================================
    // Validações de parâmetros
    CHECK(sceAudioOutSetVolume(p_out, SCE_AUDIO_OUT_VOLUME_FLAG_STEREO, static_cast<const std::int32_t*>(nullptr)) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    std::int32_t vol_test[2] = {32768, 32768};
    CHECK(sceAudioOutSetVolume(p_out, 0, vol_test) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
    CHECK(sceAudioOutSetVolume(p_out, 0x10, vol_test) == SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
    CHECK(sceAudioOutSetVolume(99999, SCE_AUDIO_OUT_VOLUME_FLAG_STEREO, vol_test) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    // Atenuação estereofônica assimétrica: Canal esquerdo 100% (32768), Canal direito 50% (16384)
    std::int32_t vol_asymm[2] = {32768, 16384};
    CHECK(sceAudioOutSetVolume(p_out, SCE_AUDIO_OUT_VOLUME_FLAG_STEREO, vol_asymm) == SCE_OK);
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.volume_left == 32768);
    CHECK(info.volume_right == 16384);

    // Submete 1 bloco com valores estéreo constantes: L = 1000, R = 1000
    std::vector<std::int16_t> const_stereo(256 * 2);
    for (std::size_t i = 0; i < 256; ++i) {
        const_stereo[i * 2 + 0] = 1000;
        const_stereo[i * 2 + 1] = 1000;
    }
    CHECK(sceAudioOutOutput(p_out, const_stereo.data()) == SCE_OK);

    drained_frames = AudioOutManager::instance().drain_port_pcm16(p_out, drained.data(), 256);
    CHECK(drained_frames == 256);
    CHECK(drained[0] == 1000); // L atenuado por 1.0 = 1000
    CHECK(drained[1] == 500);  // R atenuado por 0.5 = 500

    // Ajuste individual apenas de canal esquerdo
    std::int32_t single_left[1] = {0}; // Muta canal esquerdo
    CHECK(sceAudioOutSetVolume(p_out, SCE_AUDIO_OUT_VOLUME_FLAG_LEFT, single_left) == SCE_OK);
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.volume_left == 0);
    CHECK(info.volume_right == 16384); // Mantém o direito em 50%

    // Ajuste individual apenas de canal direito
    std::int32_t single_right[1] = {0}; // Muta canal direito
    CHECK(sceAudioOutSetVolume(p_out, SCE_AUDIO_OUT_VOLUME_FLAG_RIGHT, single_right) == SCE_OK);
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.volume_left == 0);
    CHECK(info.volume_right == 0);

    // Com ambos mutados, saída deve ser silêncio absoluto (0)
    CHECK(sceAudioOutOutput(p_out, const_stereo.data()) == SCE_OK);
    drained_frames = AudioOutManager::instance().drain_port_pcm16(p_out, drained.data(), 256);
    CHECK(drained_frames == 256);
    CHECK(drained[0] == 0 && drained[1] == 0);

    // Restaura volume com set_volume_float (1.0f em ambos)
    CHECK(AudioOutManager::instance().set_volume_float(p_out, 1.0f, 1.0f) == SCE_OK);
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.volume_left == 32768 && info.volume_right == 32768);

    // ========================================================================
    // 6. Consultas de Tempo Monotônico (sceAudioOutGetLastOutputTime) Determinísticas
    // ========================================================================
    // Nova porta sem nenhum output ainda
    std::int32_t p_time = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(p_time > 0);

    std::uint64_t last_time = 9999;
    CHECK(sceAudioOutGetLastOutputTime(p_time, &last_time) == SCE_OK);
    CHECK(last_time == 0); // Nenhum output realizado

    // Validações de erro
    CHECK(sceAudioOutGetLastOutputTime(p_time, nullptr) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    CHECK(sceAudioOutGetLastOutputTime(99999, &last_time) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    // Injeção de time provider determinístico para testes
    std::uint64_t simulated_time = 1000000; // 1 segundo em us
    AudioOutManager::instance().set_time_provider([&simulated_time]() {
        return simulated_time;
    });

    CHECK(sceAudioOutOutput(p_time, const_stereo.data()) == SCE_OK);
    CHECK(sceAudioOutGetLastOutputTime(p_time, &last_time) == SCE_OK);
    CHECK(last_time == 1000000);

    // Avança 21333 microssegundos (~1024 amostras a 48kHz)
    simulated_time = 1021333;
    CHECK(sceAudioOutOutput(p_time, const_stereo.data()) == SCE_OK);
    CHECK(sceAudioOutGetLastOutputTime(p_time, &last_time) == SCE_OK);
    CHECK(last_time == 1021333);

    // Reseta time provider para o relógio real do sistema e valida monotonicidade
    AudioOutManager::instance().reset_time_provider();
    CHECK(sceAudioOutOutput(p_time, const_stereo.data()) == SCE_OK);
    std::uint64_t real_time = 0;
    CHECK(sceAudioOutGetLastOutputTime(p_time, &real_time) == SCE_OK);
    CHECK(real_time > 0);

    CHECK(sceAudioOutClose(p_time) == SCE_OK);

    // ========================================================================
    // 7. Estatísticas de Diagnóstico: Underflow e Overflow
    // ========================================================================
    // Porta aberta p_out (len = 256)
    // Drena intencionalmente mais amostras do que há disponível no buffer
    std::vector<std::int16_t> large_drain(4096 * 2, 0);
    AudioOutManager::instance().drain_port_pcm16(p_out, large_drain.data(), 4096);
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.underflow_count >= 1);

    // Submete amostras até estourar a capacidade do buffer ring (4096 frames)
    // 20 blocos de 256 frames = 5120 frames > 4096 capacidade
    for (int i = 0; i < 20; ++i) {
        CHECK(sceAudioOutOutput(p_out, const_stereo.data()) == SCE_OK);
    }
    CHECK(sceAudioOutGetPortInfo(p_out, &info) == SCE_OK);
    CHECK(info.overflow_count >= 1);
    CHECK(info.total_frames_output >= 5120);

    // Consulta de PortInfo com ponteiro nulo
    CHECK(sceAudioOutGetPortInfo(p_out, nullptr) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    CHECK(sceAudioOutGetPortState(p_out, nullptr) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    CHECK(sceAudioOutGetPortInfo(99999, &info) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    // Drain da porta principal através de drain_main_port_pcm16
    std::vector<std::int16_t> main_drained(256 * 2, 0);
    std::size_t m_read = AudioOutManager::instance().drain_main_port_pcm16(main_drained.data(), 256);
    CHECK(m_read == 256);

    CHECK(sceAudioOutClose(p_out) == SCE_OK);

    // A fila do host é estéreo: uma porta principal mono não deve ser drenada
    // para um destino com capacidade para apenas 2 * frames amostras.
    const auto mono = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                    SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
    CHECK(mono > 0);
    std::vector<std::int16_t> mono_block(256, 42);
    CHECK(sceAudioOutOutput(mono, mono_block.data()) == SCE_OK);
    std::vector<std::int16_t> host_buffer(512, -1);
    CHECK(AudioOutManager::instance().drain_main_port_pcm16(host_buffer.data(), 256) == 0);
    CHECK(sceAudioOutClose(mono) == SCE_OK);

    // Fechamento concorrente com o callback de saída não pode usar PortEntry
    // depois da destruição. Exercita ciclos curtos de abertura/drenagem/fecho.
    std::atomic<bool> audio_run{true};
    std::thread callback([&]() {
        std::vector<std::int16_t> samples(256 * 2);
        while (audio_run.load()) {
            AudioOutManager::instance().drain_main_port_pcm16(samples.data(), 256);
        }
    });
    for (int i = 0; i < 100; ++i) {
        const auto h = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                      SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h > 0);
        CHECK(sceAudioOutClose(h) == SCE_OK);
    }
    audio_run = false;
    callback.join();

    // ========================================================================
    // 8. Teste de Concorrência Multithreaded
    // ========================================================================
    std::atomic<bool> thread_error{false};
    std::vector<std::thread> workers;

    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([t, &thread_error]() {
            auto &mgr = AudioOutManager::instance();
            std::int32_t p = mgr.open_port(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, t, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            if (p <= 0) {
                thread_error = true;
                return;
            }

            std::vector<std::int16_t> block(256 * 2, static_cast<std::int16_t>(t * 100));
            for (int k = 0; k < 10; ++k) {
                if (mgr.output(p, block.data()) != SCE_OK) thread_error = true;
                std::vector<std::int16_t> drain_buf(128 * 2);
                mgr.drain_port_pcm16(p, drain_buf.data(), 128);
                SceAudioOutPortInfo pinfo{};
                if (mgr.get_port_info(p, &pinfo) != SCE_OK) thread_error = true;
            }

            if (mgr.close_port(p) != SCE_OK) {
                thread_error = true;
            }
        });
    }

    for (auto &w : workers) {
        if (w.joinable()) w.join();
    }
    CHECK(!thread_error.load());
    CHECK(AudioOutManager::instance().open_ports_count() == 0);
}); }
