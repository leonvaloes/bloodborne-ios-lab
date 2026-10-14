#ifndef BB_AUDIO_OUT_H
#define BB_AUDIO_OUT_H

#include "audio.h"
#include "guest_memory.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <functional>

namespace bb {

// Constantes do subsistema SCE AudioOut (compatíveis com libSceAudioOut no Orbis OS)

enum SceAudioOutPortType : std::int32_t {
    SCE_AUDIO_OUT_PORT_TYPE_MAIN     = 0,
    SCE_AUDIO_OUT_PORT_TYPE_BGM      = 1,
    SCE_AUDIO_OUT_PORT_TYPE_VOICE    = 2,
    SCE_AUDIO_OUT_PORT_TYPE_PERSONAL = 3,
    SCE_AUDIO_OUT_PORT_TYPE_PADSPK   = 4,
    SCE_AUDIO_OUT_PORT_TYPE_AUX      = 5,
};

enum SceAudioOutParamFormat : std::uint32_t {
    SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO     = 0,
    SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO   = 1,
    SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH      = 2,
    SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_MONO   = 3,
    SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO = 4,
    SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_8CH    = 5,
};

enum SceAudioOutVolumeFlag : std::int32_t {
    SCE_AUDIO_OUT_VOLUME_FLAG_LEFT   = 0x01,
    SCE_AUDIO_OUT_VOLUME_FLAG_RIGHT  = 0x02,
    SCE_AUDIO_OUT_VOLUME_FLAG_STEREO = (SCE_AUDIO_OUT_VOLUME_FLAG_LEFT | SCE_AUDIO_OUT_VOLUME_FLAG_RIGHT),
};

// Códigos de erro PS4 SCE
constexpr std::int32_t SCE_OK                                 = 0;
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_NOT_OPENED         = static_cast<std::int32_t>(0x80260001);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_BUSY               = static_cast<std::int32_t>(0x80260002);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_INVALID_PORT       = static_cast<std::int32_t>(0x80260003);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_INVALID_POINTER    = static_cast<std::int32_t>(0x80260004);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_PORT_FULL          = static_cast<std::int32_t>(0x80260005);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_INVALID_PARAM      = static_cast<std::int32_t>(0x80260006);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_OUT_OF_MEMORY      = static_cast<std::int32_t>(0x80260007);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_INVALID_SIZE       = static_cast<std::int32_t>(0x80260008);
constexpr std::int32_t SCE_AUDIO_OUT_ERROR_UNSUPPORTED_FORMAT = static_cast<std::int32_t>(0x80260009);

// Volume de ganho unitário de referência (0 dB = 32768 / 0x8000)
constexpr std::int32_t SCE_AUDIO_OUT_VOLUME_0dB = 32768;

// Informações de status e diagnóstico da porta de áudio
struct SceAudioOutPortInfo {
    std::int32_t status{0};                  // 1 = ativa/aberta, 0 = inativa/fechada
    std::int32_t port_type{0};               // SceAudioOutPortType
    std::int32_t index{0};                   // Índice dentro do tipo da porta
    std::uint32_t samples_per_block{0};      // len (frames por bloco)
    std::uint32_t sample_rate{48000};        // Taxa em Hz (padrão 48.000 Hz)
    std::uint32_t channels{2};               // Número de canais
    std::uint32_t format{SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO};
    std::int32_t volume_left{SCE_AUDIO_OUT_VOLUME_0dB};
    std::int32_t volume_right{SCE_AUDIO_OUT_VOLUME_0dB};
    std::uint64_t total_frames_output{0};    // Total de frames emitidos com sucesso
    std::uint32_t underflow_count{0};        // Contagem de underflows do buffer
    std::uint32_t overflow_count{0};         // Contagem de overflows do buffer
    std::uint64_t last_output_time_us{0};    // Timestamp monotônico (microssegundos)
};

using SceAudioOutPortState = SceAudioOutPortInfo;

class AudioOutManager {
public:
    static constexpr std::size_t MAX_CONCURRENT_PORTS = 16;
    static constexpr std::uint32_t DEFAULT_SAMPLE_RATE = 48000;
    static constexpr std::uint32_t MAX_SAMPLES_PER_BLOCK = 4096;

    using TimeProvider = std::function<std::uint64_t()>;

    AudioOutManager();
    ~AudioOutManager();

    // Ciclo de vida das portas
    std::int32_t open_port(std::int32_t user_id, std::int32_t type, std::int32_t index,
                           std::uint32_t len, std::uint32_t freq, std::uint32_t param);
    std::int32_t close_port(std::int32_t handle);
    bool is_port_open(std::int32_t handle) const;
    std::size_t open_ports_count() const;
    void close_all_ports();

    // Submissão de saída de áudio (PCM16)
    // Buffer host:
    std::int32_t output(std::int32_t handle, const void *ptr, std::size_t frames = 0);
    // Guest memory:
    std::int32_t output_hle(std::int32_t handle, const GuestMemory &mem, std::uint64_t guest_addr, std::size_t frames = 0);

    // Configuração de atenuação / ganho estereofônico
    std::int32_t set_volume(std::int32_t handle, std::int32_t flag, const std::int32_t *vol);
    std::int32_t set_volume(std::int32_t handle, std::int32_t flag, int32_t *vol);
    std::int32_t set_volume_float(std::int32_t handle, float left, float right);

    // Diagnóstico e tempo
    std::int32_t get_port_info(std::int32_t handle, SceAudioOutPortInfo *info) const;
    std::int32_t get_last_output_time(std::int32_t handle, std::uint64_t *output_time) const;

    // Drenagem do ring buffer interno (para consumidor host / testes)
    std::size_t drain_port_pcm16(std::int32_t handle, std::int16_t *out_samples, std::size_t frames);
    std::size_t drain_main_port_pcm16(std::int16_t *out_samples, std::size_t frames);

    // Acesso ao ring buffer da porta
    AudioRingBuffer* get_port_ring_buffer(std::int32_t handle);

    // Injeção de time provider monotônico para testes determinísticos
    void set_time_provider(TimeProvider provider);
    void reset_time_provider();

    // Instância global / singleton padrão para chamadas de HLE
    static AudioOutManager& instance();

private:
    struct PortEntry {
        std::int32_t handle{0};
        std::int32_t user_id{0};
        std::int32_t type{0};
        std::int32_t index{0};
        std::uint32_t len{0};
        std::uint32_t freq{DEFAULT_SAMPLE_RATE};
        std::uint32_t format{SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO};
        std::uint32_t channels{2};
        std::int32_t volume_left{SCE_AUDIO_OUT_VOLUME_0dB};
        std::int32_t volume_right{SCE_AUDIO_OUT_VOLUME_0dB};
        std::uint64_t total_frames_output{0};
        std::uint64_t last_output_time_us{0};
        bool is_open{false};
        std::unique_ptr<AudioRingBuffer> ring_buffer;
        mutable std::mutex mutex;
    };

    std::uint64_t get_current_time_us() const;

    mutable std::mutex manager_mutex_;
    std::unordered_map<std::int32_t, std::unique_ptr<PortEntry>> ports_;
    std::int32_t next_handle_{1};
    TimeProvider time_provider_{nullptr};
};

// Funções globais HLE estilo SCE C para libSceAudioOut
std::int32_t sceAudioOutInit();
std::int32_t sceAudioOutOpen(std::int32_t userId, std::int32_t type, std::int32_t index,
                             std::uint32_t len, std::uint32_t freq, std::uint32_t param);
std::int32_t sceAudioOutClose(std::int32_t handle);
std::int32_t sceAudioOutOutput(std::int32_t handle, const void *ptr);
std::int32_t sceAudioOutOutputHle(std::int32_t handle, const GuestMemory &mem, std::uint64_t guest_addr);
std::int32_t sceAudioOutSetVolume(std::int32_t handle, std::int32_t flag, const std::int32_t *vol);
std::int32_t sceAudioOutSetVolume(std::int32_t handle, std::int32_t flag, int32_t *vol);
std::int32_t sceAudioOutGetPortInfo(std::int32_t handle, SceAudioOutPortInfo *info);
std::int32_t sceAudioOutGetPortState(std::int32_t handle, SceAudioOutPortInfo *info);
std::int32_t sceAudioOutGetLastOutputTime(std::int32_t handle, std::uint64_t *output_time);

} // namespace bb

#endif // BB_AUDIO_OUT_H
