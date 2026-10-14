#include "check.h"
#include "video_out.h"
#include <vector>
#include <cstdint>

struct Sink : bb::VideoOutSink {
    std::vector<bb::FlipEvent> events;
    void on_flip(const bb::FlipEvent& e) override { events.push_back(e); }
};
int main() { return run([] {
    bb::GuestMemory memory(4096);
    CHECK(memory.map(0x1000, 4096, bb::read | bb::write) == bb::MemoryError::ok);
    Sink sink;
    bb::VideoOut video(memory, sink);
    CHECK(video.submit_flip(0, 1) == bb::VideoStatus::missing);
    CHECK(video.register_buffer(16, {0x1000, 2, 2, 2, 4}) == bb::VideoStatus::invalid);
    CHECK(video.register_buffer(0, {0x1000, 3, 2, 2, 4}) == bb::VideoStatus::invalid);
    CHECK(video.register_buffer(0, {UINT64_MAX - 3, 2, 2, 2, 4}) == bb::VideoStatus::overflow);
    CHECK(video.register_buffer(0, {0x8000, 2, 2, 2, 4}) == bb::VideoStatus::unmapped);
    CHECK(video.register_buffer(0, {0x1000, 2, 2, 2, 4}) == bb::VideoStatus::ok);
    CHECK(video.register_buffer(0, {0x1000, 2, 2, 2, 4}) == bb::VideoStatus::occupied);
    CHECK(video.buffer(0) && video.buffer(0)->address == 0x1000);
    CHECK(video.submit_flip(0, 1234) == bb::VideoStatus::ok);
    CHECK(video.submit_flip(0, 5678) == bb::VideoStatus::ok);
    CHECK(video.flip_count() == 2 && video.last_flip().argument == 5678);
    CHECK(sink.events.size() == 2 && sink.events[0].sequence == 1);
    CHECK(video.unregister_buffer(0) == bb::VideoStatus::ok);
    CHECK(video.unregister_buffer(0) == bb::VideoStatus::missing);
    CHECK(video.submit_flip(0, 0) == bb::VideoStatus::missing);
}); }
