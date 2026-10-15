#include "gcn_msl.h"

#include <sstream>

namespace bb::gcn {
namespace {
std::string value(const Value& v, std::string& error) {
    const auto o = v.operand;
    switch (o.kind) {
    case OperandKind::Scalar:
        if (o.value <= 103) return "r.s[" + std::to_string(o.value) + "]";
        break;
    case OperandKind::Vector:
        if (o.value <= 255) return "r.v[" + std::to_string(o.value) + "]";
        break;
    case OperandKind::Literal: return std::to_string(o.value) + "u";
    case OperandKind::InlineConstant:
        if (o.value >= 128 && o.value <= 192) return std::to_string(o.value - 128) + "u";
        if (o.value >= 193 && o.value <= 208) return std::to_string(0u - (o.value - 192)) + "u";
        break;
    default: break;
    }
    error = "unsupported register selector or inline constant " + std::to_string(o.value);
    return {};
}
const char* reason(const Node& n) {
    if (n.format == Format::SOP1 && (n.opcode == 32 || n.opcode == 33 || n.opcode == 34))
        return "indirect branch";
    if (n.operation == Operation::Branch || n.effect == Effect::Control) return "branch/control flow";
    if (n.effect == Effect::Texture) return "texture";
    if (n.effect == Effect::Export) return "export";
    if (n.effect == Effect::Atomic) return "atomics";
    if (n.effect == Effect::Barrier) return "synchronization";
    if (n.effect == Effect::ReadMemory || n.effect == Effect::WriteMemory) return "memory";
    return "opcode or register semantics";
}
} // namespace

MslResult translate_msl(const ProgramIR& ir, ShaderStage stage) {
    if (!ir.complete) return {"", "IR has diagnostics: " +
        (ir.diagnostics.empty() ? std::string("incomplete") : ir.diagnostics.front())};
    if (ir.blocks.size() != 1 || ir.blocks[0].nodes.empty() ||
        !ir.blocks[0].successors.empty()) return {"", "unsupported branch/control flow"};
    const auto& nodes = ir.blocks[0].nodes;
    std::ostringstream body;
    for (size_t idx = 0; idx < nodes.size(); ++idx) {
        const auto& n = nodes[idx];
        if (!n.diagnostic.empty()) return {"", "IR has diagnostics: " + n.diagnostic};
        if (n.operation == Operation::End && idx + 1 == nodes.size()) continue;
        if (n.operation != Operation::Move && n.operation != Operation::Add &&
            n.operation != Operation::FloatAdd && n.operation != Operation::FloatMultiply)
            return {"", std::string("unsupported ") + reason(n) + " at word " + std::to_string(n.word_offset)};
        if (n.destinations.size() != 1 || n.sources.size() != (n.operation == Operation::Move ? 1u : 2u))
            return {"", "unsupported operand arity"};
        std::string error;
        const auto dst = value(n.destinations[0], error);
        const auto a = value(n.sources[0], error);
        const auto b = n.sources.size() > 1 ? value(n.sources[1], error) : std::string{};
        if (!error.empty()) return {"", error + " at word " + std::to_string(n.word_offset)};
        if ((n.operation == Operation::Move && !((n.format == Format::SOP1 && n.opcode == 3) ||
                                               (n.format == Format::VOP1 && n.opcode == 1))) ||
            (n.operation == Operation::Add && !(n.format == Format::SOP2 && n.opcode == 0)) ||
            ((n.operation == Operation::FloatAdd || n.operation == Operation::FloatMultiply) &&
             !(n.format == Format::VOP2 && n.opcode == (n.operation == Operation::FloatAdd ? 3u : 8u))))
            return {"", "unsupported opcode/operation pairing"};
        // Resolve all operands before writing the destination (including aliasing sources).
        body << "    { uint a = " << a << ";\n";
        if (n.sources.size() > 1) body << "      uint b = " << b << ";\n";
        if (n.operation == Operation::Move) body << "      " << dst << " = a;\n";
        else if (n.operation == Operation::Add) {
            body << "      uint sum = a + b;\n      " << dst << " = sum;\n"
                 << "      scc = uint(sum < a);\n";
        } else body << "      " << dst << " = as_type<uint>(as_type<float>(a) "
                    << (n.operation == Operation::FloatAdd ? "+" : "*")
                    << " as_type<float>(b));\n";
        body << "    }\n";
    }
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
        << "struct GcnRegisters { uint s[104]; uint v[256]; };\n";
    if (stage == ShaderStage::Vertex) {
        out << "struct GcnVertexOut { float4 position [[position]]; };\n"
            << "vertex GcnVertexOut gcn_vertex(const device GcnRegisters* input [[buffer(0)]], uint id [[vertex_id]]) {\n"
            << "    GcnRegisters r = input[id];\n";
    } else if (stage == ShaderStage::Pixel) {
        out << "struct GcnFragmentOut { float4 color [[color(0)]]; };\n"
            << "fragment GcnFragmentOut gcn_fragment(const device GcnRegisters* input [[buffer(0)]], uint id [[primitive_id]]) {\n"
            << "    GcnRegisters r = input ? input[0] : GcnRegisters{};\n";
    } else {
        out << "kernel void gcn_compute(device GcnRegisters* files [[buffer(0)]], uint id [[thread_position_in_grid]]) {\n"
            << "    GcnRegisters r = files[id];\n";
    }
    out << "    uint scc = 0u;\n" << body.str() << "    (void)scc;\n";
    if (stage == ShaderStage::Vertex) {
        out << "    GcnVertexOut result;\n    result.position = float4(as_type<float>(r.v[0]), as_type<float>(r.v[1]), as_type<float>(r.v[2]), as_type<float>(r.v[3]));\n    return result;\n";
    } else if (stage == ShaderStage::Pixel) {
        out << "    GcnFragmentOut result;\n    result.color = float4(as_type<float>(r.v[0]), as_type<float>(r.v[1]), as_type<float>(r.v[2]), as_type<float>(r.v[3]));\n    return result;\n";
    } else {
        out << "    files[id] = r;\n";
    }
    out << "}\n";
    return {out.str(), ""};
}

MslResult translate_msl(const ProgramIR& ir, bool vertex) {
    return translate_msl(ir, vertex ? ShaderStage::Vertex : ShaderStage::Compute);
}
} // namespace bb::gcn
