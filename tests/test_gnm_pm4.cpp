#include "check.h"
#include "gnm_pm4.h"
#include <vector>
#include <cstdint>

namespace {
struct Sink : bb::Pm4Sink {
    std::vector<bb::Pm4Event> events;
    void on_pm4_event(const bb::Pm4Event& e) override { events.push_back(e); }
};
std::uint32_t header(std::uint32_t op, std::uint32_t count) {
    return 0xc0000000u | ((count - 1) << 16) | (op << 8);
}
void put(bb::GuestMemory& m, std::uint64_t address, const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> bytes;
    for (auto w : words) for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
    CHECK(m.store(address, bytes.data(), bytes.size()) == bb::MemoryError::ok);
}
}
int main() { return run([] {
    bb::GuestMemory memory(65536);
    CHECK(memory.map(0x1000, 65536, bb::read | bb::write) == bb::MemoryError::ok);
    Sink sink;
    bb::GnmPm4 parser(memory, sink);
    std::vector<std::uint32_t> commands{
        0x00010004, 9, 10, // tipo 0: dois registradores
        0x80000000, // tipo 2
        header(0x69, 3), 5, 11, 12,
        header(0x76, 2) | 2, 7, 42,
        header(0x79, 2), 8, 43,
        header(0x27, 5), 12, 0x3000, 0, 6, 0,
        header(0x2d, 2), 3, 0,
        header(0x15, 4), 2, 3, 4, 0,
        header(0x10, 2), 0x68750777, 1,
        header(0x46, 1), 4,
        header(0x47, 5), 1, 2, 0, 3, 4,
        header(0x48, 4), 1, 2, 0, 3,
        header(0x37, 4), 0, 0x3000, 0, 42,
        header(0x50, 6), 0, 0, 0, 0, 0, 0,
        header(0x3c, 6), 0, 0, 0, 0, 0, 0,
        header(0x58, 6), 0, 0, 0, 0, 0, 0
    };
    put(memory, 0x1000, commands);
    CHECK(parser.process(0x1000, static_cast<std::uint32_t>(commands.size())) == bb::Pm4Status::ok);
    CHECK(parser.context(4) == 9 && parser.context(5) == 11 && parser.context(6) == 12);
    CHECK(parser.shader(7, true) == 42 && parser.shader(7) == 0);
    CHECK(parser.uconfig(8) == 43);
    CHECK(sink.events.size() == 11);
    CHECK(sink.events[0].kind == bb::Pm4Kind::draw_indexed);
    CHECK(sink.events[0].address == 0x3000);
    CHECK(sink.events[3].kind == bb::Pm4Kind::flip);
    CHECK(sink.events[8].kind == bb::Pm4Kind::dma_data);
    put(memory, 0x2000, {header(0x27, 5), 1});
    CHECK(parser.process(0x2000, 2) == bb::Pm4Status::truncated);
    put(memory, 0x2000, {header(0x69, 2), 0x4000, 5});
    CHECK(parser.process(0x2000, 3) == bb::Pm4Status::invalid);
    put(memory, 0x2000, {0x3fff0000});
    CHECK(parser.process(0x2000, 1) == bb::Pm4Status::truncated);
    CHECK(parser.process(0x1000, 0xffffffff) == bb::Pm4Status::limit);
    CHECK(parser.process(UINT64_MAX - 3, 2) == bb::Pm4Status::invalid);
    CHECK(parser.process(0x900000, 1) == bb::Pm4Status::unmapped);
    put(memory, 0x2000, {header(0x3f, 3), 0x2000, 0, 4});
    CHECK(parser.process(0x2000, 4) == bb::Pm4Status::cycle);
    put(memory, 0x2000, {header(0x3f, 3), 0x2100, 0, 3});
    put(memory, 0x2100, {header(0x2d, 2), 20, 0});
    CHECK(parser.process(0x2000, 4) == bb::Pm4Status::ok);
    CHECK(sink.events.back().kind == bb::Pm4Kind::draw_auto);
    bb::GnmPm4 small(memory, sink, {4, 10, 2});
    CHECK(small.process(0x2000, 4) == bb::Pm4Status::limit);
    put(memory, 0x2000, {0x40000000});
    CHECK(parser.process(0x2000, 1) == bb::Pm4Status::unsupported);
    put(memory, 0x2000, {header(0x7e, 1), 123}); // tipo 3 desconhecido nao pode passar despercebido
    CHECK(parser.process(0x2000, 2) == bb::Pm4Status::unsupported);
}); }
