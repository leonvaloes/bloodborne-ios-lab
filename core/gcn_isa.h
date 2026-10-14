#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bb::gcn {

// Static Sea Islands instruction layout, not an executable shader or a semantic opcode map.
enum class Format { Unknown, SOP1, SOP2, SOPK, SOPC, SOPP, SMRD, VOP1, VOP2,
                    VOP3, VOPC, DS, MUBUF, MTBUF, MIMG, EXP };
enum class Diagnostic { None, UnknownEncoding, UnverifiedOpcode, Truncated, UnsupportedExtension,
                        BranchOutOfBounds };
enum class OperandKind { Scalar, Vector, InlineConstant, Special, Literal, Immediate };
struct Operand {
    OperandKind kind = OperandKind::Special;
    uint32_t value = 0; // register number, encoded selector, or immediate bits
};
struct Instruction {
    Format format = Format::Unknown;
    Diagnostic diagnostic = Diagnostic::None;
    size_t word_offset = 0;
    size_t word_count = 1;
    uint32_t opcode = 0; // format-local, not a semantic operation
    std::array<uint32_t, 2> raw{};
    std::array<Operand, 4> sources{};
    std::array<Operand, 2> destinations{};
    uint8_t source_count = 0;
    uint8_t destination_count = 0;
    bool has_literal = false;
    uint32_t literal = 0;
    bool has_branch = false;
    int64_t branch_target_word = 0; // SOPP: next instruction + signed displacement in dwords
    // Uninterpreted encoding-specific bitfields remain available in raw[] for later translation.
};
struct Program {
    std::vector<Instruction> instructions;
    // Number of complete words consumed; on truncation includes the incomplete instruction's prefix.
    size_t consumed_words = 0;
};

// Input words are host-endian 32-bit instruction words in shader byte order (low word first).
// A truncated instruction is emitted once, then decoding stops; never reads past word_count.
Program decode(const uint32_t* words, size_t word_count);

} // namespace bb::gcn
