#include "check.h"
#include "ngs2_hle.h"
#include "audio_out.h"
#include "guest_memory.h"
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <iostream>

int main() { return run([] {
    using namespace bb;

    // Garante que o ambiente de áudio inicie limpo
    Ngs2Manager::instance().destroy_all_racks();
    AudioOutManager::instance().close_all_ports();

    // Configuração ATRAC9 sintética determinística (48 kHz, Estéreo, 192 bytes por superframe)
    // Conforme especificado pelo formato LibAtrac9 (sample rate index 7 = 48 kHz, channel config 2 = stereo)
    auto at9_cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0);
    CHECK(at9_cfg[0] == 0xFE);
    CHECK(at9_cfg[1] == 0x74);
    CHECK(at9_cfg[2] == 0x17);
    CHECK(at9_cfg[3] == 0xE0);

    // ========================================================================
    // 1. Rejeição de streams corrompidos, configurações inválidas e limites de memória
    // ========================================================================
    SceNgs2RackHandle rack_h = 0;
    CHECK(sceNgs2RackCreate(8, 2, 256, &rack_h) == SCE_NGS2_OK);
    CHECK(rack_h > 0);

    std::vector<std::uint8_t> valid_stream(192 * 2, 0); // 2 blocos de silêncio ATRAC9 válidos

    // Rejeição de ponteiros nulos
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, nullptr, valid_stream.size(), at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_POINTER);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), valid_stream.size(), nullptr) == SCE_NGS2_ERROR_INVALID_POINTER);

    // Rejeição de tamanho zero
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), 0, at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Rejeição de configuração inválida (Magic != 0xFE ou campos corrompidos)
    std::uint8_t bad_cfg[4] = {0xAA, 0x12, 0x34, 0x56};
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), valid_stream.size(), bad_cfg) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Rejeição de stream truncado (tamanho menor que um superframe de 192 bytes)
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), 100, at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Rejeição de stream corrompido (bytes de payload que quebram o cabeçalho/descompressão do LibAtrac9)
    // No LibAtrac9, um bloco inicial com bit 0 = 0 (FirstInSuperframe) e bit 1 = 1 (ReuseBandParams)
    // viola as regras do codec e aciona ERR_UNPACK_REUSE_BAND_PARAMS_INVALID.
    std::vector<std::uint8_t> corrupt_stream(192, 0x40);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, corrupt_stream.data(), corrupt_stream.size(), at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Rejeição de estouro de memória máxima por stream (> 16 MB)
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), SCE_NGS2_MAX_ATRAC9_STREAM_SIZE + 1024, at9_cfg.data()) == SCE_NGS2_ERROR_OUT_OF_MEMORY);

    // Rejeição de handle inválido ou índice de voz fora do limite do rack
    CHECK(sceNgs2VoiceSetAtrac9Buffer(99999, 0, valid_stream.data(), valid_stream.size(), at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_HANDLE);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 8, valid_stream.data(), valid_stream.size(), at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Rejeição via GuestMemory com ponteiro inválido / não mapeado
    GuestMemory guest_mem(1024 * 1024);
    CHECK(sceNgs2VoiceSetAtrac9BufferHle(rack_h, 0, guest_mem, 0, valid_stream.size(), at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_POINTER);
    CHECK(sceNgs2VoiceSetAtrac9BufferHle(rack_h, 0, guest_mem, 0x500000, valid_stream.size(), at9_cfg.data()) == SCE_NGS2_ERROR_INVALID_POINTER);

    // Sucesso de configuração a partir de GuestMemory mapeada
    std::uint64_t guest_addr = 0x30000;
    CHECK(guest_mem.map(guest_addr, valid_stream.size(), Permission::read | Permission::write) == MemoryError::ok);
    CHECK(guest_mem.store(guest_addr, valid_stream.data(), valid_stream.size()) == MemoryError::ok);
    std::int32_t hle_ret = sceNgs2VoiceSetAtrac9BufferHle(rack_h, 0, guest_mem, guest_addr, valid_stream.size(), at9_cfg.data(), false);
    std::cout << "DEBUG: hle_ret = 0x" << std::hex << hle_ret << std::dec << "\n";
    CHECK(hle_ret == SCE_NGS2_OK);

    // ========================================================================
    // 2. Configuração e Diagnóstico de Voz ATRAC9
    // ========================================================================
    // Configura a voz 0 diretamente com buffer do host
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), valid_stream.size(), at9_cfg.data(), false) == SCE_NGS2_OK);

    SceNgs2VoiceInfo v_info{};
    CHECK(sceNgs2VoiceGetInfo(rack_h, 0, &v_info) == SCE_NGS2_OK);
    CHECK(v_info.is_atrac9);
    CHECK(v_info.state == SCE_NGS2_VOICE_STATE_STOPPED);
    CHECK(!v_info.loop);
    CHECK(v_info.frames_rendered == 0);

    // Controle de repetição / looping
    CHECK(sceNgs2VoiceSetLoop(rack_h, 0, true) == SCE_NGS2_OK);
    bool is_looping = false;
    CHECK(sceNgs2VoiceGetLoop(rack_h, 0, &is_looping) == SCE_NGS2_OK);
    CHECK(is_looping);
    CHECK(sceNgs2VoiceGetInfo(rack_h, 0, &v_info) == SCE_NGS2_OK);
    CHECK(v_info.loop);

    CHECK(sceNgs2VoiceSetLoop(rack_h, 0, false) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetLoop(rack_h, 0, &is_looping) == SCE_NGS2_OK);
    CHECK(!is_looping);

    // Configuração e controle via SceNgs2VoiceHandle
    SceNgs2VoiceHandle vh = sceNgs2MakeVoiceHandle(rack_h, 1);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(vh, valid_stream.data(), valid_stream.size(), at9_cfg.data(), true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetInfo(vh, &v_info) == SCE_NGS2_OK);
    CHECK(v_info.is_atrac9);
    CHECK(v_info.loop);

    // ========================================================================
    // 3. Renderização de Rack com Voz ATRAC9 e Roteamento para AudioOutManager
    // ========================================================================
    CHECK(sceAudioOutInit() == SCE_OK);
    std::int32_t audio_port = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                              SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(audio_port > 0);
    CHECK(sceNgs2RackSetAudioOutPort(rack_h, audio_port) == SCE_NGS2_OK);

    // Configura voz 0 com stream de 4 blocos ATRAC9 (4 * 192 = 768 bytes = 1024 frames)
    std::vector<std::uint8_t> stream_4blocks(192 * 4, 0);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, stream_4blocks.data(), stream_4blocks.size(), at9_cfg.data(), false) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, 1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, -1.0f) == SCE_NGS2_OK); // Pan total à esquerda
    CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);

    // Renderiza o primeiro bloco (256 frames)
    std::vector<std::int16_t> rendered_pcm(256 * 2, -1);
    std::size_t rendered_frames = 0;
    CHECK(sceNgs2RackRender(rack_h, rendered_pcm.data(), 256, &rendered_frames) == SCE_NGS2_OK);
    CHECK(rendered_frames == 256);

    // Drena amostras da porta de áudio do AudioOutManager e valida integridade sem distorção
    std::vector<std::int16_t> drained_pcm(256 * 2, -1);
    std::size_t drained_count = AudioOutManager::instance().drain_port_pcm16(audio_port, drained_pcm.data(), 256);
    CHECK(drained_count == 256);

    // Amostras entregues coincidem entre a saída direta e a porta de áudio
    for (std::size_t i = 0; i < 256 * 2; ++i) {
        CHECK(drained_pcm[i] == rendered_pcm[i]);
    }

    // Como o Pan é -1.0f, o canal direito deve ser exatamente zero
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(drained_pcm[i * 2 + 1] == 0);
    }

    // Testa modulação de Pan para a direita (+1.0f)
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, 1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
    drained_count = AudioOutManager::instance().drain_port_pcm16(audio_port, drained_pcm.data(), 256);
    CHECK(drained_count == 256);
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(drained_pcm[i * 2 + 0] == 0); // Canal esquerdo zerado
    }

    // ========================================================================
    // 4. Teste de One-Shot vs Looping de Áudio ATRAC9
    // ========================================================================
    // 4.1 Teste One-Shot (Voz para automaticamente ao término do stream)
    // Stream com exatamente 2 blocos (2 * 192 = 384 bytes, gerando 512 frames)
    std::vector<std::uint8_t> oneshot_stream(192 * 2, 0);
    CHECK(sceNgs2VoiceStop(rack_h, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, oneshot_stream.data(), oneshot_stream.size(), at9_cfg.data(), false) == SCE_NGS2_OK);
    CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);

    std::int32_t state = -1;
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &state) == SCE_NGS2_OK);
    CHECK(state == SCE_NGS2_VOICE_STATE_PLAYING);

    // Bloco 1 (frames 0..255)
    CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &state) == SCE_NGS2_OK);
    CHECK(state == SCE_NGS2_VOICE_STATE_PLAYING);

    // Bloco 2 (frames 256..511)
    CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);

    // Bloco 3: ao tentar renderizar além do stream em modo one-shot, a voz transita para STOPPED
    CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &state) == SCE_NGS2_OK);
    CHECK(state == SCE_NGS2_VOICE_STATE_STOPPED);

    // 4.2 Teste Looping Contínuo (Voz nunca para e repete os blocos infinitamente)
    // Stream de 2 blocos (512 frames) em loop
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 1, oneshot_stream.data(), oneshot_stream.size(), at9_cfg.data(), true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoicePlay(rack_h, 1) == SCE_NGS2_OK);

    // Renderiza 10 blocos consecutivos (10 * 256 = 2560 frames, equivalendo a 5 repetições completas do stream)
    for (int cycle = 0; cycle < 10; ++cycle) {
        CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
        CHECK(sceNgs2VoiceGetState(rack_h, 1, &state) == SCE_NGS2_OK);
        CHECK(state == SCE_NGS2_VOICE_STATE_PLAYING);
    }

    CHECK(sceNgs2VoiceGetInfo(rack_h, 1, &v_info) == SCE_NGS2_OK);
    CHECK(v_info.frames_rendered == 2560);
    CHECK(sceNgs2VoiceStop(rack_h, 1) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 1, &state) == SCE_NGS2_OK);
    CHECK(state == SCE_NGS2_VOICE_STATE_STOPPED);

    // ========================================================================
    // 5. Suporte a Reamostragem (24 kHz) e Mono -> Estéreo
    // ========================================================================
    // Stream mono a 48 kHz (index 7, channel config 0 = mono, 96 bytes por superframe)
    auto mono_cfg = Atrac9Decoder::make_config_data(7, 0, 96, 0);
    std::vector<std::uint8_t> mono_stream(96 * 2, 0);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 2, mono_stream.data(), mono_stream.size(), mono_cfg.data(), true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoicePlay(rack_h, 2) == SCE_NGS2_OK);
    CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceStop(rack_h, 2) == SCE_NGS2_OK);

    // Stream a 24 kHz (index 4 = 24 kHz, stereo config 2, 96 bytes)
    auto resample_cfg = Atrac9Decoder::make_config_data(4, 2, 96, 0);
    std::vector<std::uint8_t> resample_stream(96 * 4, 0);
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 3, resample_stream.data(), resample_stream.size(), resample_cfg.data(), true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoicePlay(rack_h, 3) == SCE_NGS2_OK);
    CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceStop(rack_h, 3) == SCE_NGS2_OK);

    // ========================================================================
    // 6. Reaproveitamento de Decodificadores e Limite de Instâncias
    // ========================================================================
    // Reconfigura a voz 0 repetidas vezes com streams diferentes para verificar reaproveitamento de instâncias
    for (int i = 0; i < 5; ++i) {
        CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), valid_stream.size(), at9_cfg.data(), false) == SCE_NGS2_OK);
        CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);
        CHECK(sceNgs2RackRender(rack_h) == SCE_NGS2_OK);
        CHECK(sceNgs2VoiceStop(rack_h, 0) == SCE_NGS2_OK);
    }

    // Transição da voz de ATRAC9 para forma de onda sintética (deve liberar recursos e reciclar decodificador)
    CHECK(sceNgs2VoiceSetSyntheticWaveform(rack_h, 0, 440.0f, 0.5f, true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetInfo(rack_h, 0, &v_info) == SCE_NGS2_OK);
    CHECK(!v_info.is_atrac9);

    // Transição de volta para ATRAC9
    CHECK(sceNgs2VoiceSetAtrac9Buffer(rack_h, 0, valid_stream.data(), valid_stream.size(), at9_cfg.data(), true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetInfo(rack_h, 0, &v_info) == SCE_NGS2_OK);
    CHECK(v_info.is_atrac9);

    // ========================================================================
    // 7. Concorrência e Thread-Safety com Múltiplas Vozes ATRAC9
    // ========================================================================
    std::int32_t conc_port = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 256, 48000,
                                             SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(conc_port > 0);

    SceNgs2RackParam conc_param{};
    conc_param.maxVoices = 4;
    conc_param.channels = 2;
    conc_param.blockSize = 256;
    conc_param.sampleRate = 48000;
    conc_param.audioOutPort = conc_port;
    SceNgs2RackHandle conc_rack = 0;
    CHECK(sceNgs2RackCreate(&conc_param, &conc_rack) == SCE_NGS2_OK);

    // Configura 4 vozes concorrentes com ATRAC9 em loop
    for (std::uint32_t v = 0; v < 4; ++v) {
        CHECK(sceNgs2VoiceSetAtrac9Buffer(conc_rack, v, valid_stream.data(), valid_stream.size(), at9_cfg.data(), true) == SCE_NGS2_OK);
        CHECK(sceNgs2VoiceSetVolume(conc_rack, v, 0.5f) == SCE_NGS2_OK);
        CHECK(sceNgs2VoicePlay(conc_rack, v) == SCE_NGS2_OK);
    }

    std::atomic<bool> running{true};
    std::atomic<std::uint32_t> render_count{0};
    std::atomic<std::uint32_t> drain_count{0};

    // Thread A: Renderiza blocos continuamente
    std::thread render_thread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            if (sceNgs2RackRender(conc_rack) == SCE_NGS2_OK) {
                render_count.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    // Thread B: Modula volume e pan concorrentemente
    std::thread mod_thread([&]() {
        float p = -0.8f;
        while (running.load(std::memory_order_relaxed)) {
            for (std::uint32_t v = 0; v < 4; ++v) {
                sceNgs2VoiceSetPan(conc_rack, v, p);
                sceNgs2VoiceSetVolume(conc_rack, v, 0.4f);
            }
            p = -p;
            std::this_thread::yield();
        }
    });

    // Thread C: Drena áudio da porta BGM
    std::thread drain_thread([&]() {
        std::vector<std::int16_t> drain_buf(256 * 2);
        while (running.load(std::memory_order_relaxed)) {
            std::size_t r = AudioOutManager::instance().drain_port_pcm16(conc_port, drain_buf.data(), 256);
            if (r > 0) {
                drain_count.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_relaxed);

    render_thread.join();
    mod_thread.join();
    drain_thread.join();

    CHECK(render_count.load() > 0);
    CHECK(drain_count.load() > 0);

    // ========================================================================
    // 8. Limpeza Final de Racks e Portas
    // ========================================================================
    CHECK(sceNgs2RackDestroy(rack_h) == SCE_NGS2_OK);
    CHECK(sceNgs2RackDestroy(conc_rack) == SCE_NGS2_OK);
    CHECK(sceAudioOutClose(audio_port) == SCE_OK);
    CHECK(sceAudioOutClose(conc_port) == SCE_OK);

    Ngs2Manager::instance().destroy_all_racks();
    AudioOutManager::instance().close_all_ports();

    std::cout << "ngs2_atrac9: Sony ATRAC9 streaming, one-shot/looping, and AudioOut integration passed\n";
}); }
