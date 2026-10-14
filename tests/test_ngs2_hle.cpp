#include "check.h"
#include "ngs2_hle.h"
#include "audio_out.h"
#include "guest_memory.h"
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstring>

int main() { return run([] {
    using namespace bb;

    // Garante que o ambiente de áudio inicie limpo
    Ngs2Manager::instance().destroy_all_racks();
    AudioOutManager::instance().close_all_ports();

    // ========================================================================
    // 1. Validação de Parâmetros e Limites de Racks (sceNgs2RackCreate)
    // ========================================================================
    SceNgs2RackHandle rack_h = 0;
    SceNgs2RackParam valid_param{};
    valid_param.maxVoices = 16;
    valid_param.channels = 2;
    valid_param.blockSize = 256;
    valid_param.sampleRate = 48000;
    valid_param.audioOutPort = -1;

    // Rejeição de ponteiros nulos
    CHECK(sceNgs2RackCreate(nullptr, &rack_h) == SCE_NGS2_ERROR_INVALID_POINTER);
    CHECK(sceNgs2RackCreate(&valid_param, nullptr) == SCE_NGS2_ERROR_INVALID_POINTER);

    // Validação estrita de limite de vozes (<= 64 vozes por rack)
    SceNgs2RackParam p_voices = valid_param;
    p_voices.maxVoices = 0;
    CHECK(sceNgs2RackCreate(&p_voices, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);

    p_voices.maxVoices = 65; // Excede limite de 64
    CHECK(sceNgs2RackCreate(&p_voices, &rack_h) == SCE_NGS2_ERROR_VOICE_LIMIT_EXCEEDED);

    // Limite máximo exato de 64 deve ser aceito
    p_voices.maxVoices = 64;
    CHECK(sceNgs2RackCreate(&p_voices, &rack_h) == SCE_NGS2_OK);
    CHECK(rack_h > 0);
    CHECK(sceNgs2RackDestroy(rack_h) == SCE_NGS2_OK);

    // Canais inválidos (apenas 1 mono ou 2 stereo são permitidos)
    SceNgs2RackParam p_ch = valid_param;
    p_ch.channels = 0;
    CHECK(sceNgs2RackCreate(&p_ch, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);
    p_ch.channels = 3;
    CHECK(sceNgs2RackCreate(&p_ch, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);
    p_ch.channels = 8;
    CHECK(sceNgs2RackCreate(&p_ch, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Tamanho de bloco inválido (0 ou > 4096)
    SceNgs2RackParam p_bs = valid_param;
    p_bs.blockSize = 0;
    CHECK(sceNgs2RackCreate(&p_bs, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);
    p_bs.blockSize = 4097;
    CHECK(sceNgs2RackCreate(&p_bs, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Taxa de amostragem inválida (!= 48000 Hz)
    SceNgs2RackParam p_sr = valid_param;
    p_sr.sampleRate = 44100;
    CHECK(sceNgs2RackCreate(&p_sr, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);
    p_sr.sampleRate = 0;
    CHECK(sceNgs2RackCreate(&p_sr, &rack_h) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Criação com sucesso de rack válido
    CHECK(sceNgs2RackCreate(&valid_param, &rack_h) == SCE_NGS2_OK);
    CHECK(rack_h > 0);
    CHECK(Ngs2Manager::instance().is_rack_valid(rack_h));
    CHECK(Ngs2Manager::instance().active_racks_count() == 1);

    // Consulta de informações do Rack criado
    SceNgs2RackInfo rack_info{};
    CHECK(sceNgs2RackGetInfo(rack_h, &rack_info) == SCE_NGS2_OK);
    CHECK(rack_info.handle == rack_h);
    CHECK(rack_info.max_voices == 16);
    CHECK(rack_info.channels == 2);
    CHECK(rack_info.block_size == 256);
    CHECK(rack_info.sample_rate == 48000);
    CHECK(rack_info.active_voices == 0);
    CHECK(rack_info.total_blocks_rendered == 0);

    // Rejeição de ponteiro nulo em get_info
    CHECK(sceNgs2RackGetInfo(rack_h, nullptr) == SCE_NGS2_ERROR_INVALID_POINTER);
    CHECK(sceNgs2RackGetInfo(99999, &rack_info) == SCE_NGS2_ERROR_INVALID_HANDLE);

    // Limite de Racks concorrentes (SCE_NGS2_MAX_RACKS = 16)
    std::vector<SceNgs2RackHandle> extra_racks;
    for (std::size_t i = 1; i < SCE_NGS2_MAX_RACKS; ++i) {
        SceNgs2RackHandle h = 0;
        CHECK(sceNgs2RackCreate(&valid_param, &h) == SCE_NGS2_OK);
        extra_racks.push_back(h);
    }
    CHECK(Ngs2Manager::instance().active_racks_count() == SCE_NGS2_MAX_RACKS);

    // A 17ª criação deve falhar com OUT_OF_MEMORY
    SceNgs2RackHandle overflow_h = 0;
    CHECK(sceNgs2RackCreate(&valid_param, &overflow_h) == SCE_NGS2_ERROR_OUT_OF_MEMORY);

    // Destrói os racks extras
    for (auto h : extra_racks) {
        CHECK(sceNgs2RackDestroy(h) == SCE_NGS2_OK);
    }
    CHECK(Ngs2Manager::instance().active_racks_count() == 1);

    // Teste de destruição do rack principal
    CHECK(sceNgs2RackDestroy(rack_h) == SCE_NGS2_OK);
    CHECK(!Ngs2Manager::instance().is_rack_valid(rack_h));
    CHECK(Ngs2Manager::instance().active_racks_count() == 0);

    // Destruição repetida ou de handle inválido
    CHECK(sceNgs2RackDestroy(rack_h) == SCE_NGS2_ERROR_INVALID_HANDLE);
    CHECK(sceNgs2RackDestroy(-1) == SCE_NGS2_ERROR_INVALID_HANDLE);
    CHECK(sceNgs2RackDestroy(99999) == SCE_NGS2_ERROR_INVALID_HANDLE);

    // Sobrecarga de conveniência: sceNgs2RackCreate(maxVoices, channels, blockSize, outHandle)
    CHECK(sceNgs2RackCreate(8, 2, 256, &rack_h) == SCE_NGS2_OK);
    CHECK(rack_h > 0);
    CHECK(Ngs2Manager::instance().active_racks_count() == 1);

    // ========================================================================
    // 2. Controle de Estado de Vozes (Play, Pause, Stop)
    // ========================================================================
    // Validação de índice fora do rack (rack tem 8 vozes, índices 0..7)
    CHECK(sceNgs2VoicePlay(rack_h, 8) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceStop(rack_h, 8) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoicePause(rack_h, 8) == SCE_NGS2_ERROR_INVALID_PARAM);
    std::int32_t v_state = -1;
    CHECK(sceNgs2VoiceGetState(rack_h, 8, &v_state) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, nullptr) == SCE_NGS2_ERROR_INVALID_POINTER);

    // Estado inicial deve ser STOPPED
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_STOPPED);

    // Play -> PLAYING
    CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_PLAYING);

    // Verifica que active_voices no rack agora é 1
    CHECK(sceNgs2RackGetInfo(rack_h, &rack_info) == SCE_NGS2_OK);
    CHECK(rack_info.active_voices == 1);

    // Pause -> PAUSED
    CHECK(sceNgs2VoicePause(rack_h, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_PAUSED);

    // Play a partir de PAUSED -> PLAYING
    CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_PLAYING);

    // Stop -> STOPPED
    CHECK(sceNgs2VoiceStop(rack_h, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(rack_h, 0, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_STOPPED);

    // Teste da API baseada em SceNgs2VoiceHandle
    SceNgs2VoiceHandle v_handle = sceNgs2MakeVoiceHandle(rack_h, 1);
    CHECK(sceNgs2VoiceHandleGetRack(v_handle) == rack_h);
    CHECK(sceNgs2VoiceHandleGetIndex(v_handle) == 1);

    CHECK(sceNgs2VoicePlay(v_handle) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(v_handle, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_PLAYING);

    CHECK(sceNgs2VoicePause(v_handle) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(v_handle, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_PAUSED);

    CHECK(sceNgs2VoiceStop(v_handle) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetState(v_handle, &v_state) == SCE_NGS2_OK);
    CHECK(v_state == SCE_NGS2_VOICE_STATE_STOPPED);

    // ========================================================================
    // 3. Modulação de Parâmetros: Volume, Pitch, Pan e Áudio Espacial 3D
    // ========================================================================
    // Validação de Volume: [0.0f .. 10.0f]
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, -0.1f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, 10.1f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, NAN) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, INFINITY) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, 0.75f) == SCE_NGS2_OK);

    // Validação de Pan: [-1.0f .. +1.0f]
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, -1.01f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, 1.01f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, NAN) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, -0.5f) == SCE_NGS2_OK);

    // Validação de Pitch: (0.0f .. 16.0f]
    CHECK(sceNgs2VoiceSetPitch(rack_h, 0, 0.0f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPitch(rack_h, 0, -1.0f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPitch(rack_h, 0, 16.5f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPitch(rack_h, 0, NAN) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPitch(rack_h, 0, 1.5f) == SCE_NGS2_OK);

    // Validação de Posição Espacial 3D
    CHECK(sceNgs2VoiceSetPosition3D(rack_h, 0, NAN, 0.0f, 0.0f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetPosition3D(rack_h, 0, 10.0f, 2.0f, -5.0f) == SCE_NGS2_OK);

    // Validação de Atenuação por Distância
    CHECK(sceNgs2VoiceSetDistanceAttenuation(rack_h, 0, -1.0f, 10.0f, 1.0f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetDistanceAttenuation(rack_h, 0, 10.0f, 5.0f, 1.0f) == SCE_NGS2_ERROR_INVALID_PARAM); // max < min
    CHECK(sceNgs2VoiceSetDistanceAttenuation(rack_h, 0, 1.0f, 10.0f, -0.5f) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetDistanceAttenuation(rack_h, 0, 2.0f, 50.0f, 1.0f) == SCE_NGS2_OK);

    // Consulta de parâmetros atualizados na voz
    SceNgs2VoiceInfo v_info{};
    CHECK(sceNgs2VoiceGetInfo(rack_h, 0, &v_info) == SCE_NGS2_OK);
    CHECK(std::abs(v_info.volume - 0.75f) < 1e-4f);
    CHECK(std::abs(v_info.pan - (-0.5f)) < 1e-4f);
    CHECK(std::abs(v_info.pitch - 1.5f) < 1e-4f);
    CHECK(v_info.has_position_3d);
    CHECK(std::abs(v_info.position_3d.x - 10.0f) < 1e-4f);
    CHECK(std::abs(v_info.min_distance - 2.0f) < 1e-4f);
    CHECK(std::abs(v_info.max_distance - 50.0f) < 1e-4f);

    // Modulação equivalente usando voice handle
    CHECK(sceNgs2VoiceSetVolume(v_handle, 0.6f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPan(v_handle, 0.25f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPitch(v_handle, 1.2f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceGetInfo(v_handle, &v_info) == SCE_NGS2_OK);
    CHECK(std::abs(v_info.volume - 0.6f) < 1e-4f);
    CHECK(std::abs(v_info.pan - 0.25f) < 1e-4f);
    CHECK(std::abs(v_info.pitch - 1.2f) < 1e-4f);

    // ========================================================================
    // 4. Configuração de Buffers (Host, Float, GuestMemory e Sintético)
    // ========================================================================
    // Configuração com buffer PCM16 nulo ou 0 frames
    std::vector<std::int16_t> pcm_test(512, 10000);
    CHECK(sceNgs2VoiceSetBuffer(rack_h, 0, nullptr, 512, 2) == SCE_NGS2_ERROR_INVALID_POINTER);
    CHECK(sceNgs2VoiceSetBuffer(rack_h, 0, pcm_test.data(), 0, 2) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetBuffer(rack_h, 0, pcm_test.data(), 512, 0) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Configuração com sucesso de buffer PCM16
    CHECK(sceNgs2VoiceSetBuffer(rack_h, 0, pcm_test.data(), 256, 2, false) == SCE_NGS2_OK);

    // Configuração com buffer Float
    std::vector<float> float_test(512, 0.5f);
    CHECK(sceNgs2VoiceSetBufferFloat(rack_h, 1, float_test.data(), 256, 2, true) == SCE_NGS2_OK);

    // Configuração de forma de onda senoidal sintética (440 Hz, amplitude 0.8)
    CHECK(sceNgs2VoiceSetSyntheticWaveform(rack_h, 2, 440.0f, 0.8f, true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetSyntheticWaveform(rack_h, 2, -10.0f, 0.8f, true) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetSyntheticWaveform(rack_h, 2, 440.0f, -0.5f, true) == SCE_NGS2_ERROR_INVALID_PARAM);

    // Configuração a partir de GuestMemory (SCE HLE)
    GuestMemory guest_mem(1024 * 1024);
    std::uint64_t guest_addr = 0x20000;
    std::size_t guest_size = 256 * 2 * sizeof(std::int16_t); // 256 frames stereo
    CHECK(guest_mem.map(guest_addr, guest_size, Permission::read | Permission::write) == MemoryError::ok);

    std::vector<std::int16_t> guest_pcm(256 * 2, 12000);
    CHECK(guest_mem.store(guest_addr, guest_pcm.data(), guest_size) == MemoryError::ok);

    // Endereço nulo ou desalinhado
    CHECK(sceNgs2VoiceSetBufferHle(rack_h, 3, guest_mem, 0, guest_size, 2) == SCE_NGS2_ERROR_INVALID_POINTER);
    CHECK(sceNgs2VoiceSetBufferHle(rack_h, 3, guest_mem, guest_addr, guest_size + 1, 2) == SCE_NGS2_ERROR_INVALID_PARAM);
    CHECK(sceNgs2VoiceSetBufferHle(rack_h, 3, guest_mem, 0x900000, guest_size, 2) == SCE_NGS2_ERROR_INVALID_POINTER);

    // Sucesso via GuestMemory
    CHECK(sceNgs2VoiceSetBufferHle(rack_h, 3, guest_mem, guest_addr, guest_size, 2, false) == SCE_NGS2_OK);

    // Limpeza do rack de teste preliminar
    CHECK(sceNgs2RackDestroy(rack_h) == SCE_NGS2_OK);

    // ========================================================================
    // 5. Síntese Determinística: Pan, Volume, Pitch e Renderização de Bloco
    // ========================================================================
    CHECK(sceNgs2RackCreate(4, 2, 256, &rack_h) == SCE_NGS2_OK);

    // Teste 5.1: Pan extremo para a esquerda (-1.0f)
    // Com Pan -1.0f: Canal Esquerdo recebe o sinal com ganho máximo; Canal Direito é SILÊNCIO (0).
    std::vector<std::int16_t> const_samples(256, 16000); // 256 frames mono
    CHECK(sceNgs2VoiceSetBuffer(rack_h, 0, const_samples.data(), 256, 1, true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, 1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, -1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);

    std::vector<std::int16_t> rendered_pcm(256 * 2, 0);
    std::size_t frames_rendered = 0;
    CHECK(sceNgs2RackRender(rack_h, rendered_pcm.data(), 256, &frames_rendered) == SCE_NGS2_OK);
    CHECK(frames_rendered == 256);

    for (std::size_t i = 0; i < 256; ++i) {
        // Canal esquerdo deve conter amostras próximas de 16000 (+- 1 erro de quantização)
        CHECK(std::abs(rendered_pcm[i * 2 + 0] - 16000) <= 2);
        // Canal direito deve ser exatamente zero
        CHECK(rendered_pcm[i * 2 + 1] == 0);
    }

    // Teste 5.2: Pan extremo para a direita (+1.0f)
    // Canal Esquerdo = 0; Canal Direito = 16000.
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, 1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2RackRender(rack_h, rendered_pcm.data(), 256, &frames_rendered) == SCE_NGS2_OK);
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(rendered_pcm[i * 2 + 0] == 0);
        CHECK(std::abs(rendered_pcm[i * 2 + 1] - 16000) <= 2);
    }

    // Teste 5.3: Atenuação por Volume (Volume = 0.5f, Pan = -1.0f)
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, -1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, 0.5f) == SCE_NGS2_OK);
    CHECK(sceNgs2RackRender(rack_h, rendered_pcm.data(), 256, &frames_rendered) == SCE_NGS2_OK);
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(std::abs(rendered_pcm[i * 2 + 0] - 8000) <= 2);
        CHECK(rendered_pcm[i * 2 + 1] == 0);
    }

    // Teste 5.4: Modulação de Pitch (velocidade 2x)
    // Prepara um buffer com rampa linear: sample[i] = i * 10
    std::vector<std::int16_t> ramp(512);
    for (std::size_t i = 0; i < 512; ++i) {
        ramp[i] = static_cast<std::int16_t>(i * 10);
    }
    CHECK(sceNgs2VoiceStop(rack_h, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetBuffer(rack_h, 0, ramp.data(), 512, 1, false) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetVolume(rack_h, 0, 1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPan(rack_h, 0, -1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPitch(rack_h, 0, 2.0f) == SCE_NGS2_OK); // Avanço de 2 amostras por frame
    CHECK(sceNgs2VoicePlay(rack_h, 0) == SCE_NGS2_OK);

    CHECK(sceNgs2RackRender(rack_h, rendered_pcm.data(), 256, &frames_rendered) == SCE_NGS2_OK);
    // Frame 0 deve ter ramp[0] = 0; Frame 1 deve ter ramp[2] = 20; Frame 2 -> ramp[4] = 40, etc.
    for (std::size_t i = 0; i < 256; ++i) {
        std::int16_t expected = static_cast<std::int16_t>(i * 20);
        CHECK(std::abs(rendered_pcm[i * 2 + 0] - expected) <= 2);
    }

    CHECK(sceNgs2RackDestroy(rack_h) == SCE_NGS2_OK);

    // ========================================================================
    // 6. Integração e Roteamento Direto para Porta do AudioOutManager
    // ========================================================================
    // Inicializa subsistema AudioOut e abre porta MAIN
    CHECK(sceAudioOutInit() == SCE_OK);
    std::int32_t audio_port = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                                              SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(audio_port > 0);

    // Cria rack NGS2 vinculado à porta aberta
    SceNgs2RackParam rack_routed{};
    rack_routed.maxVoices = 8;
    rack_routed.channels = 2;
    rack_routed.blockSize = 256;
    rack_routed.sampleRate = 48000;
    rack_routed.audioOutPort = audio_port;
    SceNgs2RackHandle routed_rack = 0;
    CHECK(sceNgs2RackCreate(&rack_routed, &routed_rack) == SCE_NGS2_OK);

    // Configura voz 0 com onda senoidal sintética (440 Hz)
    CHECK(sceNgs2VoiceSetSyntheticWaveform(routed_rack, 0, 440.0f, 0.7f, true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetVolume(routed_rack, 0, 1.0f) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetPan(routed_rack, 0, -0.5f) == SCE_NGS2_OK); // Pan ligeiramente à esquerda
    CHECK(sceNgs2VoicePlay(routed_rack, 0) == SCE_NGS2_OK);

    // Renderiza o rack: este ciclo sintetiza as 256 amostras e as envia à porta do AudioOutManager
    CHECK(sceNgs2RackRender(routed_rack) == SCE_NGS2_OK);

    // Drena amostras da porta de áudio do AudioOutManager e valida o recebimento
    std::vector<std::int16_t> drained_samples(256 * 2, 0);
    std::size_t drained_count = AudioOutManager::instance().drain_port_pcm16(audio_port, drained_samples.data(), 256);
    CHECK(drained_count == 256);

    // Verifica que o buffer não é silencioso e que há sinal com maior energia no canal esquerdo
    std::int64_t energy_left = 0;
    std::int64_t energy_right = 0;
    for (std::size_t i = 0; i < 256; ++i) {
        energy_left += std::abs(static_cast<std::int64_t>(drained_samples[i * 2 + 0]));
        energy_right += std::abs(static_cast<std::int64_t>(drained_samples[i * 2 + 1]));
    }
    CHECK(energy_left > 10000);
    CHECK(energy_right > 10000);
    // Como pan é -0.5f, a energia no canal esquerdo deve ser superior à do direito
    CHECK(energy_left > energy_right);

    // Pausa a voz e renderiza novamente: o PCM resultante deve ser silêncio absoluto
    CHECK(sceNgs2VoicePause(routed_rack, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2RackRender(routed_rack) == SCE_NGS2_OK);

    drained_count = AudioOutManager::instance().drain_port_pcm16(audio_port, drained_samples.data(), 256);
    CHECK(drained_count == 256);
    for (std::size_t i = 0; i < 256 * 2; ++i) {
        CHECK(drained_samples[i] == 0);
    }

    // ========================================================================
    // 7. Destruição do Rack e Drenagem de Vozes Ativas
    // ========================================================================
    // Reativa a voz 0 e inicia a voz 1
    CHECK(sceNgs2VoicePlay(routed_rack, 0) == SCE_NGS2_OK);
    CHECK(sceNgs2VoiceSetSyntheticWaveform(routed_rack, 1, 880.0f, 0.5f, true) == SCE_NGS2_OK);
    CHECK(sceNgs2VoicePlay(routed_rack, 1) == SCE_NGS2_OK);

    CHECK(sceNgs2RackGetInfo(routed_rack, &rack_info) == SCE_NGS2_OK);
    CHECK(rack_info.active_voices == 2);

    // Destrói o rack: deve cessar e drenar todas as vozes ativas
    CHECK(sceNgs2RackDestroy(routed_rack) == SCE_NGS2_OK);

    // Tentativa de renderizar no rack destruído deve falhar com INVALID_HANDLE
    CHECK(sceNgs2RackRender(routed_rack) == SCE_NGS2_ERROR_INVALID_HANDLE);
    CHECK(sceNgs2VoicePlay(routed_rack, 0) == SCE_NGS2_ERROR_INVALID_HANDLE);

    CHECK(sceAudioOutClose(audio_port) == SCE_OK);

    // ========================================================================
    // 8. Concorrência e Thread-Safety entre Threads de Áudio
    // ========================================================================
    std::int32_t conc_port = sceAudioOutOpen(0, SCE_AUDIO_OUT_PORT_TYPE_BGM, 0, 256, 48000,
                                             SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    CHECK(conc_port > 0);

    SceNgs2RackParam conc_param{};
    conc_param.maxVoices = 8;
    conc_param.channels = 2;
    conc_param.blockSize = 256;
    conc_param.sampleRate = 48000;
    conc_param.audioOutPort = conc_port;
    SceNgs2RackHandle conc_rack = 0;
    CHECK(sceNgs2RackCreate(&conc_param, &conc_rack) == SCE_NGS2_OK);

    for (std::uint32_t v = 0; v < 8; ++v) {
        CHECK(sceNgs2VoiceSetSyntheticWaveform(conc_rack, v, 220.0f + static_cast<float>(v * 100), 0.2f, true) == SCE_NGS2_OK);
        CHECK(sceNgs2VoicePlay(conc_rack, v) == SCE_NGS2_OK);
    }

    std::atomic<bool> running{true};
    std::atomic<std::uint32_t> render_cycles{0};
    std::atomic<std::uint32_t> drain_cycles{0};

    // Thread A: Renderiza blocos continuamente para o AudioOut
    std::thread render_thread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            if (sceNgs2RackRender(conc_rack) == SCE_NGS2_OK) {
                render_cycles.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    // Thread B: Modula volume, pan e pitch aleatoriamente/rapidamente
    std::thread modulation_thread([&]() {
        float p = -1.0f;
        while (running.load(std::memory_order_relaxed)) {
            for (std::uint32_t v = 0; v < 8; ++v) {
                sceNgs2VoiceSetVolume(conc_rack, v, 0.5f);
                sceNgs2VoiceSetPan(conc_rack, v, p);
                sceNgs2VoiceSetPitch(conc_rack, v, 1.0f);
            }
            p = -p;
            std::this_thread::yield();
        }
    });

    // Thread C: Alterna estado de play/pause de vozes concorrentemente
    std::thread state_thread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            for (std::uint32_t v = 0; v < 8; ++v) {
                sceNgs2VoicePause(conc_rack, v);
                sceNgs2VoicePlay(conc_rack, v);
            }
            std::this_thread::yield();
        }
    });

    // Thread D: Drena periodicamente amostras do ring buffer da porta
    std::thread drain_thread([&]() {
        std::vector<std::int16_t> drain_buf(256 * 2);
        while (running.load(std::memory_order_relaxed)) {
            std::size_t r = AudioOutManager::instance().drain_port_pcm16(conc_port, drain_buf.data(), 256);
            if (r > 0) {
                drain_cycles.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    // Executa por 50 milissegundos de carga concorrente contínua
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_relaxed);

    render_thread.join();
    modulation_thread.join();
    state_thread.join();
    drain_thread.join();

    CHECK(render_cycles.load() > 0);
    CHECK(drain_cycles.load() > 0);

    CHECK(sceNgs2RackDestroy(conc_rack) == SCE_NGS2_OK);
    CHECK(sceAudioOutClose(conc_port) == SCE_OK);

    // Limpeza final do ambiente
    Ngs2Manager::instance().destroy_all_racks();
    AudioOutManager::instance().close_all_ports();
}); }
