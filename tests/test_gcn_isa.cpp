#include "gcn_isa.h"
#include "check.h"

#include <array>
#include <cstdint>

using namespace bb::gcn;

int main() {
    struct Sample { uint32_t low; uint32_t high; Format format; uint32_t opcode; bool wide; };
    // Synthetic encodings only; no guest shader binaries or vendor assets.
    constexpr std::array<Sample, 16> samples{{
        {0xbe800000u | (3u << 8) | (5u << 16) | 4u, 0, Format::SOP1, 3, false},
        {0x80000000u | (1u << 23) | (7u << 16) | (3u << 8) | 2u, 0, Format::SOP2, 1, false},
        {0xb0000000u | (3u << 23) | (2u << 16) | 0xfffdu, 0, Format::SOPK, 3, false},
        {0xbf000000u | (1u << 16) | 3u, 0, Format::SOPC, 1, false},
        {0xbf800000u | (1u << 16), 0, Format::SOPP, 1, false},
        {0xc0000000u | (1u << 22) | (3u << 15) | (2u << 9) | (1u << 8) | 4u, 0, Format::SMRD, 1, false},
        {0x7e000000u | (1u << 9) | (2u << 17) | 256u, 0, Format::VOP1, 1, false},
        {(3u << 25) | (2u << 17) | (1u << 9) | 128u, 0, Format::VOP2, 3, false},
        {0xd0000000u | (3u << 17) | 1u, 256u | (257u << 9) | (258u << 18), Format::VOP3, 3, true},
        {0x7c000000u | (1u << 17) | (2u << 9) | 3u, 0, Format::VOPC, 1, false},
        {0xd8000000u | (4u << 18), 1u | (2u << 8) | (3u << 16) | (4u << 24), Format::DS, 4, true},
        {0xe0000000u | (5u << 18), 1u | (2u << 8) | (3u << 16) | (4u << 24), Format::MUBUF, 5, true},
        {0xe8000000u | (3u << 16), 1u | (2u << 8) | (3u << 16) | (4u << 24), Format::MTBUF, 3, true},
        {0xf0000000u | (6u << 18), 1u | (2u << 8) | (3u << 16) | (4u << 21), Format::MIMG, 6, true},
        {0xf8000000u | (8u << 4), 1u | (2u << 8) | (3u << 16) | (4u << 24), Format::EXP, 8, true},
        {0xffffffffu, 0, Format::Unknown, 0, false},
    }};
    for (const auto& s : samples) {
        const uint32_t words[] = {s.low, s.high};
        const auto result = decode(words, s.wide ? 2 : 1);
        CHECK(result.consumed_words == (s.wide ? 2u : 1u));
        CHECK(result.instructions.size() == 1);
        const auto& i = result.instructions.front();
        CHECK(i.format == s.format);
        CHECK(i.opcode == s.opcode);
        CHECK(i.word_count == (s.wide ? 2u : 1u));
        CHECK(i.raw[0] == s.low);
        if (s.wide) {
            CHECK(i.raw[1] == s.high);
            CHECK(decode(words, 1).instructions[0].diagnostic == Diagnostic::Truncated);
        }
    }
    const uint32_t lit[] = {0x80000000u | (1u << 8) | 255u, 0x12345678u};
    auto p = decode(lit, 2);
    CHECK(p.instructions.size() == 1);
    CHECK(p.instructions[0].has_literal);
    CHECK(p.instructions[0].sources[0].value == 0x12345678u);
    CHECK(decode(lit, 1).instructions[0].diagnostic == Diagnostic::Truncated);
    const uint32_t extra[] = {0xb0000000u | (21u << 23), 0xdeadbeefu,
                              (32u << 25), 0x87654321u};
    p = decode(extra, 4);
    CHECK(p.instructions.size() == 2);
    CHECK(p.instructions[0].literal == 0xdeadbeefu);
    CHECK(p.instructions[1].literal == 0x87654321u);
    const uint32_t jumps[] = {0xbf820001u, 0xbf810000u, 0xbf82fffeu};
    p = decode(jumps, 3);
    CHECK(p.instructions[0].branch_target_word == 2);
    CHECK(p.instructions[0].diagnostic == Diagnostic::None);
    CHECK(p.instructions[2].branch_target_word == 1);
    const uint32_t bad[] = {0xbf827fffu, 0xd0000000u};
    p = decode(bad, 2);
    CHECK(p.instructions[0].diagnostic == Diagnostic::BranchOutOfBounds);
    CHECK(p.instructions[1].diagnostic == Diagnostic::Truncated);
    const uint32_t ext[] = {0xcc000000u, 0u, 0x7e0000f9u, 0xa8000000u};
    p = decode(ext, 4);
    CHECK(p.instructions[0].diagnostic == Diagnostic::UnsupportedExtension);
    CHECK(p.instructions[2].diagnostic == Diagnostic::UnsupportedExtension);
    CHECK(p.instructions[2].word_count == 2);
    CHECK(decode(nullptr, 4).instructions.empty());
    // SOP1 S_MOV_B32 (op 3)
    const uint32_t sop1_mov[] = {0xbe800000u | (3u << 8) | (5u << 16) | 4u};
    const auto r_sop1 = decode(sop1_mov, 1);
    CHECK(r_sop1.instructions.size() == 1);
    CHECK(r_sop1.instructions[0].format == Format::SOP1);
    CHECK(r_sop1.instructions[0].opcode == 3);
    CHECK(r_sop1.instructions[0].diagnostic == Diagnostic::None);

    // SOP2 with two literals rejected
    const uint32_t two_lits[] = {0x80000000u | (255u << 8) | 255u, 0x12345678u};
    CHECK(decode(two_lits, 2).instructions[0].diagnostic == Diagnostic::InvalidField);

    return 0;
}
