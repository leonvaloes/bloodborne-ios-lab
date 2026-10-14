#include "gcn_ir.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace bb::gcn {
namespace {
bool reg(Operand o) { return o.kind == OperandKind::Scalar || o.kind == OperandKind::Vector || o.kind == OperandKind::Special; }
const char* name(Diagnostic d) {
    switch (d) {
    case Diagnostic::None: return "none";
    case Diagnostic::UnknownEncoding: return "unknown encoding";
    case Diagnostic::UnverifiedOpcode: return "unverified opcode";
    case Diagnostic::Truncated: return "truncated instruction";
    case Diagnostic::UnsupportedExtension: return "unsupported extension";
    case Diagnostic::BranchOutOfBounds: return "branch out of bounds";
    case Diagnostic::InvalidField: return "invalid field";
    }
    return "invalid diagnostic";
}
void classify(const Instruction& i, Node& n) {
    switch (i.format) {
    case Format::SOPP:
        if (i.opcode == 1) { n.operation = Operation::End; n.effect = Effect::Control; }
        else if (i.opcode == 2 || (i.opcode >= 4 && i.opcode <= 9)) {
            n.operation = Operation::Branch; n.effect = Effect::Control;
            n.branch_target_word = i.branch_target_word;
        } else if (i.opcode == 12) { n.operation = Operation::Wait; n.effect = Effect::Barrier; }
        break;
    case Format::SOP1: case Format::VOP1:
        if ((i.format == Format::SOP1 && i.opcode == 0) ||
            (i.format == Format::VOP1 && i.opcode == 1)) {
            n.operation = Operation::Move; n.effect = Effect::Pure;
        }
        break;
    case Format::SOP2:
        if (i.opcode == 0) { n.operation = Operation::Add; n.effect = Effect::Pure; }
        break;
    case Format::VOP2:
        // Floating-point VOP2 semantics and modifiers need separate verification.
        break;
    case Format::SMRD: n.operation = Operation::Load; n.effect = Effect::ReadMemory; break;
    case Format::DS: case Format::MUBUF: case Format::MTBUF:
        // Do not guess write vs read for unverified opcode families.
        if (i.format == Format::MUBUF && i.opcode == 0) {
            n.operation = Operation::Load; n.effect = Effect::ReadMemory;
        } else if (i.format == Format::MUBUF && i.opcode == 4) {
            n.operation = Operation::Store; n.effect = Effect::WriteMemory;
        } else if (i.format == Format::MUBUF && i.opcode >= 48 && i.opcode <= 79) {
            n.operation = Operation::Atomic; n.effect = Effect::Atomic;
        }
        break;
    case Format::MIMG:
        if (i.opcode >= 32 && i.opcode <= 63) { n.operation = Operation::Sample; n.effect = Effect::Texture; }
        break;
    case Format::EXP: n.operation = Operation::Export; n.effect = Effect::Export; break;
    default: break;
    }
}
std::string location(const Instruction& i, const char* reason) {
    return "word " + std::to_string(i.word_offset) + " format " +
           std::to_string(static_cast<int>(i.format)) + " opcode " +
           std::to_string(i.opcode) + ": " + reason;
}
} // namespace
ProgramIR lower(const Program& decoded, size_t max_nodes) {
    ProgramIR ir;
    if (decoded.instructions.size() > max_nodes) {
        ir.diagnostics.push_back("instruction limit exceeded"); return ir;
    }
    if (decoded.instructions.empty()) { ir.diagnostics.push_back("empty program"); return ir; }
    std::set<size_t> starts{decoded.instructions.front().word_offset};
    std::set<size_t> offsets;
    for (const auto& i : decoded.instructions) offsets.insert(i.word_offset);
    for (size_t index = 0; index < decoded.instructions.size(); ++index) {
        const auto& i = decoded.instructions[index];
        if (i.has_branch && i.branch_target_word >= 0 &&
            offsets.count(static_cast<size_t>(i.branch_target_word)))
            starts.insert(static_cast<size_t>(i.branch_target_word));
        if (i.format == Format::SOPP && index + 1 < decoded.instructions.size() &&
            (i.has_branch || i.opcode == 1))
            starts.insert(decoded.instructions[index + 1].word_offset);
    }
    std::map<size_t, size_t> blocks;
    for (auto start : starts) {
        blocks[start] = ir.blocks.size(); ir.blocks.push_back({start, {}, {}});
    }
    size_t current = 0;
    std::map<std::pair<OperandKind, uint32_t>, uint32_t> versions;
    for (const auto& i : decoded.instructions) {
        auto it = blocks.find(i.word_offset);
        if (it != blocks.end()) { current = it->second; versions.clear(); }
        Node n; n.word_offset = i.word_offset;
        classify(i, n);
        n.async_memory_hazard = n.effect == Effect::ReadMemory || n.effect == Effect::Texture ||
                                n.effect == Effect::Atomic;
        n.ordering_barrier = n.effect == Effect::Barrier;
        for (unsigned s = 0; s < i.source_count; ++s) {
            const auto o = i.sources[s];
            n.sources.push_back({o, reg(o) ? versions[{o.kind, o.value}] : 0u});
        }
        for (unsigned d = 0; d < i.destination_count; ++d) {
            const auto o = i.destinations[d];
            n.destinations.push_back({o, reg(o) ? ++versions[{o.kind, o.value}] : 0u});
        }
        if (i.diagnostic != Diagnostic::None || n.operation == Operation::Unsupported) {
            n.diagnostic = location(i, i.diagnostic != Diagnostic::None ? name(i.diagnostic) : "unsupported operation semantics");
            ir.diagnostics.push_back(n.diagnostic);
        }
        if (i.has_branch && !blocks.count(static_cast<size_t>(i.branch_target_word))) {
            const auto message = location(i, "branch target is not an instruction boundary");
            n.diagnostic = message; ir.diagnostics.push_back(message);
        }
        ir.blocks[current].nodes.push_back(std::move(n));
    }
    for (size_t b = 0; b < ir.blocks.size(); ++b) {
        auto& block = ir.blocks[b];
        const auto& last = block.nodes.back();
        if (last.operation == Operation::Branch && last.diagnostic.empty()) {
            auto target = blocks.find(static_cast<size_t>(last.branch_target_word));
            if (target != blocks.end()) block.successors.push_back(target->second);
        }
        const auto original = std::find_if(decoded.instructions.begin(), decoded.instructions.end(),
            [&](const Instruction& i) { return i.word_offset == last.word_offset; });
        const bool unconditional = original != decoded.instructions.end() &&
            original->format == Format::SOPP && original->opcode == 2;
        if (last.operation != Operation::End && !unconditional && b + 1 < ir.blocks.size())
            block.successors.push_back(b + 1);
    }
    ir.complete = ir.diagnostics.empty();
    return ir;
}
MslResult emit_msl(const ProgramIR& ir, bool vertex) {
    (void)vertex;
    if (!ir.complete) return {"", "IR has diagnostics"};
    return {"", "MSL unavailable: register types, stage IO and export routing are not inferred"};
}
} // namespace bb::gcn
