#include "gcn_ir.h"
#include "check.h"

#include <cstdint>
#include <string>
using namespace bb::gcn;

int main() { return run([] {
    // Invented words only. No game shader bytes.
    const uint32_t flow[] = {0xbf820001u, 0x80010000u, 0xbf810000u};
    auto ir = lower(decode(flow, 3));
    CHECK(ir.complete);
    CHECK(ir.blocks.size() == 3);
    CHECK(ir.blocks[0].successors.size() == 1);
    CHECK(ir.blocks[0].successors[0] == 2);
    CHECK(ir.blocks[1].nodes[0].destinations[0].version == 1);
    CHECK(!emit_msl(ir, true).error.empty());
    const uint32_t badjump[] = {0xbf820001u, 0xd0000000u, 0u};
    ir = lower(decode(badjump, 3));
    CHECK(!ir.complete);
    CHECK(ir.diagnostics[0].find("boundary") != std::string::npos);
    const uint32_t trunc[] = {0xf0000000u};
    ir = lower(decode(trunc, 1));
    CHECK(!ir.complete);
    CHECK(ir.diagnostics[0].find("truncated") != std::string::npos);
    const uint32_t texture[] = {0xf0000000u | (32u << 18), 1u | (2u << 8) | (3u << 16)};
    ir = lower(decode(texture, 2));
    CHECK(ir.complete);
    CHECK(ir.blocks[0].nodes[0].effect == Effect::Texture);
    CHECK(ir.blocks[0].nodes[0].operation == Operation::Sample);
    const uint32_t exp[] = {0xf8000000u | (15u << 12), 1u | (2u << 8)};
    ir = lower(decode(exp, 2));
    CHECK(ir.complete);
    CHECK(ir.blocks[0].nodes[0].effect == Effect::Export);
    const uint32_t atomic[] = {0xe0000000u | (48u << 18), 1u | (2u << 8) | (3u << 16)};
    ir = lower(decode(atomic, 2));
    CHECK(ir.complete);
    CHECK(ir.blocks[0].nodes[0].effect == Effect::Atomic);
    CHECK(!emit_msl(ir, false).error.empty());
    const uint32_t unknown[] = {0xffffffffu};
    CHECK(!lower(decode(unknown, 1)).complete);
    CHECK(!lower(decode(flow, 3), 2).complete);
    const uint32_t simple[] = {0x80010000u, 0xbf810000u};
    ir = lower(decode(simple, 2));
    CHECK(ir.complete);
    CHECK(emit_msl(ir, true).text.empty());
    CHECK(emit_msl(ir, false).error.find("stage IO") != std::string::npos);
    const uint32_t invalid[] = {0xbe800000u | (1u << 8) | (127u << 16)};
    CHECK(decode(invalid, 1).instructions[0].diagnostic == Diagnostic::InvalidField);
    });
}
