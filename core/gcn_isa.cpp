#include "gcn_isa.h"

namespace bb::gcn {
namespace {
uint32_t bits(uint32_t value, unsigned shift, uint32_t mask) { return (value >> shift) & mask; }
Operand scalar(uint32_t code) {
    if (code <= 103) return {OperandKind::Scalar, code};
    if ((code >= 128 && code <= 208) || (code >= 240 && code <= 248))
        return {OperandKind::InlineConstant, code};
    if (code == 255) return {OperandKind::Literal, 0};
    return {OperandKind::Special, code};
}
Operand vector(uint32_t code) { return {OperandKind::Vector, code}; }
Operand vsrc(uint32_t code) { return code >= 256 ? vector(code - 256) : scalar(code); }
void src(Instruction& i, Operand o) { i.sources[i.source_count++] = o; }
void dst(Instruction& i, Operand o) { i.destinations[i.destination_count++] = o; }
Format format(uint32_t w) {
    if ((w & 0xff800000u) == 0xbe800000u) return Format::SOP1;
    if ((w & 0xff800000u) == 0xbf800000u) return Format::SOPP;
    if ((w & 0xff800000u) == 0xbf000000u) return Format::SOPC;
    if ((w & 0xfe000000u) == 0x7e000000u) return Format::VOP1;
    if ((w & 0xfe000000u) == 0x7c000000u) return Format::VOPC;
    if ((w & 0xfc000000u) == 0xd0000000u) return Format::VOP3;
    if ((w & 0xfc000000u) == 0xf8000000u) return Format::EXP;
    if ((w & 0xfc000000u) == 0xd8000000u) return Format::DS;
    if ((w & 0xfc000000u) == 0xe0000000u) return Format::MUBUF;
    if ((w & 0xfc000000u) == 0xe8000000u) return Format::MTBUF;
    if ((w & 0xfc000000u) == 0xf0000000u) return Format::MIMG;
    if ((w & 0xf8000000u) == 0xc0000000u) return Format::SMRD;
    if ((w & 0xf0000000u) == 0xb0000000u) return Format::SOPK;
    if ((w & 0xc0000000u) == 0x80000000u) return Format::SOP2;
    if ((w & 0x80000000u) == 0) return Format::VOP2;
    return Format::Unknown; // includes VINTRP and VOP3P: not silently misdecoded
}
// Deliberately narrow opcode allowlist: unknown/reserved values must not become IR.
// A listed opcode only validates the opcode slot, not its full semantics.
bool known(Format f, uint32_t op) {
    switch (f) {
    case Format::SOP1: return op <= 53;
    case Format::SOP2: return op <= 44;
    case Format::SOPK: return op <= 21;
    case Format::SOPC: return op <= 15;
    case Format::SOPP: return op <= 26 && op != 3;
    case Format::SMRD: return op <= 4 || (op >= 8 && op <= 12);
    case Format::VOP1: return op <= 17 || (op >= 32 && op <= 69);
    case Format::VOP2: return op <= 36;
    case Format::VOPC: return op <= 247;
    case Format::VOP3: return op <= 511; // opcode families beyond this require separate validation
    case Format::DS: return op <= 127;
    case Format::MUBUF: return op <= 113;
    case Format::MTBUF: return op <= 7;
    case Format::MIMG: return op <= 111;
    case Format::EXP: return op <= 63; // target, not opcode
    case Format::Unknown: return false;
    }
    return false;
}
bool valid_fields(const Instruction& i) {
    // A scalar destination is a register selector, never an inline/literal value.
    for (unsigned n = 0; n < i.destination_count; ++n)
        if (i.destinations[n].kind == OperandKind::Special && i.format != Format::VOPC)
            return false;
    if (i.format == Format::SMRD && i.sources[1].kind == OperandKind::Special) return false;
    // A single trailing dword cannot stand for two independent literal operands.
    if (i.format == Format::SOP2 && i.sources[0].kind == OperandKind::Literal &&
        i.sources[1].kind == OperandKind::Literal) return false;
    // VOP3 src modifiers are not yet modelled. Refuse them rather than dropping bits.
    if (i.format == Format::VOP3 && ((i.raw[0] & 0x0000ff00u) != 0 ||
                                      (i.raw[1] & 0xf8000000u) != 0)) return false;
    // EXP requires at least one enabled component and cannot use both done and valid_mask
    // to imply anything about scheduling; those flags are carried in raw words.
    if (i.format == Format::EXP && ((i.raw[0] >> 12) & 15u) == 0) return false;
    return true;
}
bool wide(Format f) {
    return f == Format::VOP3 || f == Format::DS || f == Format::MUBUF ||
           f == Format::MTBUF || f == Format::MIMG || f == Format::EXP;
}
} // namespace

Program decode(const uint32_t* words, size_t count) {
    Program program;
    if (!words) return program;
    size_t pos = 0;
    while (pos < count) {
        Instruction i;
        i.word_offset = pos;
        const uint32_t w = words[pos];
        i.raw[0] = w;
        i.format = format(w);
        if (i.format == Format::Unknown) {
            i.diagnostic = ((w & 0xfc000000u) == 0xcc000000u ||
                            (w & 0xfc000000u) == 0xc8000000u)
                               ? Diagnostic::UnsupportedExtension : Diagnostic::UnknownEncoding;
            program.instructions.push_back(i);
            ++pos;
            continue;
        }
        const size_t base = wide(i.format) ? 2 : 1;
        if (base > count - pos) {
            i.diagnostic = Diagnostic::Truncated;
            i.word_count = count - pos;
            program.instructions.push_back(i);
            pos = count;
            break;
        }
        const uint32_t hi = base == 2 ? words[pos + 1] : 0;
        i.raw[1] = hi;
        switch (i.format) {
        case Format::SOP1:
            i.opcode = bits(w, 8, 255); src(i, scalar(bits(w, 0, 255)));
            dst(i, scalar(bits(w, 16, 127))); break;
        case Format::SOP2:
            i.opcode = bits(w, 23, 127); src(i, scalar(bits(w, 0, 255)));
            src(i, scalar(bits(w, 8, 255))); dst(i, scalar(bits(w, 16, 127))); break;
        case Format::SOPC:
            i.opcode = bits(w, 16, 127); src(i, scalar(bits(w, 0, 255)));
            src(i, scalar(bits(w, 8, 255))); break;
        case Format::SOPK:
            i.opcode = bits(w, 23, 31); dst(i, scalar(bits(w, 16, 127)));
            src(i, {OperandKind::Immediate, bits(w, 0, 65535)}); break;
        case Format::SOPP:
            i.opcode = bits(w, 16, 127);
            src(i, {OperandKind::Immediate, bits(w, 0, 65535)});
            if (i.opcode == 2 || (i.opcode >= 4 && i.opcode <= 9)) {
                i.has_branch = true;
                i.branch_target_word = static_cast<int64_t>(pos) + 1 +
                                       static_cast<int16_t>(w & 65535);
            }
            break;
        case Format::SMRD:
            i.opcode = bits(w, 22, 31); dst(i, scalar(bits(w, 15, 127)));
            src(i, scalar(bits(w, 9, 63) * 2));
            src(i, bits(w, 8, 1) ? Operand{OperandKind::Immediate, bits(w, 0, 255)}
                                 : scalar(bits(w, 0, 255))); break;
        case Format::VOP1:
            i.opcode = bits(w, 9, 255); src(i, vsrc(bits(w, 0, 511)));
            dst(i, vector(bits(w, 17, 255))); break;
        case Format::VOP2:
            i.opcode = bits(w, 25, 63); src(i, vsrc(bits(w, 0, 511)));
            src(i, vector(bits(w, 9, 255))); dst(i, vector(bits(w, 17, 255))); break;
        case Format::VOPC:
            i.opcode = bits(w, 17, 255); src(i, vsrc(bits(w, 0, 511)));
            src(i, vector(bits(w, 9, 255))); dst(i, {OperandKind::Special, 106}); break;
        case Format::VOP3:
            i.opcode = bits(w, 17, 511) | (bits(w, 16, 1) << 9);
            dst(i, vector(bits(w, 0, 255)));
            src(i, vsrc(bits(hi, 0, 511))); src(i, vsrc(bits(hi, 9, 511)));
            src(i, vsrc(bits(hi, 18, 511))); break;
        case Format::DS:
            i.opcode = bits(w, 18, 255); src(i, vector(bits(hi, 0, 255)));
            src(i, vector(bits(hi, 8, 255))); src(i, vector(bits(hi, 16, 255)));
            dst(i, vector(bits(hi, 24, 255))); break;
        case Format::MUBUF: case Format::MTBUF:
            i.opcode = i.format == Format::MUBUF ? bits(w, 18, 127) : bits(w, 16, 7);
            src(i, vector(bits(hi, 0, 255)));
            // vdata is a destination for loads, a source for stores/atomics.
            if ((i.format == Format::MUBUF && i.opcode < 4) ||
                (i.format == Format::MTBUF && i.opcode < 4))
                dst(i, vector(bits(hi, 8, 255)));
            else src(i, vector(bits(hi, 8, 255)));
            src(i, scalar(bits(hi, 16, 31) * 4)); src(i, scalar(bits(hi, 24, 255)));
            break;
        case Format::MIMG:
            i.opcode = bits(w, 18, 127);
            src(i, vector(bits(hi, 0, 255))); dst(i, vector(bits(hi, 8, 255)));
            src(i, scalar(bits(hi, 16, 31) * 4));
            src(i, scalar(bits(hi, 21, 31) * 4)); break;
        case Format::EXP:
            i.opcode = bits(w, 4, 63); // target selector, not an ALU opcode
            for (unsigned n = 0; n < 4; ++n) src(i, vector(bits(hi, n * 8, 255)));
            break;
        case Format::Unknown: break;
        }
        bool literal = (i.format == Format::SOPK && i.opcode == 21) ||
                       (i.format == Format::VOP2 && (i.opcode == 32 || i.opcode == 33));
        if (i.format == Format::SOP1 || i.format == Format::SOP2 ||
            i.format == Format::SOPC || i.format == Format::VOP1 ||
            i.format == Format::VOP2 || i.format == Format::VOPC)
            for (unsigned n = 0; n < i.source_count; ++n)
                literal = literal || i.sources[n].kind == OperandKind::Literal;
        // SDWA/DPP are separate extension words, not ordinary literal constants.
        bool extension = false;
        if (i.format == Format::VOP1 || i.format == Format::VOP2 || i.format == Format::VOPC)
            extension = bits(w, 0, 511) == 249 || bits(w, 0, 511) == 250;
        if (extension) { literal = false; i.diagnostic = Diagnostic::UnsupportedExtension; }
        const size_t length = base + (literal || extension ? 1 : 0);
        if (length > count - pos) {
            i.diagnostic = Diagnostic::Truncated;
            i.word_count = count - pos;
            program.instructions.push_back(i);
            pos = count;
            break;
        }
        i.word_count = length;
        if (literal) {
            i.has_literal = true;
            i.literal = words[pos + base];
            for (unsigned n = 0; n < i.source_count; ++n)
                if (i.sources[n].kind == OperandKind::Literal) i.sources[n].value = i.literal;
        }
        if (i.diagnostic == Diagnostic::None && !known(i.format, i.opcode))
            i.diagnostic = Diagnostic::UnverifiedOpcode;
        if (i.diagnostic == Diagnostic::None && !valid_fields(i))
            i.diagnostic = Diagnostic::InvalidField;
        pos += length;
        program.instructions.push_back(i);
    }
    program.consumed_words = pos;
    for (auto& i : program.instructions) {
        if (i.has_branch && (i.branch_target_word < 0 ||
                             i.branch_target_word >= static_cast<int64_t>(count)))
            i.diagnostic = Diagnostic::BranchOutOfBounds;
    }
    return program;
}
} // namespace bb::gcn
