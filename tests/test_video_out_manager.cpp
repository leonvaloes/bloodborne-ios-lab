#include "check.h"
#include "video_out.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace {
struct Sink : bb::VideoOutSink {
    unsigned flips = 0;
    void on_flip(const bb::FlipEvent&) override { ++flips; }
};
}

int main() { return run([] {
    bb::GuestMemory memory(4096);
    CHECK(memory.map(0x1000, 4096, bb::read | bb::write) == bb::MemoryError::ok);
    Sink sink;
    bb::VideoOutManager manager;
    bb::VideoOutManager capped(17);
    CHECK(capped.register_buffer(16, {0x1000, 2, 2, 2, 4}) == bb::VideoStatus::invalid);
    bb::HostFrameBuffer frame;
    const bb::VideoBuffer buffer{0x1000, 2, 2, 2, 4};
    CHECK(manager.register_buffer(0, buffer) == bb::VideoStatus::ok);
    CHECK(manager.submit_flip(0, 7) == bb::VideoStatus::ok);
    CHECK(manager.has_new_frame());
    CHECK(!manager.read_current_frame_pixels(frame));
    CHECK(!frame.valid && frame.pixel_data.empty()); // modo sintético nunca fabrica pixels
    CHECK(!manager.current_display_buffer().valid);
    CHECK(manager.has_new_frame()); // consultas não consomem o flip
    CHECK(manager.consume_new_frame());
    CHECK(!manager.consume_new_frame());

    manager.attach_guest_memory(&memory, &sink);
    CHECK(manager.flip_count() == 0 && !manager.has_new_frame());
    CHECK(manager.register_buffer(0, buffer) == bb::VideoStatus::ok);
    const std::array<std::uint8_t, 16> pixels{{1, 2, 3, 4, 5, 6, 7, 8,
                                                 9, 10, 11, 12, 13, 14, 15, 16}};
    CHECK(memory.store(0x1000, pixels.data(), pixels.size()) == bb::MemoryError::ok);
    CHECK(manager.submit_flip(0, 9) == bb::VideoStatus::ok);
    CHECK(sink.flips == 1 && manager.flip_count() == 1);
    CHECK(manager.read_current_frame_pixels(frame));
    CHECK(frame.valid && frame.flip_sequence == 1 && frame.pixel_data.size() == pixels.size());
    CHECK(!frame.is_tiled && frame.tiling_mode == 1);
    CHECK(std::equal(frame.pixel_data.begin(), frame.pixel_data.end(), pixels.begin()));
    CHECK(manager.has_new_frame());
    CHECK(memory.protect(0x1000, bb::write) == bb::MemoryError::ok);
    CHECK(!manager.read_current_frame_pixels(frame));
    CHECK(!frame.valid && frame.pixel_data.empty());
    CHECK(!manager.current_display_buffer().valid);
    CHECK(manager.has_new_frame());
    CHECK(manager.consume_new_frame());
    CHECK(memory.protect(0x1000, bb::read | bb::write) == bb::MemoryError::ok);

    // Buffer tiled (tiling_mode = 0) propaga flag is_tiled
    CHECK(manager.unregister_buffer(0) == bb::VideoStatus::ok);
    const bb::VideoBuffer tiled_buffer{0x1000, 2, 2, 2, 4, 0x80000000, 0};
    CHECK(manager.register_buffer(0, tiled_buffer) == bb::VideoStatus::ok);
    CHECK(manager.submit_flip(0, 11) == bb::VideoStatus::ok);
    CHECK(manager.read_current_frame_pixels(frame));
    CHECK(frame.valid && frame.is_tiled && frame.tiling_mode == 0 && frame.pixel_format == 0x80000000);
    CHECK(manager.consume_new_frame());
    // Pitch é contado em pixels, não em bytes: a leitura inclui padding
    // entre linhas, e a apresentação BGRA8 usa pitch * 4 por linha.
    CHECK(manager.register_buffer(1, {0x1100, 2, 2, 3, 4}) == bb::VideoStatus::ok);
    const std::array<std::uint8_t, 24> padded{{
        1, 2, 3, 4, 5, 6, 7, 8, 0xaa, 0xbb, 0xcc, 0xdd,
        9, 10, 11, 12, 13, 14, 15, 16, 0xee, 0xff, 0x17, 0x18
    }};
    CHECK(memory.store(0x1100, padded.data(), padded.size()) == bb::MemoryError::ok);
    CHECK(manager.submit_flip(1, 10) == bb::VideoStatus::ok);
    CHECK(manager.read_current_frame_pixels(frame));
    CHECK(frame.pitch == 3 && frame.bytes_per_pixel == 4 && frame.pixel_data.size() == 24);
    CHECK(std::equal(frame.pixel_data.begin(), frame.pixel_data.end(), padded.begin()));
    CHECK(manager.consume_new_frame());
    manager.detach_guest_memory();
    CHECK(!manager.has_guest_memory() && !manager.has_new_frame());
    CHECK(manager.flip_count() == 0 && manager.last_flip().sequence == 0);
    CHECK(manager.buffer(0) == nullptr && !manager.current_display_buffer().valid);

    CHECK(manager.register_buffer(1, {0, 4096, 4096, 4096, 4}) == bb::VideoStatus::ok); // 64 MiB
    CHECK(manager.register_buffer(2, {0, 4097, 4096, 4096, 4}) == bb::VideoStatus::invalid);
    CHECK(manager.register_buffer(2, {0, 4097, 4096, 4097, 4}) == bb::VideoStatus::overflow);
    CHECK(manager.register_buffer(2, {UINT64_MAX - 1, 2, 2, 2, 4}) == bb::VideoStatus::overflow);
    CHECK(manager.submit_flip(1, 0) == bb::VideoStatus::ok);
    CHECK(!manager.current_display_buffer().valid);
}); }
