#include "check.h"
#include "audio_out.h"
#include "atrac9_decoder.h"
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
    CHECK(info.dropped_frames == 1024);

    // Consulta de PortInfo com ponteiro nulo
    CHECK(sceAudioOutGetPortInfo(p_out, nullptr) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    CHECK(sceAudioOutGetPortState(p_out, nullptr) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
    CHECK(sceAudioOutGetPortInfo(99999, &info) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    // Drain da porta principal através de drain_main_port_pcm16
    std::vector<std::int16_t> main_drained(256 * 2, 0);
    std::size_t m_read = AudioOutManager::instance().drain_main_port_pcm16(main_drained.data(), 256);
    CHECK(m_read == 256);

    CHECK(sceAudioOutClose(p_out) == SCE_OK);

    // ========================================================================
    // 7.1 T053: Pressão no buffer sem confirmar quadros descartados e reprodução contínua
    // ========================================================================
    std::int32_t p_press = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(p_press > 0);
    // Submete 20 blocos de 256 = 5120 frames para buffer virgem de 4096
    for (int i = 0; i < 20; ++i) {
        CHECK(sceAudioOutOutput(p_press, const_stereo.data()) == SCE_OK);
    }
    SceAudioOutPortInfo press_info{};
    CHECK(sceAudioOutGetPortInfo(p_press, &press_info) == SCE_OK);
    CHECK(press_info.overflow_count >= 1);
    CHECK(press_info.total_frames_output == 4096);
    CHECK(press_info.dropped_frames == 1024);
    CHECK(press_info.total_frames_output + press_info.dropped_frames == 5120);

    // Reprodução contínua: drenando o buffer, a escrita subsequente avança sem descartes
    std::vector<std::int16_t> drain_buf(1024 * 2, 0);
    std::size_t num_drained = AudioOutManager::instance().drain_port_pcm16(p_press, drain_buf.data(), 1024);
    CHECK(num_drained == 1024);
    for (int i = 0; i < 4; ++i) {
        CHECK(sceAudioOutOutput(p_press, const_stereo.data()) == SCE_OK);
    }
    CHECK(sceAudioOutGetPortInfo(p_press, &press_info) == SCE_OK);
    CHECK(press_info.total_frames_output == 4096 + 1024);
    CHECK(press_info.dropped_frames == 1024);
    CHECK(sceAudioOutClose(p_press) == SCE_OK);

    // A fila do host é estéreo: duplicar amostras mono sem escrever além
    // do destino dimensionado para apenas 2 * frames amostras.
    const auto mono = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                    SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
    CHECK(mono > 0);
    std::vector<std::int16_t> mono_block(256, 42);
    CHECK(sceAudioOutOutput(mono, mono_block.data()) == SCE_OK);
    std::vector<std::int16_t> host_buffer(514, static_cast<std::int16_t>(-1));
    CHECK(AudioOutManager::instance().drain_main_port_pcm16(host_buffer.data(), 256) == 256);
    for (std::size_t i = 0; i < 512; ++i) CHECK(host_buffer[i] == 42);
    CHECK(host_buffer[512] == -1 && host_buffer[513] == -1);
    CHECK(sceAudioOutClose(mono) == SCE_OK);

    // 8 canais: apenas L/R frontais são enviados ao host estéreo.
    const auto surround = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                        SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH);
    CHECK(surround > 0);
    std::vector<std::int16_t> surround_block(256 * 8, 999);
    for (std::size_t i = 0; i < 256; ++i) {
        surround_block[i * 8] = 10;
        surround_block[i * 8 + 1] = 20;
    }
    CHECK(sceAudioOutOutput(surround, surround_block.data()) == SCE_OK);
    std::fill(host_buffer.begin(), host_buffer.end(), static_cast<std::int16_t>(-1));
    CHECK(AudioOutManager::instance().drain_main_port_pcm16(host_buffer.data(), 256) == 256);
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(host_buffer[2 * i] == 10 && host_buffer[2 * i + 1] == 20);
    }
    CHECK(host_buffer[512] == -1 && host_buffer[513] == -1);
    CHECK(sceAudioOutClose(surround) == SCE_OK);

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

    // ========================================================================
    // 9. Decodificação e Saída de Áudio ATRAC9 (T011)
    // ========================================================================
    {
        auto h_at9 = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                     SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h_at9 > 0);

        auto at9_cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0); // 48kHz, stereo, 192 bytes
        std::vector<std::uint8_t> at9_blocks(192 * 2, 0); // 2 blocos de silêncio ATRAC9 válidos

        // Rejeições de ponteiro nulo e tamanho zero
        CHECK(sceAudioOutOutputAtrac9(h_at9, nullptr, at9_blocks.size(), at9_cfg.data()) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
        CHECK(sceAudioOutOutputAtrac9(h_at9, at9_blocks.data(), 0, at9_cfg.data()) == SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
        CHECK(sceAudioOutOutputAtrac9(-1, at9_blocks.data(), at9_blocks.size(), at9_cfg.data()) == SCE_AUDIO_OUT_ERROR_INVALID_PORT);

        // Sucesso de submissão direta do stream ATRAC9
        CHECK(sceAudioOutOutputAtrac9(h_at9, at9_blocks.data(), at9_blocks.size(), at9_cfg.data()) == SCE_OK);

        std::vector<std::int16_t> at9_drained(512 * 2, -1);
        std::size_t at9_read = AudioOutManager::instance().drain_port_pcm16(h_at9, at9_drained.data(), 512);
        CHECK(at9_read > 0);

        // Submissão via GuestMemory
        GuestMemory at9_guest_mem(65536);
        CHECK(at9_guest_mem.map(0x10000, 65536, read | write) == MemoryError::ok);
        CHECK(at9_guest_mem.store(0x10000, at9_blocks.data(), at9_blocks.size()) == MemoryError::ok);

        CHECK(sceAudioOutOutputHleAtrac9(h_at9, at9_guest_mem, 0x10000, at9_blocks.size(), at9_cfg.data()) == SCE_OK);
        at9_read = AudioOutManager::instance().drain_port_pcm16(h_at9, at9_drained.data(), 512);
        CHECK(at9_read > 0);

        CHECK(sceAudioOutClose(h_at9) == SCE_OK);
    }

    // ========================================================================
    // 10. Mixagem de Múltiplas Portas (MAIN + BGM) e Saturação PCM16 (T079)
    // ========================================================================
    {
        AudioOutManager::instance().close_all_ports();

        // Abre uma porta MAIN e uma porta BGM
        auto h_main = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                      SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        auto h_bgm = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 256, 48000,
                                     SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h_main > 0 && h_bgm > 0);

        // 1. Mixagem linear simples: MAIN (1000) + BGM (2500) = 3500
        std::vector<std::int16_t> main_buf(256 * 2, 1000);
        std::vector<std::int16_t> bgm_buf(256 * 2, 2500);
        CHECK(sceAudioOutOutput(h_main, main_buf.data()) == SCE_OK);
        CHECK(sceAudioOutOutput(h_bgm, bgm_buf.data()) == SCE_OK);

        std::vector<std::int16_t> mixed_out(256 * 2, 0);
        std::size_t drained = AudioOutManager::instance().drain_main_port_pcm16(mixed_out.data(), 256);
        CHECK(drained == 256);
        for (std::size_t i = 0; i < 256 * 2; ++i) {
            CHECK(mixed_out[i] == 3500);
        }

        // 2. Mixagem com saturação positiva (+32767): MAIN (25000) + BGM (15000) = 40000 -> 32767
        std::vector<std::int16_t> sat_pos_main(256 * 2, 25000);
        std::vector<std::int16_t> sat_pos_bgm(256 * 2, 15000);
        CHECK(sceAudioOutOutput(h_main, sat_pos_main.data()) == SCE_OK);
        CHECK(sceAudioOutOutput(h_bgm, sat_pos_bgm.data()) == SCE_OK);

        drained = AudioOutManager::instance().drain_main_port_pcm16(mixed_out.data(), 256);
        CHECK(drained == 256);
        for (std::size_t i = 0; i < 256 * 2; ++i) {
            CHECK(mixed_out[i] == 32767);
        }

        // 3. Mixagem com saturação negativa (-32768): MAIN (-20000) + BGM (-20000) = -40000 -> -32768
        std::vector<std::int16_t> sat_neg_main(256 * 2, -20000);
        std::vector<std::int16_t> sat_neg_bgm(256 * 2, -20000);
        CHECK(sceAudioOutOutput(h_main, sat_neg_main.data()) == SCE_OK);
        CHECK(sceAudioOutOutput(h_bgm, sat_neg_bgm.data()) == SCE_OK);

        drained = AudioOutManager::instance().drain_main_port_pcm16(mixed_out.data(), 256);
        CHECK(drained == 256);
        for (std::size_t i = 0; i < 256 * 2; ++i) {
            CHECK(mixed_out[i] == -32768);
        }

        CHECK(sceAudioOutClose(h_main) == SCE_OK);
        CHECK(sceAudioOutClose(h_bgm) == SCE_OK);
    }

    // ========================================================================
    // 11. Suspensão / Retomada e Purga de Buffers Inativos (T050 / T043)
    // ========================================================================
    {
        auto& mgr = AudioOutManager::instance();
        mgr.close_all_ports();
        CHECK(!mgr.is_paused());

        auto h_test = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                     SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h_test > 0);

        std::vector<std::int16_t> audio_data(256 * 2, 777);
        CHECK(sceAudioOutOutput(h_test, audio_data.data()) == SCE_OK);

        // Simula interrupção de áudio (AVAudioSessionInterruptionTypeBegan)
        mgr.set_paused(true);
        CHECK(mgr.is_paused());

        // Quando pausado, a drenagem é silenciada
        std::vector<std::int16_t> out_buf(256 * 2, -1);
        CHECK(mgr.drain_main_port_pcm16(out_buf.data(), 256) == 0);

        // Purga de buffers inativos (alívio de memória sob didReceiveMemoryWarning e fim de interrupção)
        mgr.purge_inactive_buffers();

        // Submissão durante interrupção é absorvida com SCE_OK sem sujar buffers
        std::vector<std::int16_t> ignored_data(256 * 2, 9999);
        CHECK(sceAudioOutOutput(h_test, ignored_data.data()) == SCE_OK);
        CHECK(mgr.drain_main_port_pcm16(out_buf.data(), 256) == 0);

        // Simula fim da interrupção sem opção de retomada imediata: deve permanecer pausado
        // (ex: chamada encerrada mas app em segundo plano)
        CHECK(mgr.is_paused());

        // Simula fim da interrupção com retomada (AVAudioSessionInterruptionOptionShouldResume)
        mgr.set_paused(false);
        CHECK(!mgr.is_paused());

        // Com o buffer purgado, não há áudio preso ou estalo da sessão anterior
        CHECK(mgr.drain_main_port_pcm16(out_buf.data(), 256) == 0);

        // Nova saída após retomada opera normalmente
        CHECK(sceAudioOutOutput(h_test, audio_data.data()) == SCE_OK);
        CHECK(mgr.drain_main_port_pcm16(out_buf.data(), 256) == 256);
        CHECK(out_buf[0] == 777);

        CHECK(sceAudioOutClose(h_test) == SCE_OK);

        // 11b. Teste com múltiplas portas ativas (MAIN + BGM) sob interrupção e retomada
        auto h_m = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        auto h_b = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h_m > 0 && h_b > 0);

        std::vector<std::int16_t> m_samples(256 * 2, 1200);
        std::vector<std::int16_t> b_samples(256 * 2, 800);
        CHECK(sceAudioOutOutput(h_m, m_samples.data()) == SCE_OK);
        CHECK(sceAudioOutOutput(h_b, b_samples.data()) == SCE_OK);

        // Interrupção
        mgr.set_paused(true);
        mgr.purge_inactive_buffers();
        std::vector<std::int16_t> mixed_silence(256 * 2, -1);
        CHECK(mgr.drain_main_port_pcm16(mixed_silence.data(), 256) == 0);

        // Retomada e nova mixagem limpa
        mgr.set_paused(false);
        std::vector<std::int16_t> fresh_m(256 * 2, 2000);
        std::vector<std::int16_t> fresh_b(256 * 2, 1000);
        CHECK(sceAudioOutOutput(h_m, fresh_m.data()) == SCE_OK);
        CHECK(sceAudioOutOutput(h_b, fresh_b.data()) == SCE_OK);

        std::vector<std::int16_t> mixed_resumed(256 * 2, 0);
        CHECK(mgr.drain_main_port_pcm16(mixed_resumed.data(), 256) == 256);
        for (std::size_t i = 0; i < 256 * 2; ++i) {
            CHECK(mixed_resumed[i] == 3000);
        }

        CHECK(sceAudioOutClose(h_m) == SCE_OK);
        CHECK(sceAudioOutClose(h_b) == SCE_OK);
    }

    // ========================================================================
    // 12. T184: Limitação de Frames a MAX_SAMPLES_PER_BLOCK em output e output_hle
    // ========================================================================
    {
        auto& mgr = AudioOutManager::instance();
        mgr.close_all_ports();

        auto h_lim = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                     SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        CHECK(h_lim > 0);

        std::vector<std::int16_t> sample_buf(256 * 2, 123);

        // 1. Frames maiores que MAX_SAMPLES_PER_BLOCK (4096) são rejeitados imediatamente com INVALID_PARAM
        CHECK(mgr.output(h_lim, sample_buf.data(), AudioOutManager::MAX_SAMPLES_PER_BLOCK + 1) ==
              SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

        // 2. Valores extremos adversariais (0xFFFFFFFF, 0x80000000) prevenindo overflow aritmético
        CHECK(mgr.output(h_lim, sample_buf.data(), static_cast<std::size_t>(0xFFFFFFFFULL)) ==
              SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
        CHECK(mgr.output(h_lim, sample_buf.data(), static_cast<std::size_t>(0x80000000ULL)) ==
              SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

        // 3. Validação de output_hle com GuestMemory
        GuestMemory gmem(65536);
        CHECK(gmem.map(0x10000, 65536, read | write) == MemoryError::ok);
        CHECK(gmem.store(0x10000, sample_buf.data(), 256 * 2 * sizeof(std::int16_t)) == MemoryError::ok);

        CHECK(mgr.output_hle(h_lim, gmem, 0x10000, AudioOutManager::MAX_SAMPLES_PER_BLOCK + 1) ==
              SCE_AUDIO_OUT_ERROR_INVALID_PARAM);
        CHECK(mgr.output_hle(h_lim, gmem, 0x10000, static_cast<std::size_t>(0xFFFFFFFFULL)) ==
              SCE_AUDIO_OUT_ERROR_INVALID_PARAM);

        // 4. Limite exato MAX_SAMPLES_PER_BLOCK (4096) é aceito
        std::vector<std::int16_t> max_buf(AudioOutManager::MAX_SAMPLES_PER_BLOCK * 2, 456);
        CHECK(mgr.output(h_lim, max_buf.data(), AudioOutManager::MAX_SAMPLES_PER_BLOCK) == SCE_OK);

        CHECK(sceAudioOutClose(h_lim) == SCE_OK);
    }
}); }
