#pragma once
#include "gcn_isa.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bb::gcn {
// Static, bounded analysis. SSA versions are local to straight-line blocks; no phi nodes.
enum class Effect { Pure, Control, ReadMemory, WriteMemory, Atomic, Texture, Export, Barrier, Unknown };
enum class Operation { Unsupported, Move, Add, Multiply, FloatAdd, FloatMultiply, Load, Store, Atomic, Sample, Export, Branch, End, Wait };
struct Value {
    Operand operand{};
    uint32_t version = 0; // zero denotes live-in or non-register value
};
struct Node {
    size_t word_offset = 0;
    Format format = Format::Unknown;
    uint32_t opcode = 0;
    Operation operation = Operation::Unsupported;
    Effect effect = Effect::Unknown;
    std::vector<Value> sources;
    std::vector<Value> destinations;
    int64_t branch_target_word = -1;
    bool async_memory_hazard = false; // requires waitcnt tracking before consuming a result
    bool ordering_barrier = false;
    std::string diagnostic;
};
struct Block {
    size_t start_word = 0;
    std::vector<Node> nodes;
    std::vector<size_t> successors; // indices into ProgramIR::blocks
};
struct ProgramIR {
    std::vector<Block> blocks;
    std::vector<std::string> diagnostics;
    bool complete = false;
};
// No shader execution, no resource binding, no assumptions about stage or register types.
ProgramIR lower(const Program& decoded, size_t max_nodes = 65536);
// Returns an error, not fabricated MSL, for any effect/control flow outside a tiny subset.
struct MslResult { std::string text; std::string error; };
MslResult emit_msl(const ProgramIR& ir, bool vertex);
} // namespace bb::gcn
