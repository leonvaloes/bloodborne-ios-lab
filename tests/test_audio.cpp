#include "check.h"
#include "audio.h"
#include "atrac9_decoder.h"
#include "guest_memory.h"
#include <vector>
#include <cmath>
#include <cstring>

int main() { return run([] {
    using namespace bb;

    // ========================================================================
    // 1. Audio Ring Buffer Básico
    // ========================================================================
    AudioRingBuffer ring(16, 2); // 16 frames estéreo
    CHECK(ring.available_read() == 0);
    CHECK(ring.available_write() == 16);

    std::vector<std::int16_t> in_samples = {10, 20, 30, 40, 50, 60, 70, 80}; // 4 frames
    CHECK(ring.write(in_samples.data(), 4) == 4);
    CHECK(ring.available_read() == 4);
    CHECK(ring.available_write() == 12);

    std::vector<std::int16_t> out_samples(8, 0);
    CHECK(ring.read(out_samples.data(), 4) == 4);
    CHECK(out_samples == in_samples);
    CHECK(ring.available_read() == 0);

    // Teste de wrap-around (passando do fim do buffer circular)
    std::vector<std::int16_t> chunk1(14 * 2, static_cast<std::int16_t>(100)); // 14 frames
    CHECK(ring.write(chunk1.data(), 14) == 14);
    std::vector<std::int16_t> dummy(10 * 2, static_cast<std::int16_t>(0));
    CHECK(ring.read(dummy.data(), 10) == 10); // Avança read_pos_ para 10

    // Agora há 4 frames no buffer (pos 10..14). Escreve 8 frames, o que deve dar a volta (wrap)
    std::vector<std::int16_t> chunk2(8 * 2, static_cast<std::int16_t>(200));
    CHECK(ring.write(chunk2.data(), 8) == 8);
    CHECK(ring.available_read() == 12);

    std::vector<std::int16_t> wrap_read(12 * 2, static_cast<std::int16_t>(0));
    CHECK(ring.read(wrap_read.data(), 12) == 12);
    CHECK(wrap_read[0] == 100);       // Primeiros 4 frames vinham de chunk1
    CHECK(wrap_read[4 * 2] == 200);   // Próximos 8 frames vinham de chunk2

    // Teste de underflow (solicitar mais do que há disponível)
    std::vector<std::int16_t> underflow_buf(4 * 2, static_cast<std::int16_t>(999));
    CHECK(ring.read(underflow_buf.data(), 4) == 0); // 0 disponíveis
    CHECK(underflow_buf[0] == 0); // Deve ser preenchido com silêncio (0)
    auto stats = ring.get_stats();
    CHECK(stats.underflow_count >= 1);

    // ========================================================================
    // 2. Audio Manager: Volume, Mute, Geração de Onda
    // ========================================================================
    AudioManager audio;
    AudioConfig cfg{48000, 2, 128};
    CHECK(audio.init(cfg));
    CHECK(audio.get_master_volume() == 1.0f);

    audio.set_master_volume(0.5f);
    CHECK(audio.get_master_volume() == 0.5f);

    std::vector<std::int16_t> vol_test = {1000, 2000}; // 1 frame
    audio.submit_pcm16(vol_test.data(), 1);

    std::vector<std::int16_t> drained(2, static_cast<std::int16_t>(0));
    audio.drain_pcm16(drained.data(), 1);
    CHECK(drained[0] == 500); // 1000 * 0.5
    CHECK(drained[1] == 1000); // 2000 * 0.5

    // Mute
    audio.set_mute(true);
    CHECK(audio.is_muted());
    audio.submit_pcm16(vol_test.data(), 1);
    audio.drain_pcm16(drained.data(), 1);
    CHECK(drained[0] == 0 && drained[1] == 0);

    // Gerador de tom de teste senoidal
    auto sine = audio.generate_sine_wave(440.0f, 0.01f, 0.8f);
    CHECK(!sine.empty());
    CHECK(sine.size() == static_cast<std::size_t>(48000 * 0.01f) * 2);

    // Integração HLE a partir de memória guest
    GuestMemory mem(512);
    CHECK(mem.map(0x2000, 512, read | write) == MemoryError::ok);
    std::int16_t guest_audio[4] = {300, 400, 500, 600}; // 2 frames
    CHECK(mem.store(0x2000, guest_audio, sizeof(guest_audio)) == MemoryError::ok);

    audio.set_mute(false);
    audio.set_master_volume(1.0f);
    CHECK(audio.submit_hle_audio(mem, 0x2000, 2) == MemoryError::ok);

    std::vector<std::int16_t> hle_drained(4, static_cast<std::int16_t>(0));
    CHECK(audio.drain_pcm16(hle_drained.data(), 2) == 2);
    CHECK(hle_drained[0] == 300 && hle_drained[1] == 400);

    // ========================================================================
    // 3. LibAtrac9 Decoder: Configuração e Metadados
    // ========================================================================
    // Criação de configuração ATRAC9 para 48.000 Hz, Estéreo, 192 bytes por frame
    auto at9_cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0);
    CHECK(at9_cfg[0] == 0xFE);
    CHECK(at9_cfg[1] == 0x74);
    CHECK(at9_cfg[2] == 0x17);
    CHECK(at9_cfg[3] == 0xE0);

    Atrac9Decoder decoder;
    CHECK(!decoder.is_initialized());
    CHECK(decoder.init(at9_cfg.data()));
    CHECK(decoder.is_initialized());

    const auto &info = decoder.get_info();
    CHECK(info.channels == 2);
    CHECK(info.sample_rate == 48000);
    CHECK(info.frame_samples == 256);
    CHECK(info.superframe_size == 192);
    CHECK(info.frames_in_superframe == 1);
    CHECK(info.config_data == at9_cfg);

    // Rejeição de configuração inválida (header != 0xFE)
    std::uint8_t bad_cfg[4] = {0xAA, 0x74, 0x17, 0xE0};
    Atrac9Decoder bad_decoder;
    CHECK(!bad_decoder.init(bad_cfg));
    CHECK(!bad_decoder.is_initialized());

    // Inicialização via cabeçalho RIFF WAVE com chunk 'fmt '
    std::vector<std::uint8_t> riff(70, 0);
    std::memcpy(riff.data(), "RIFF", 4);
    std::uint32_t riff_len = 62;
    std::memcpy(riff.data() + 4, &riff_len, 4);
    std::memcpy(riff.data() + 8, "WAVE", 4);
    std::memcpy(riff.data() + 12, "fmt ", 4);
    std::uint32_t fmt_len = 48;
    std::memcpy(riff.data() + 16, &fmt_len, 4);
    // ConfigData em offset 44 do chunk fmt (offset 12 + 8 + 44 = 64)
    std::memcpy(riff.data() + 64, at9_cfg.data(), 4);

    Atrac9Decoder riff_decoder;
    CHECK(riff_decoder.init_from_riff(riff.data(), riff.size()));
    CHECK(riff_decoder.is_initialized());
    CHECK(riff_decoder.get_info().sample_rate == 48000);
    CHECK(riff_decoder.get_info().channels == 2);

    // ========================================================================
    // 4. Integração ATRAC9 no AudioManager e Ring Buffer Estéreo 48 kHz
    // ========================================================================
    CHECK(audio.init_atrac9(at9_cfg.data()));
    CHECK(audio.has_atrac9_decoder());
    CHECK(audio.atrac9_decoder()->get_info().sample_rate == 48000);

    // Inicialização e decodificação Mono (1 canal) -> Upmix Estéreo
    auto mono_cfg = Atrac9Decoder::make_config_data(7, 0, 96, 0); // index 0 = mono
    Atrac9Decoder mono_decoder;
    CHECK(mono_decoder.init(mono_cfg.data()));
    CHECK(mono_decoder.get_info().channels == 1);
    CHECK(mono_decoder.get_info().sample_rate == 48000);

    // Inicialização 24.000 Hz -> Reamostragem para 48.000 Hz
    auto resample_cfg = Atrac9Decoder::make_config_data(4, 2, 96, 0); // index 4 = 24 kHz
    Atrac9Decoder resample_decoder;
    CHECK(resample_decoder.init(resample_cfg.data()));
    CHECK(resample_decoder.get_info().sample_rate == 24000);

    // Teste de submissão ATRAC9 HLE a partir de memória guest
    std::vector<std::uint8_t> fake_stream(192, 0);
    CHECK(mem.store(0x2040, fake_stream.data(), fake_stream.size()) == MemoryError::ok);
    CHECK(audio.submit_hle_atrac9(mem, 0x2040, fake_stream.size(), at9_cfg.data()) == MemoryError::ok);

    std::cout << "audio: Ring buffer, AudioManager and LibAtrac9 48kHz stereo passed\n";
}); }
