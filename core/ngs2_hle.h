#ifndef BB_NGS2_HLE_H
#define BB_NGS2_HLE_H

#include "audio_out.h"
#include "guest_memory.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace bb {

// ============================================================================
// Constantes e Tipos do subsistema SCE NGS2 (libSceNgs2 no Orbis OS)
// ============================================================================

constexpr std::uint32_t SCE_NGS2_MAX_VOICES_PER_RACK = 64;
constexpr std::size_t   SCE_NGS2_MAX_RACKS           = 16;
constexpr std::uint32_t SCE_NGS2_DEFAULT_SAMPLE_RATE = 48000;
constexpr std::uint32_t SCE_NGS2_MAX_BLOCK_SIZE      = 4096;

// Códigos de erro PS4 SCE NGS2
constexpr std::int32_t SCE_NGS2_OK                         = 0;
constexpr std::int32_t SCE_NGS2_ERROR_INVALID_PARAM        = static_cast<std::int32_t>(0x805A0001);
constexpr std::int32_t SCE_NGS2_ERROR_INVALID_HANDLE       = static_cast<std::int32_t>(0x805A0002);
constexpr std::int32_t SCE_NGS2_ERROR_INVALID_POINTER      = static_cast<std::int32_t>(0x805A0003);
constexpr std::int32_t SCE_NGS2_ERROR_OUT_OF_MEMORY        = static_cast<std::int32_t>(0x805A0004);
constexpr std::int32_t SCE_NGS2_ERROR_VOICE_LIMIT_EXCEEDED = static_cast<std::int32_t>(0x805A0005);
constexpr std::int32_t SCE_NGS2_ERROR_BUSY                 = static_cast<std::int32_t>(0x805A0006);
constexpr std::int32_t SCE_NGS2_ERROR_NOT_INITIALIZED      = static_cast<std::int32_t>(0x805A0007);
constexpr std::int32_t SCE_NGS2_ERROR_PORT_UNAVAILABLE     = static_cast<std::int32_t>(0x805A0008);

enum SceNgs2VoiceState : std::int32_t {
    SCE_NGS2_VOICE_STATE_STOPPED = 0,
    SCE_NGS2_VOICE_STATE_PLAYING = 1,
    SCE_NGS2_VOICE_STATE_PAUSED  = 2,
};

using SceNgs2RackHandle  = std::int32_t;
using SceNgs2VoiceHandle = std::uint64_t;

// Parâmetros de criação de Voice Rack
struct SceNgs2RackParam {
    std::uint32_t maxVoices{32};          // Máximo de vozes no rack (1 .. 64)
    std::uint32_t channels{2};            // Canais de saída (1 = mono, 2 = stereo)
    std::uint32_t blockSize{256};         // Tamanho de bloco por renderização (1 .. 4096 frames)
    std::uint32_t sampleRate{48000};      // Frequência de amostragem em Hz (padrão 48000)
    std::int32_t audioOutPort{-1};        // Porta vinculada do AudioOutManager (-1 se nenhuma)
    std::uint32_t reserved[4]{0, 0, 0, 0};
};

// Coordenadas para áudio espacial 3D
struct SceNgs2Position3D {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// Diagnóstico e estado de uma voz individual
struct SceNgs2VoiceInfo {
    std::int32_t state{SCE_NGS2_VOICE_STATE_STOPPED};
    float volume{1.0f};
    float pan{0.0f};
    float pitch{1.0f};
    bool loop{false};
    bool has_position_3d{false};
    SceNgs2Position3D position_3d{0.0f, 0.0f, 0.0f};
    float min_distance{1.0f};
    float max_distance{100.0f};
    float rolloff{1.0f};
    std::uint64_t frames_rendered{0};
};

// Informações de status de um Rack
struct SceNgs2RackInfo {
    SceNgs2RackHandle handle{0};
    std::uint32_t max_voices{0};
    std::uint32_t active_voices{0};
    std::uint32_t channels{0};
    std::uint32_t block_size{0};
    std::uint32_t sample_rate{0};
    std::int32_t audio_out_port{-1};
    std::uint64_t total_blocks_rendered{0};
};

// Funções utilitárias de codificação de voice handle
inline SceNgs2VoiceHandle sceNgs2MakeVoiceHandle(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rackHandle)) << 32) |
           (static_cast<std::uint64_t>(voiceIndex) & 0xFFFFFFFFULL);
}

inline SceNgs2RackHandle sceNgs2VoiceHandleGetRack(SceNgs2VoiceHandle voiceHandle) {
    return static_cast<SceNgs2RackHandle>(static_cast<std::uint32_t>(voiceHandle >> 32));
}

inline std::uint32_t sceNgs2VoiceHandleGetIndex(SceNgs2VoiceHandle voiceHandle) {
    return static_cast<std::uint32_t>(voiceHandle & 0xFFFFFFFFULL);
}

// ============================================================================
// Ngs2Manager: Gerenciador de Racks, Síntese e Roteamento
// ============================================================================

class Ngs2Manager {
public:
    Ngs2Manager();
    ~Ngs2Manager();

    static Ngs2Manager& instance();

    // Ciclo de vida de Voice Racks
    std::int32_t create_rack(const SceNgs2RackParam *param, SceNgs2RackHandle *out_handle);
    std::int32_t destroy_rack(SceNgs2RackHandle handle);
    bool is_rack_valid(SceNgs2RackHandle handle) const;
    std::size_t active_racks_count() const;
    void destroy_all_racks();

    // Associação de porta do AudioOutManager
    std::int32_t set_rack_audio_out_port(SceNgs2RackHandle rack_handle, std::int32_t audio_out_port);
    std::int32_t get_rack_audio_out_port(SceNgs2RackHandle rack_handle, std::int32_t *out_port) const;

    // Diagnóstico
    std::int32_t get_rack_info(SceNgs2RackHandle rack_handle, SceNgs2RackInfo *out_info) const;
    std::int32_t get_voice_info(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, SceNgs2VoiceInfo *out_info) const;

    // Controle de playback de vozes individuais
    std::int32_t voice_play(SceNgs2RackHandle rack_handle, std::uint32_t voice_index);
    std::int32_t voice_stop(SceNgs2RackHandle rack_handle, std::uint32_t voice_index);
    std::int32_t voice_pause(SceNgs2RackHandle rack_handle, std::uint32_t voice_index);
    std::int32_t voice_get_state(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, std::int32_t *out_state) const;

    // Parâmetros de áudio (volume, pan estéreo, pitch)
    std::int32_t voice_set_volume(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float volume);
    std::int32_t voice_set_pan(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float pan);
    std::int32_t voice_set_pitch(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float pitch);

    // Parâmetros de áudio espacial 3D (posição e atenuação por distância)
    std::int32_t voice_set_position_3d(SceNgs2RackHandle rack_handle, std::uint32_t voice_index, float x, float y, float z);
    std::int32_t voice_set_distance_attenuation(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                               float min_distance, float max_distance, float rolloff);

    // Configuração de buffers de forma de onda / amostras
    std::int32_t voice_set_buffer(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                  const std::int16_t *pcm_data, std::size_t num_frames,
                                  std::uint32_t channels = 2, bool loop = false);
    std::int32_t voice_set_buffer_float(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                        const float *pcm_data, std::size_t num_frames,
                                        std::uint32_t channels = 2, bool loop = false);
    std::int32_t voice_set_buffer_hle(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                      const GuestMemory &mem, std::uint64_t guest_addr,
                                      std::size_t num_bytes, std::uint32_t channels = 2, bool loop = false);
    std::int32_t voice_set_synthetic(SceNgs2RackHandle rack_handle, std::uint32_t voice_index,
                                     float freq_hz, float amplitude = 1.0f, bool loop = true);

    // Síntese e Renderização do Rack
    // Processa vozes ativas, modula volume, pitch e pan, e submete o PCM resultante à porta AudioOut
    std::int32_t render_rack(SceNgs2RackHandle rack_handle,
                             std::int32_t override_audio_out_port = -1,
                             std::int16_t *out_pcm = nullptr,
                             std::size_t max_frames = 0,
                             std::size_t *frames_rendered = nullptr,
                             AudioOutManager *audio_out_mgr = nullptr);

private:
    enum class VoiceSourceType {
        None,
        SyntheticSine,
        PcmBuffer
    };

    struct Voice {
        std::uint32_t id{0};
        SceNgs2VoiceState state{SCE_NGS2_VOICE_STATE_STOPPED};

        float volume{1.0f};
        float pan{0.0f};
        float pitch{1.0f};

        bool has_position_3d{false};
        SceNgs2Position3D position_3d{0.0f, 0.0f, 0.0f};
        float min_distance{1.0f};
        float max_distance{100.0f};
        float rolloff{1.0f};

        VoiceSourceType source_type{VoiceSourceType::None};
        std::vector<float> samples; // Amostras normalizadas [-1.0f, +1.0f]
        std::size_t sample_count{0}; // Total de frames
        std::uint32_t buffer_channels{1};
        double playback_pos{0.0};
        bool loop{false};

        float synthetic_freq{440.0f};
        float synthetic_amplitude{1.0f};
        double synthetic_phase{0.0};

        std::uint64_t frames_rendered{0};
    };

    struct Rack {
        SceNgs2RackHandle handle{0};
        SceNgs2RackParam param{};
        std::vector<Voice> voices;
        std::uint64_t total_blocks_rendered{0};
        bool is_destroyed{false};
        mutable std::mutex rack_mutex;
    };

    std::shared_ptr<Rack> get_rack(SceNgs2RackHandle handle) const;

    mutable std::mutex manager_mutex_;
    std::unordered_map<SceNgs2RackHandle, std::shared_ptr<Rack>> racks_;
    SceNgs2RackHandle next_rack_handle_{1};
};

// ============================================================================
// Funções C de conveniência HLE estilo SCE (libSceNgs2)
// ============================================================================

std::int32_t sceNgs2RackCreate(const SceNgs2RackParam *param, SceNgs2RackHandle *outHandle);
std::int32_t sceNgs2RackCreate(std::uint32_t maxVoices, std::uint32_t channels,
                               std::uint32_t blockSize, SceNgs2RackHandle *outHandle);
std::int32_t sceNgs2RackDestroy(SceNgs2RackHandle handle);
std::int32_t sceNgs2RackGetInfo(SceNgs2RackHandle handle, SceNgs2RackInfo *outInfo);
std::int32_t sceNgs2RackSetAudioOutPort(SceNgs2RackHandle handle, std::int32_t audioOutPort);
std::int32_t sceNgs2RackGetAudioOutPort(SceNgs2RackHandle handle, std::int32_t *outPort);

std::int32_t sceNgs2VoicePlay(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex);
std::int32_t sceNgs2VoicePlay(SceNgs2VoiceHandle voiceHandle);

std::int32_t sceNgs2VoiceStop(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex);
std::int32_t sceNgs2VoiceStop(SceNgs2VoiceHandle voiceHandle);

std::int32_t sceNgs2VoicePause(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex);
std::int32_t sceNgs2VoicePause(SceNgs2VoiceHandle voiceHandle);

std::int32_t sceNgs2VoiceGetState(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, std::int32_t *outState);
std::int32_t sceNgs2VoiceGetState(SceNgs2VoiceHandle voiceHandle, std::int32_t *outState);

std::int32_t sceNgs2VoiceGetInfo(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, SceNgs2VoiceInfo *outInfo);
std::int32_t sceNgs2VoiceGetInfo(SceNgs2VoiceHandle voiceHandle, SceNgs2VoiceInfo *outInfo);

std::int32_t sceNgs2VoiceSetVolume(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float volume);
std::int32_t sceNgs2VoiceSetVolume(SceNgs2VoiceHandle voiceHandle, float volume);

std::int32_t sceNgs2VoiceSetPan(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float pan);
std::int32_t sceNgs2VoiceSetPan(SceNgs2VoiceHandle voiceHandle, float pan);

std::int32_t sceNgs2VoiceSetPitch(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float pitch);
std::int32_t sceNgs2VoiceSetPitch(SceNgs2VoiceHandle voiceHandle, float pitch);

std::int32_t sceNgs2VoiceSetPosition3D(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex, float x, float y, float z);
std::int32_t sceNgs2VoiceSetPosition3D(SceNgs2VoiceHandle voiceHandle, float x, float y, float z);

std::int32_t sceNgs2VoiceSetDistanceAttenuation(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                                float minDistance, float maxDistance, float rolloff);
std::int32_t sceNgs2VoiceSetDistanceAttenuation(SceNgs2VoiceHandle voiceHandle,
                                                float minDistance, float maxDistance, float rolloff);

std::int32_t sceNgs2VoiceSetBuffer(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                   const std::int16_t *pcmData, std::size_t numFrames,
                                   std::uint32_t channels = 2, bool loop = false);
std::int32_t sceNgs2VoiceSetBuffer(SceNgs2VoiceHandle voiceHandle,
                                   const std::int16_t *pcmData, std::size_t numFrames,
                                   std::uint32_t channels = 2, bool loop = false);

std::int32_t sceNgs2VoiceSetBufferFloat(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                        const float *pcmData, std::size_t numFrames,
                                        std::uint32_t channels = 2, bool loop = false);
std::int32_t sceNgs2VoiceSetBufferFloat(SceNgs2VoiceHandle voiceHandle,
                                        const float *pcmData, std::size_t numFrames,
                                        std::uint32_t channels = 2, bool loop = false);

std::int32_t sceNgs2VoiceSetBufferHle(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                      const GuestMemory &mem, std::uint64_t guest_addr,
                                      std::size_t numBytes, std::uint32_t channels = 2, bool loop = false);
std::int32_t sceNgs2VoiceSetBufferHle(SceNgs2VoiceHandle voiceHandle,
                                      const GuestMemory &mem, std::uint64_t guest_addr,
                                      std::size_t numBytes, std::uint32_t channels = 2, bool loop = false);

std::int32_t sceNgs2VoiceSetSyntheticWaveform(SceNgs2RackHandle rackHandle, std::uint32_t voiceIndex,
                                              float freqHz, float amplitude = 1.0f, bool loop = true);
std::int32_t sceNgs2VoiceSetSyntheticWaveform(SceNgs2VoiceHandle voiceHandle,
                                              float freqHz, float amplitude = 1.0f, bool loop = true);

std::int32_t sceNgs2RackRender(SceNgs2RackHandle rackHandle);
std::int32_t sceNgs2RackRender(SceNgs2RackHandle rackHandle, std::int32_t audioOutPort);
std::int32_t sceNgs2RackRender(SceNgs2RackHandle rackHandle, std::int16_t *outPcm,
                               std::size_t maxFrames, std::size_t *framesRendered);

} // namespace bb

#endif // BB_NGS2_HLE_H
