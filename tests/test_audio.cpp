#include "check.h"
#include "audio.h"
#include "guest_memory.h"
#include <vector>
#include <cmath>

int main() { return run([] {
    using namespace bb;

    // Teste do Ring Buffer básico
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
    std::vector<std::int16_t> chunk1(14 * 2, 100); // 14 frames
    CHECK(ring.write(chunk1.data(), 14) == 14);
    std::vector<std::int16_t> dummy(10 * 2, 0);
    CHECK(ring.read(dummy.data(), 10) == 10); // Avança read_pos_ para 10

    // Agora há 4 frames no buffer (pos 10..14). Escreve 8 frames, o que deve dar a volta (wrap)
    std::vector<std::int16_t> chunk2(8 * 2, 200);
    CHECK(ring.write(chunk2.data(), 8) == 8);
    CHECK(ring.available_read() == 12);

    std::vector<std::int16_t> wrap_read(12 * 2, 0);
    CHECK(ring.read(wrap_read.data(), 12) == 12);
    CHECK(wrap_read[0] == 100);       // Primeiros 4 frames vinham de chunk1
    CHECK(wrap_read[4 * 2] == 200);   // Próximos 8 frames vinham de chunk2

    // Teste de underflow (solicitar mais do que há disponível)
    std::vector<std::int16_t> underflow_buf(4 * 2, 999);
    CHECK(ring.read(underflow_buf.data(), 4) == 0); // 0 disponíveis
    CHECK(underflow_buf[0] == 0); // Deve ser preenchido com silêncio (0)
    auto stats = ring.get_stats();
    CHECK(stats.underflow_count >= 1);

    // Teste do AudioManager com volume e mute
    AudioManager audio;
    AudioConfig cfg{48000, 2, 128};
    CHECK(audio.init(cfg));
    CHECK(audio.get_master_volume() == 1.0f);

    audio.set_master_volume(0.5f);
    CHECK(audio.get_master_volume() == 0.5f);

    std::vector<std::int16_t> vol_test = {1000, 2000}; // 1 frame
    audio.submit_pcm16(vol_test.data(), 1);

    std::vector<std::int16_t> drained(2, 0);
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
    GuestMemory mem(64);
    CHECK(mem.map(0x2000, 64, read | write) == MemoryError::ok);
    std::int16_t guest_audio[4] = {300, 400, 500, 600}; // 2 frames
    CHECK(mem.store(0x2000, guest_audio, sizeof(guest_audio)) == MemoryError::ok);

    audio.set_mute(false);
    audio.set_master_volume(1.0f);
    CHECK(audio.submit_hle_audio(mem, 0x2000, 2) == MemoryError::ok);

    std::vector<std::int16_t> hle_drained(4, 0);
    CHECK(audio.drain_pcm16(hle_drained.data(), 2) == 2);
    CHECK(hle_drained[0] == 300 && hle_drained[1] == 400);
}); }
