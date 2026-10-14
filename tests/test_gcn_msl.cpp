#include "gcn_msl.h"
#include "check.h"

#include <cstring>
#include <string>

using namespace bb::gcn;

struct Registers {
    uint32_t s[104]{};
    uint32_t v[256]{};
};

static Registers eval_cpu(const ProgramIR& ir, Registers r) {
    uint32_t scc = 0;
    (void)scc;
    for (const auto& blk : ir.blocks) {
        for (const auto& n : blk.nodes) {
            if (n.operation == Operation::End) break;
            auto get_val = [&](const Value& val) -> uint32_t {
                if (val.operand.kind == OperandKind::Scalar) return r.s[val.operand.value];
                if (val.operand.kind == OperandKind::Vector) return r.v[val.operand.value];
                if (val.operand.kind == OperandKind::Literal) return val.operand.value;
                if (val.operand.kind == OperandKind::InlineConstant) {
                    if (val.operand.value >= 128 && val.operand.value <= 192) return val.operand.value - 128;
                    if (val.operand.value >= 193 && val.operand.value <= 208) return 0u - (val.operand.value - 192);
                }
                return 0;
            };
            auto set_val = [&](const Value& val, uint32_t res) {
                if (val.operand.kind == OperandKind::Scalar) r.s[val.operand.value] = res;
                else if (val.operand.kind == OperandKind::Vector) r.v[val.operand.value] = res;
            };

            if (n.operation == Operation::Move) {
                set_val(n.destinations[0], get_val(n.sources[0]));
            } else if (n.operation == Operation::Add) {
                uint32_t a = get_val(n.sources[0]);
                uint32_t b = get_val(n.sources[1]);
                uint32_t sum = a + b;
                set_val(n.destinations[0], sum);
                scc = sum < a ? 1u : 0u;
            } else if (n.operation == Operation::FloatAdd) {
                float a, b;
                uint32_t ua = get_val(n.sources[0]);
                uint32_t ub = get_val(n.sources[1]);
                std::memcpy(&a, &ua, sizeof(float));
                std::memcpy(&b, &ub, sizeof(float));
                float res = a + b;
                uint32_t ures;
                std::memcpy(&ures, &res, sizeof(float));
                set_val(n.destinations[0], ures);
            } else if (n.operation == Operation::FloatMultiply) {
                float a, b;
                uint32_t ua = get_val(n.sources[0]);
                uint32_t ub = get_val(n.sources[1]);
                std::memcpy(&a, &ua, sizeof(float));
                std::memcpy(&b, &ub, sizeof(float));
                float res = a * b;
                uint32_t ures;
                std::memcpy(&ures, &res, sizeof(float));
                set_val(n.destinations[0], ures);
            }
        }
    }
    return r;
}

int main() { return run([] {
    // 1. Trivial compute shader
    // S_ADD_U32 s2, s0, s1 (op 0 in SOP2)
    // S_ENDPGM
    const uint32_t compute_seq[] = {
        0x80000000u | (0u << 23) | (2u << 16) | (1u << 8) | 0u, // s2 = s0 + s1
        0xbf810000u                                              // s_endpgm
    };
    auto prog = decode(compute_seq, 2);
    auto ir = lower(prog);
    CHECK(ir.complete);
    auto msl_comp = translate_msl(ir, false);
    CHECK(msl_comp.error.empty());
    CHECK(msl_comp.text.find("kernel void gcn_compute") != std::string::npos);
    CHECK(msl_comp.text.find("uint sum = a + b;") != std::string::npos);
    CHECK(msl_comp.text.find("r.s[2] = sum;") != std::string::npos);

    // CPU semantics validation
    Registers r_in{};
    r_in.s[0] = 40;
    r_in.s[1] = 2;
    auto r_out = eval_cpu(ir, r_in);
    CHECK(r_out.s[2] == 42);

    // 2. Trivial vertex shader
    // V_MOV_B32 v0, 128 (inline constant 0) -> (1u << 9) | (0u << 17) | 128u
    // V_ADD_F32 v0, v1, v2
    // V_MUL_F32 v3, v1, v2
    // S_ENDPGM
    const uint32_t vertex_seq[] = {
        0x7e000000u | (1u << 9) | (0u << 17) | 128u,               // v0 = 0.0f
        (3u << 25) | (0u << 17) | (1u << 9) | (256u + 2u),         // v0 = v2 + v1
        (8u << 25) | (3u << 17) | (1u << 9) | (256u + 2u),         // v3 = v2 * v1
        0xbf810000u                                                // s_endpgm
    };
    prog = decode(vertex_seq, 4);
    ir = lower(prog);
    CHECK(ir.complete);
    auto msl_vert = translate_msl(ir, true);
    CHECK(msl_vert.error.empty());
    CHECK(msl_vert.text.find("vertex GcnVertexOut gcn_vertex") != std::string::npos);
    CHECK(msl_vert.text.find("as_type<float>(a) + as_type<float>(b)") != std::string::npos);
    CHECK(msl_vert.text.find("as_type<float>(a) * as_type<float>(b)") != std::string::npos);
    CHECK(msl_vert.text.find("result.position = float4") != std::string::npos);

    // Snapshot exact text check
    const std::string expected_snapshot =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct GcnRegisters { uint s[104]; uint v[256]; };\n"
        "struct GcnVertexOut { float4 position [[position]]; };\n"
        "vertex GcnVertexOut gcn_vertex(const device GcnRegisters* input [[buffer(0)]], uint id [[vertex_id]]) {\n"
        "    GcnRegisters r = input[id];\n"
        "    uint scc = 0u;\n"
        "    { uint a = 0u;\n"
        "      r.v[0] = a;\n"
        "    }\n"
        "    { uint a = r.v[2];\n"
        "      uint b = r.v[1];\n"
        "      r.v[0] = as_type<uint>(as_type<float>(a) + as_type<float>(b));\n"
        "    }\n"
        "    { uint a = r.v[2];\n"
        "      uint b = r.v[1];\n"
        "      r.v[3] = as_type<uint>(as_type<float>(a) * as_type<float>(b));\n"
        "    }\n"
        "    (void)scc;\n"
        "    GcnVertexOut result;\n"
        "    result.position = float4(as_type<float>(r.v[0]), as_type<float>(r.v[1]), as_type<float>(r.v[2]), as_type<float>(r.v[3]));\n"
        "    return result;\n"
        "}\n";
    CHECK(msl_vert.text == expected_snapshot);

    // CPU semantics for float add & mul
    Registers v_in{};
    float f1 = 1.5f;
    float f2 = 2.0f;
    std::memcpy(&v_in.v[1], &f1, sizeof(float));
    std::memcpy(&v_in.v[2], &f2, sizeof(float));
    auto v_out = eval_cpu(ir, v_in);
    float res0, res3;
    std::memcpy(&res0, &v_out.v[0], sizeof(float));
    std::memcpy(&res3, &v_out.v[3], sizeof(float));
    CHECK(res0 == 3.5f);
    CHECK(res3 == 3.0f);

    // 3. Status explicit unsupported tests
    // A: Indirect branch (S_SETPC_B64 = 32 in SOP1)
    const uint32_t ind_branch[] = {0xbe800000u | (32u << 8) | (0u << 16) | 0u};
    auto ir_ind = lower(decode(ind_branch, 1));
    CHECK(!ir_ind.complete);
    auto res_ind = translate_msl(ir_ind, false);
    CHECK(!res_ind.error.empty());
    CHECK(res_ind.error.find("indirect branch") != std::string::npos);

    // B: Texture sample (MIMG)
    const uint32_t tex[] = {0xf0000000u | (32u << 18), 1u | (2u << 8) | (3u << 16)};
    auto ir_tex = lower(decode(tex, 2));
    CHECK(ir_tex.complete);
    auto res_tex = translate_msl(ir_tex, false);
    CHECK(!res_tex.error.empty());
    CHECK(res_tex.error.find("texture") != std::string::npos);

    // C: Export (EXP)
    const uint32_t exp[] = {0xf8000000u | (15u << 12), 1u | (2u << 8)};
    auto ir_exp = lower(decode(exp, 2));
    CHECK(ir_exp.complete);
    auto res_exp = translate_msl(ir_exp, false);
    CHECK(!res_exp.error.empty());
    CHECK(res_exp.error.find("export") != std::string::npos);

    // D: Synchronization (S_WAITCNT = 12 in SOPP)
    const uint32_t wait[] = {0xbf800000u | (12u << 16)};
    auto ir_wait = lower(decode(wait, 1));
    CHECK(ir_wait.complete);
    auto res_wait = translate_msl(ir_wait, false);
    CHECK(!res_wait.error.empty());
    CHECK(res_wait.error.find("synchronization") != std::string::npos);

    // E: Atomics (MUBUF atomic = 48)
    const uint32_t atom[] = {0xe0000000u | (48u << 18), 1u | (2u << 8) | (3u << 16)};
    auto ir_atom = lower(decode(atom, 2));
    CHECK(ir_atom.complete);
    auto res_atom = translate_msl(ir_atom, false);
    CHECK(!res_atom.error.empty());
    CHECK(res_atom.error.find("atomics") != std::string::npos);

    // F: Branch / Control Flow (S_BRANCH = 2 in SOPP)
    const uint32_t br[] = {0xbf820001u, 0xbf810000u, 0xbf810000u};
    auto ir_br = lower(decode(br, 3));
    CHECK(ir_br.complete);
    auto res_br = translate_msl(ir_br, false);
    CHECK(!res_br.error.empty());
    CHECK(res_br.error.find("branch") != std::string::npos);

#if defined(__APPLE__)
    // Optional compilation via metal compiler if available on host without full Xcode
    if (std::system("which metal >/dev/null 2>&1 || xcrun --find metal >/dev/null 2>&1") == 0) {
        FILE* probe = popen("xcrun -sdk macosx metal --version 2>/dev/null", "r");
        if (probe) {
            char buf[64];
            bool has_version = fgets(buf, sizeof(buf), probe) != nullptr;
            pclose(probe);
            if (has_version) {
                FILE* fp1 = popen("xcrun -sdk macosx metal -x metal - -o /dev/null 2>/dev/null", "w");
                if (fp1) {
                    std::fputs(msl_comp.text.c_str(), fp1);
                    int ret = pclose(fp1);
                    CHECK(ret == 0);
                }
                FILE* fp2 = popen("xcrun -sdk macosx metal -x metal - -o /dev/null 2>/dev/null", "w");
                if (fp2) {
                    std::fputs(msl_vert.text.c_str(), fp2);
                    int ret = pclose(fp2);
                    CHECK(ret == 0);
                }
            }
        }
    }
#endif
}); }
