#ifndef BB_ATRAC9_DECODER_H
#define BB_ATRAC9_DECODER_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include "audio.h"

namespace bb {

struct Atrac9Info {
    int channels{0};
    int channel_config_index{0};
    int sample_rate{0};
    int superframe_size{0};
    int frames_in_superframe{0};
    int frame_samples{0};
    int wlength{0};
    std::array<std::uint8_t, 4> config_data{0, 0, 0, 0};
};

class Atrac9Decoder {
public:
    Atrac9Decoder();
    ~Atrac9Decoder();

    Atrac9Decoder(const Atrac9Decoder &) = delete;
    Atrac9Decoder &operator=(const Atrac9Decoder &) = delete;

    Atrac9Decoder(Atrac9Decoder &&other) noexcept;
    Atrac9Decoder &operator=(Atrac9Decoder &&other) noexcept;

    // Inicializa o decodificador com os 4 bytes de configuração ATRAC9
    bool init(const std::uint8_t config_data[4]);

    // Inicializa o decodificador a partir de um cabeçalho RIFF WAVE com chunk 'fmt '
    bool init_from_riff(const std::uint8_t *riff_data, std::size_t size);

    bool is_initialized() const { return initialized_; }
    const Atrac9Info &get_info() const { return info_; }

    // Decodifica um frame ATRAC9 bruto para PCM de 16 bits (intercalado)
    // pcm_out deve ter capacidade para pelo menos info_.frame_samples * info_.channels amostras
    int decode_frame(const std::uint8_t *atrac9_buf, std::size_t size, std::int16_t *pcm_out, int &bytes_used);

    // Decodifica múltiplos frames de um stream contínuo, convertendo para estéreo 48.000 Hz
    // Retorna a quantidade total de frames estéreo produzidos
    std::size_t decode_stream_to_stereo_48k(const std::uint8_t *atrac9_stream, std::size_t size,
                                            std::vector<std::int16_t> &out_pcm_stereo);

    // Decodifica um stream ATRAC9 e injeta diretamente no AudioRingBuffer em 48.000 Hz estéreo
    std::size_t decode_to_ring_buffer(const std::uint8_t *atrac9_stream, std::size_t size,
                                      AudioRingBuffer &ring_buffer);

    // Cria bytes de configuração ATRAC9 válidos (SampleRate 48k = index 7, Stereo = index 2)
    static std::array<std::uint8_t, 4> make_config_data(int sample_rate_index, int channel_config_index,
                                                        int frame_bytes, int superframe_index);

private:
    void *handle_{nullptr};
    bool initialized_{false};
    Atrac9Info info_{};
};

} // namespace bb

#endif // BB_ATRAC9_DECODER_H
