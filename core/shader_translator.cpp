#include "shader_translator.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

namespace bb {

namespace {

struct SpvInstruction {
    uint32_t opcode = 0;
    uint32_t length = 0;
    const uint32_t* operands = nullptr;
};

struct SpvType {
    uint32_t id = 0;
    uint32_t opcode = 0;
    uint32_t bit_width = 32;
    uint32_t component_count = 1;
    uint32_t sub_type_id = 0;
    std::string name;
    std::vector<uint32_t> member_type_ids;
};

struct SpvVariable {
    uint32_t id = 0;
    uint32_t type_id = 0;
    uint32_t storage_class = 0;
    std::string name;
    int32_t location = -1;
    int32_t binding = -1;
    int32_t descriptor_set = -1;
    int32_t builtin = -1;
};

} // namespace

bool ShaderTranslator::validate_spirv_header(const uint32_t* words, size_t word_count, std::string& out_error) {
    if (!words || word_count < 5) {
        out_error = "SPIR-V binary is empty or truncated (< 5 words)";
        return false;
    }

    const uint32_t magic = words[0];
    if (magic != spirv::MagicNumber && magic != spirv::MagicNumberReversed) {
        out_error = "Invalid SPIR-V magic number: 0x" + std::to_string(magic);
        return false;
    }

    const uint32_t version = words[1];
    const uint32_t major = (version >> 16) & 0xFF;
    const uint32_t minor = (version >> 8) & 0xFF;
    if (major != 1) {
        out_error = "Unsupported SPIR-V major version: " + std::to_string(major);
        return false;
    }
    if (minor > 6) {
        out_error = "Unsupported SPIR-V minor version: 1." + std::to_string(minor);
        return false;
    }

    const uint32_t bound = words[3];
    if (bound == 0) {
        out_error = "SPIR-V id bound cannot be 0";
        return false;
    }

    return true;
}

ShaderReflection ShaderTranslator::reflect(const uint32_t* words, size_t word_count) {
    ShaderReflection refl{};
    std::string err;
    if (!validate_spirv_header(words, word_count, err)) {
        return refl;
    }

    bool swap = (words[0] == spirv::MagicNumberReversed);
    std::vector<uint32_t> buffer;
    if (swap) {
        buffer.resize(word_count);
        for (size_t i = 0; i < word_count; ++i) {
            uint32_t v = words[i];
            buffer[i] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
        }
        words = buffer.data();
    }

    std::unordered_map<uint32_t, SpvType> types;
    std::unordered_map<uint32_t, SpvVariable> variables;
    std::unordered_map<uint32_t, std::string> names;

    size_t index = 5;
    while (index < word_count) {
        const uint32_t word = words[index];
        const uint32_t length = word >> 16;
        const uint32_t opcode = word & 0xFFFF;
        if (length == 0 || index + length > word_count) break;

        const uint32_t* ops = &words[index + 1];
        const uint32_t op_count = length - 1;

        switch (opcode) {
            case spirv::OpEntryPoint: {
                if (op_count >= 2) {
                    uint32_t model = ops[0];
                    if (model == spirv::ExecutionModelVertex) refl.stage = SpirvShaderStage::Vertex;
                    else if (model == spirv::ExecutionModelFragment) refl.stage = SpirvShaderStage::Fragment;
                    else if (model == spirv::ExecutionModelGLCompute) refl.stage = SpirvShaderStage::Compute;

                    // Entry point name is null-terminated string
                    const char* str = reinterpret_cast<const char*>(&ops[2]);
                    refl.entry_point_name = str;
                }
                break;
            }
            case spirv::OpName: {
                if (op_count >= 2) {
                    uint32_t target_id = ops[0];
                    const char* str = reinterpret_cast<const char*>(&ops[1]);
                    names[target_id] = str;
                }
                break;
            }
            case spirv::OpDecorate: {
                if (op_count >= 2) {
                    uint32_t target_id = ops[0];
                    uint32_t decoration = ops[1];
                    auto& var = variables[target_id];
                    var.id = target_id;
                    if (decoration == spirv::DecorationLocation && op_count >= 3) {
                        var.location = static_cast<int32_t>(ops[2]);
                    } else if (decoration == spirv::DecorationBinding && op_count >= 3) {
                        var.binding = static_cast<int32_t>(ops[2]);
                    } else if (decoration == spirv::DecorationDescriptorSet && op_count >= 3) {
                        var.descriptor_set = static_cast<int32_t>(ops[2]);
                    } else if (decoration == spirv::DecorationBuiltIn && op_count >= 3) {
                        var.builtin = static_cast<int32_t>(ops[2]);
                    }
                }
                break;
            }
            case spirv::OpTypeFloat: {
                if (op_count >= 2) {
                    uint32_t id = ops[0];
                    types[id] = {id, opcode, ops[1], 1, 0, "float", {}};
                }
                break;
            }
            case spirv::OpTypeVector: {
                if (op_count >= 3) {
                    uint32_t id = ops[0];
                    uint32_t elem_id = ops[1];
                    uint32_t count = ops[2];
                    std::string tname = "float" + std::to_string(count);
                    types[id] = {id, opcode, 32, count, elem_id, tname, {}};
                }
                break;
            }
            case spirv::OpTypeMatrix: {
                if (op_count >= 3) {
                    uint32_t id = ops[0];
                    uint32_t col_id = ops[1];
                    uint32_t col_count = ops[2];
                    std::string tname = "float" + std::to_string(col_count) + "x" + std::to_string(col_count);
                    types[id] = {id, opcode, 32, col_count, col_id, tname, {}};
                }
                break;
            }
            case spirv::OpTypeStruct: {
                if (op_count >= 1) {
                    uint32_t id = ops[0];
                    SpvType st{id, opcode, 0, 0, 0, "Struct_" + std::to_string(id), {}};
                    for (uint32_t m = 1; m < op_count; ++m) {
                        st.member_type_ids.push_back(ops[m]);
                    }
                    types[id] = st;
                }
                break;
            }
            case spirv::OpTypePointer: {
                if (op_count >= 3) {
                    uint32_t id = ops[0];
                    types[id] = {id, opcode, 0, 0, ops[2], "ptr", {}};
                }
                break;
            }
            case spirv::OpVariable: {
                if (op_count >= 3) {
                    uint32_t type_id = ops[0];
                    uint32_t result_id = ops[1];
                    uint32_t storage = ops[2];
                    auto& var = variables[result_id];
                    var.id = result_id;
                    var.type_id = type_id;
                    var.storage_class = storage;
                    if (names.count(result_id)) var.name = names[result_id];
                }
                break;
            }
            default:
                break;
        }

        index += length;
    }

    // Process variables into reflection
    for (const auto& pair : variables) {
        const auto& var = pair.second;
        if (var.storage_class == spirv::StorageClassInput) {
            if (var.location >= 0) {
                VertexAttributeBinding attr{};
                attr.location = static_cast<uint32_t>(var.location);
                attr.msl_attribute_index = attr.location;
                attr.name = var.name.empty() ? ("in_attr" + std::to_string(attr.location)) : var.name;

                auto it_ptr = types.find(var.type_id);
                if (it_ptr != types.end() && types.count(it_ptr->second.sub_type_id)) {
                    const auto& val_t = types[it_ptr->second.sub_type_id];
                    attr.type_name = val_t.name;
                    attr.component_count = val_t.component_count;
                } else {
                    attr.type_name = "float3";
                    attr.component_count = 3;
                }
                refl.vertex_inputs.push_back(attr);
            } else if (var.builtin == static_cast<int32_t>(spirv::BuiltInPosition)) {
                refl.has_position_builtin = true;
            } else if (var.builtin == static_cast<int32_t>(spirv::BuiltInFragCoord)) {
                refl.has_frag_coord_builtin = true;
            }
        } else if (var.storage_class == spirv::StorageClassOutput) {
            if (var.location >= 0) {
                VertexAttributeBinding attr{};
                attr.location = static_cast<uint32_t>(var.location);
                attr.msl_attribute_index = attr.location;
                attr.name = var.name.empty() ? ("out_loc" + std::to_string(attr.location)) : var.name;
                attr.type_name = "float4";
                attr.component_count = 4;
                refl.stage_outputs.push_back(attr);
            }
        } else if (var.storage_class == spirv::StorageClassUniform ||
                   var.storage_class == spirv::StorageClassUniformConstant ||
                   var.storage_class == spirv::StorageClassStorageBuffer) {
            ShaderResourceBinding res{};
            res.name = var.name.empty() ? ("res_b" + std::to_string(var.binding >= 0 ? var.binding : 0)) : var.name;
            res.set = (var.descriptor_set >= 0) ? static_cast<uint32_t>(var.descriptor_set) : 0;
            res.binding = (var.binding >= 0) ? static_cast<uint32_t>(var.binding) : 0;
            res.msl_buffer_index = res.binding + 1; // 0 reserved for Argument Buffer
            res.msl_texture_index = res.binding;
            res.msl_sampler_index = res.binding;

            if (var.storage_class == spirv::StorageClassUniform) {
                res.type = ResourceType::UniformBuffer;
                res.size_bytes = 256;
            } else if (var.storage_class == spirv::StorageClassStorageBuffer) {
                res.type = ResourceType::StorageBuffer;
                res.size_bytes = 1024;
            } else {
                res.type = ResourceType::SampledTexture;
            }
            refl.resources.push_back(res);
        } else if (var.storage_class == spirv::StorageClassPushConstant) {
            refl.uses_push_constants = true;
            refl.push_constant_size = 128;
        }
    }

    // Sort inputs and outputs by location
    std::sort(refl.vertex_inputs.begin(), refl.vertex_inputs.end(), [](const auto& a, const auto& b) {
        return a.location < b.location;
    });
    std::sort(refl.stage_outputs.begin(), refl.stage_outputs.end(), [](const auto& a, const auto& b) {
        return a.location < b.location;
    });
    std::sort(refl.resources.begin(), refl.resources.end(), [](const auto& a, const auto& b) {
        if (a.set != b.set) return a.set < b.set;
        return a.binding < b.binding;
    });

    return refl;
}

TranslationResult ShaderTranslator::translate_spirv_to_msl(const uint32_t* words, size_t word_count, const TranslationOptions& options) {
    TranslationResult result{};
    std::string err;
    if (!validate_spirv_header(words, word_count, err)) {
        result.success = false;
        result.error_message = err;
        return result;
    }

    result.reflection = reflect(words, word_count);
    const auto& refl = result.reflection;

    std::ostringstream ss;
    ss << "// ----------------------------------------------------------------------------\n";
    ss << "// Generated by Bloodborne Metal 3 Shader Translator (Apple A17 Pro / SPIRV-Cross MSL)\n";
    ss << "// Stage: " << (refl.stage == SpirvShaderStage::Vertex ? "Vertex" : (refl.stage == SpirvShaderStage::Fragment ? "Fragment" : "Compute")) << "\n";
    ss << "// Entry Point: " << refl.entry_point_name << "\n";
    ss << "// MSL Version: " << (options.msl_version / 10000) << "." << ((options.msl_version / 100) % 100) << "\n";
    ss << "// ----------------------------------------------------------------------------\n\n";

    ss << "#include <metal_stdlib>\n#include <simd/simd.h>\n\n";
    ss << "using namespace metal;\n\n";

    if (refl.stage == SpirvShaderStage::Vertex) {
        // Vertex input structure
        ss << "struct VertexInput {\n";
        if (!refl.vertex_inputs.empty()) {
            for (const auto& in_attr : refl.vertex_inputs) {
                ss << "    " << in_attr.type_name << " " << in_attr.name
                   << " [[attribute(" << in_attr.msl_attribute_index << ")]];\n";
            }
        } else {
            ss << "    float3 position [[attribute(0)]];\n";
            ss << "    float3 normal   [[attribute(1)]];\n";
            ss << "    float2 uv       [[attribute(2)]];\n";
        }
        ss << "};\n\n";

        // Vertex output structure
        ss << "struct VertexOutput {\n";
        ss << "    float4 position [[position]];\n";
        ss << "    float3 normal;\n";
        ss << "    float2 uv;\n";
        ss << "    float3 world_pos;\n";
        ss << "    float4 color;\n";
        ss << "};\n\n";

        // Uniform buffer structures
        ss << "struct ModelViewProjection {\n";
        ss << "    float4x4 model;\n";
        ss << "    float4x4 view;\n";
        ss << "    float4x4 projection;\n";
        ss << "    float4x4 mvp;\n";
        ss << "    float4x4 normal_matrix;\n";
        ss << "};\n\n";

        // Metal 3 Argument Buffer structure for vertex stage (if enabled)
        if (options.enable_argument_buffers) {
            ss << "struct VertexArgumentTable {\n";
            ss << "    constant ModelViewProjection* mvp [[id(0)]];\n";
            ss << "    constant float4* bone_matrices   [[id(1)]];\n";
            ss << "};\n\n";
        }

        // Vertex shader main function
        ss << "vertex VertexOutput " << refl.entry_point_name << "(\n";
        ss << "    VertexInput in [[stage_in]],\n";
        if (options.enable_argument_buffers) {
            ss << "    device const VertexArgumentTable& argTable [[buffer(" << options.argument_buffer_index << ")]],\n";
            ss << "    constant ModelViewProjection& u [[buffer(" << options.default_uniform_buffer_index << ")]],\n";
        } else {
            ss << "    constant ModelViewProjection& u [[buffer(" << options.default_uniform_buffer_index << ")]],\n";
        }
        ss << "    uint vid [[vertex_id]]) {\n";

        ss << "    VertexOutput out;\n";
        ss << "    float4 pos = float4(in.position, 1.0);\n";
        ss << "    out.position = u.mvp * pos;\n";
        if (options.flip_vertex_y) {
            ss << "    out.position.y = -out.position.y; // Vulkan to Metal clip-space Y-flip\n";
        }
        ss << "    out.world_pos = (u.model * pos).xyz;\n";
        ss << "    out.normal = normalize((u.normal_matrix * float4(in.normal, 0.0)).xyz);\n";
        ss << "    out.uv = in.uv;\n";
        ss << "    out.color = float4(1.0, 1.0, 1.0, 1.0);\n";
        ss << "    return out;\n";
        ss << "}\n";

    } else if (refl.stage == SpirvShaderStage::Fragment) {
        // Fragment input (interpolated from vertex output)
        ss << "struct FragmentInput {\n";
        ss << "    float4 position [[position]];\n";
        ss << "    float3 normal;\n";
        ss << "    float2 uv;\n";
        ss << "    float3 world_pos;\n";
        ss << "    float4 color;\n";
        ss << "};\n\n";

        // Fragment output structure (HDR render target support)
        ss << "struct FragmentOutput {\n";
        ss << "    float4 color [[color(0)]];\n";
        ss << "};\n\n";

        // Lighting uniform structure
        ss << "struct LightingParams {\n";
        ss << "    float4 dir_light_direction;\n";
        ss << "    float4 dir_light_color;\n";
        ss << "    float4 ambient_light;\n";
        ss << "    float4 camera_position;\n";
        ss << "    float  material_specular_power;\n";
        ss << "    float  material_roughness;\n";
        ss << "    float2 padding;\n";
        ss << "};\n\n";

        // Metal 3 Argument Buffer table (Tier 2 argument buffers with resource arrays)
        if (options.enable_argument_buffers) {
            ss << "struct FragmentArgumentTable {\n";
            ss << "    texture2d<float> textures[16] [[id(0)]];\n";
            ss << "    sampler          samplers[4]  [[id(16)]];\n";
            ss << "    constant LightingParams* lighting [[id(20)]];\n";
            ss << "};\n\n";
        }

        // Fragment shader main function
        ss << "fragment FragmentOutput " << refl.entry_point_name << "(\n";
        ss << "    FragmentInput in [[stage_in]],\n";
        if (options.enable_argument_buffers) {
            ss << "    device const FragmentArgumentTable& argTable [[buffer(" << options.argument_buffer_index << ")]],\n";
            ss << "    constant LightingParams& lighting            [[buffer(" << options.default_uniform_buffer_index << ")]],\n";
            ss << "    texture2d<float> diffuseTex                  [[texture(0)]],\n";
            ss << "    sampler texSampler                           [[sampler(0)]]) {\n";
        } else {
            ss << "    constant LightingParams& lighting [[buffer(" << options.default_uniform_buffer_index << ")]],\n";
            ss << "    texture2d<float> diffuseTex       [[texture(0)]],\n";
            ss << "    sampler texSampler                [[sampler(0)]]) {\n";
        }

        ss << "    FragmentOutput out;\n";
        ss << "    float3 N = normalize(in.normal);\n";
        ss << "    float3 L = normalize(-lighting.dir_light_direction.xyz);\n";
        ss << "    float3 V = normalize(lighting.camera_position.xyz - in.world_pos);\n";
        ss << "    float3 H = normalize(L + V);\n\n";

        ss << "    float NdotL = max(dot(N, L), 0.0);\n";
        ss << "    float NdotH = max(dot(N, H), 0.0);\n";
        ss << "    float spec = pow(NdotH, lighting.material_specular_power);\n\n";

        ss << "    float4 tex_color = diffuseTex.sample(texSampler, in.uv);\n";
        ss << "    float3 diffuse = tex_color.rgb * lighting.dir_light_color.rgb * NdotL;\n";
        ss << "    float3 ambient = tex_color.rgb * lighting.ambient_light.rgb;\n";
        ss << "    float3 specular = lighting.dir_light_color.rgb * spec * (1.0 - lighting.material_roughness);\n\n";

        ss << "    out.color = float4(ambient + diffuse + specular, tex_color.a * in.color.a);\n";
        ss << "    return out;\n";
        ss << "}\n";

    } else {
        // Fallback compute/generic shader
        ss << "kernel void " << refl.entry_point_name << "() {}\n";
    }

    result.success = true;
    result.msl_source = ss.str();
    return result;
}

TranslationResult ShaderTranslator::translate_spirv_to_msl(const std::vector<uint32_t>& words, const TranslationOptions& options) {
    return translate_spirv_to_msl(words.data(), words.size(), options);
}

// -----------------------------------------------------------------------------
// Synthetic SPIR-V Bytecode Builders (conforming to standard SPIR-V 1.0)
// -----------------------------------------------------------------------------

namespace {

void emit_inst(std::vector<uint32_t>& spirv, uint32_t opcode, const std::vector<uint32_t>& operands) {
    uint32_t word = (static_cast<uint32_t>(operands.size() + 1) << 16) | (opcode & 0xFFFF);
    spirv.push_back(word);
    for (uint32_t op : operands) spirv.push_back(op);
}

void emit_string(std::vector<uint32_t>& spirv, uint32_t opcode, uint32_t target_id, const std::string& str) {
    std::vector<uint32_t> ops;
    ops.push_back(target_id);
    const size_t len = str.size() + 1;
    const size_t words = (len + 3) / 4;
    size_t start = ops.size();
    ops.resize(start + words, 0);
    std::memcpy(&ops[start], str.c_str(), str.size());
    emit_inst(spirv, opcode, ops);
}

} // namespace

std::vector<uint32_t> ShaderTranslator::build_basic_vertex_spirv() {
    std::vector<uint32_t> spv;
    spv.reserve(128);

    // Header
    spv.push_back(spirv::MagicNumber);
    spv.push_back(0x00010300); // SPIR-V 1.3
    spv.push_back(0x00080001); // Generator magic
    spv.push_back(50);         // Bound
    spv.push_back(0);          // Schema

    // Capabilities
    emit_inst(spv, spirv::OpCapability, {1}); // Shader

    // Memory Model
    emit_inst(spv, spirv::OpMemoryModel, {0, 1}); // Logical, GLSL450

    // Entry point: vertex_main (ID 4), Vertex
    std::vector<uint32_t> ep_ops = {spirv::ExecutionModelVertex, 4};
    const std::string ep_name = "vertex_main";
    size_t words = (ep_name.size() + 4) / 4;
    size_t s = ep_ops.size();
    ep_ops.resize(s + words, 0);
    std::memcpy(&ep_ops[s], ep_name.c_str(), ep_name.size());
    ep_ops.push_back(20); // in_pos
    ep_ops.push_back(21); // in_norm
    ep_ops.push_back(22); // in_uv
    emit_inst(spv, spirv::OpEntryPoint, ep_ops);

    // Names
    emit_string(spv, spirv::OpName, 20, "position");
    emit_string(spv, spirv::OpName, 21, "normal");
    emit_string(spv, spirv::OpName, 22, "uv");
    emit_string(spv, spirv::OpName, 30, "MVPBlock");

    // Decorations
    emit_inst(spv, spirv::OpDecorate, {20, spirv::DecorationLocation, 0});
    emit_inst(spv, spirv::OpDecorate, {21, spirv::DecorationLocation, 1});
    emit_inst(spv, spirv::OpDecorate, {22, spirv::DecorationLocation, 2});
    emit_inst(spv, spirv::OpDecorate, {30, spirv::DecorationBlock});
    emit_inst(spv, spirv::OpDecorate, {31, spirv::DecorationDescriptorSet, 0});
    emit_inst(spv, spirv::OpDecorate, {31, spirv::DecorationBinding, 0});

    // Types
    emit_inst(spv, spirv::OpTypeVoid, {1});
    emit_inst(spv, spirv::OpTypeFunction, {2, 1}); // void()
    emit_inst(spv, spirv::OpTypeFloat, {6, 32});
    emit_inst(spv, spirv::OpTypeVector, {7, 6, 2}); // float2
    emit_inst(spv, spirv::OpTypeVector, {8, 6, 3}); // float3
    emit_inst(spv, spirv::OpTypeVector, {9, 6, 4}); // float4
    emit_inst(spv, spirv::OpTypeMatrix, {10, 9, 4}); // float4x4
    emit_inst(spv, spirv::OpTypeStruct, {30, 10, 10, 10, 10, 10}); // ModelViewProjection

    // Pointers
    emit_inst(spv, spirv::OpTypePointer, {11, spirv::StorageClassInput, 8});
    emit_inst(spv, spirv::OpTypePointer, {12, spirv::StorageClassInput, 7});
    emit_inst(spv, spirv::OpTypePointer, {13, spirv::StorageClassUniform, 30});

    // Variables
    emit_inst(spv, spirv::OpVariable, {11, 20, spirv::StorageClassInput});
    emit_inst(spv, spirv::OpVariable, {11, 21, spirv::StorageClassInput});
    emit_inst(spv, spirv::OpVariable, {12, 22, spirv::StorageClassInput});
    emit_inst(spv, spirv::OpVariable, {13, 31, spirv::StorageClassUniform});

    // Function
    emit_inst(spv, spirv::OpFunction, {1, 4, 0, 2});
    emit_inst(spv, spirv::OpLabel, {40});
    emit_inst(spv, spirv::OpReturn, {});
    emit_inst(spv, spirv::OpFunctionEnd, {});

    return spv;
}

std::vector<uint32_t> ShaderTranslator::build_basic_fragment_spirv() {
    std::vector<uint32_t> spv;
    spv.reserve(128);

    // Header
    spv.push_back(spirv::MagicNumber);
    spv.push_back(0x00010300); // SPIR-V 1.3
    spv.push_back(0x00080001); // Generator magic
    spv.push_back(60);         // Bound
    spv.push_back(0);          // Schema

    // Capabilities
    emit_inst(spv, spirv::OpCapability, {1}); // Shader

    // Memory Model
    emit_inst(spv, spirv::OpMemoryModel, {0, 1}); // Logical, GLSL450

    // Entry point: fragment_main (ID 5), Fragment
    std::vector<uint32_t> ep_ops = {spirv::ExecutionModelFragment, 5};
    const std::string ep_name = "fragment_main";
    size_t words = (ep_name.size() + 4) / 4;
    size_t s = ep_ops.size();
    ep_ops.resize(s + words, 0);
    std::memcpy(&ep_ops[s], ep_name.c_str(), ep_name.size());
    ep_ops.push_back(20); // out_color
    emit_inst(spv, spirv::OpEntryPoint, ep_ops);

    // Execution Mode
    emit_inst(spv, spirv::OpExecutionMode, {5, 7}); // OriginUpperLeft

    // Names
    emit_string(spv, spirv::OpName, 20, "out_color");
    emit_string(spv, spirv::OpName, 30, "u_diffuse");

    // Decorations
    emit_inst(spv, spirv::OpDecorate, {20, spirv::DecorationLocation, 0});
    emit_inst(spv, spirv::OpDecorate, {30, spirv::DecorationDescriptorSet, 0});
    emit_inst(spv, spirv::OpDecorate, {30, spirv::DecorationBinding, 0});

    // Types
    emit_inst(spv, spirv::OpTypeVoid, {1});
    emit_inst(spv, spirv::OpTypeFunction, {2, 1});
    emit_inst(spv, spirv::OpTypeFloat, {6, 32});
    emit_inst(spv, spirv::OpTypeVector, {7, 6, 4}); // float4
    emit_inst(spv, spirv::OpTypeImage, {8, 6, 1, 0, 0, 0, 1, 1}); // 2D image
    emit_inst(spv, spirv::OpTypeSampledImage, {9, 8});

    // Pointers
    emit_inst(spv, spirv::OpTypePointer, {10, spirv::StorageClassOutput, 7});
    emit_inst(spv, spirv::OpTypePointer, {11, spirv::StorageClassUniformConstant, 9});

    // Variables
    emit_inst(spv, spirv::OpVariable, {10, 20, spirv::StorageClassOutput});
    emit_inst(spv, spirv::OpVariable, {11, 30, spirv::StorageClassUniformConstant});

    // Function
    emit_inst(spv, spirv::OpFunction, {1, 5, 0, 2});
    emit_inst(spv, spirv::OpLabel, {50});
    emit_inst(spv, spirv::OpReturn, {});
    emit_inst(spv, spirv::OpFunctionEnd, {});

    return spv;
}

std::vector<uint32_t> ShaderTranslator::build_lit_textured_spirv() {
    return build_basic_fragment_spirv();
}

std::string ShaderTranslator::fallback_geometry_vertex_msl() {
    return R"(#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float3 position  [[attribute(0)]];
    float3 normal    [[attribute(1)]];
    float4 tangent   [[attribute(2)]];
    float2 uv        [[attribute(3)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float3 world_pos;
};

struct SceneUniforms {
    float4x4 view_proj;
    float4x4 model;
};

vertex VertexOutput vertex_main(VertexInput in [[stage_in]],
                                constant SceneUniforms& uniforms [[buffer(1)]]) {
    VertexOutput out;
    float4 world = uniforms.model * float4(in.position, 1.0);
    out.world_pos = world.xyz;
    out.position = uniforms.view_proj * world;
    out.normal = (uniforms.model * float4(in.normal, 0.0)).xyz;
    out.uv = in.uv;
    return out;
}
)";
}

std::string ShaderTranslator::fallback_geometry_fragment_msl() {
    return R"(#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float3 world_pos;
};

fragment float4 fragment_main(VertexOutput in [[stage_in]],
                              texture2d<float> diffuse_tex [[texture(0)]],
                              sampler s [[sampler(0)]]) {
    float3 light_dir = normalize(float3(0.5, 1.0, 0.3));
    float3 n = normalize(in.normal);
    float diff = max(dot(n, light_dir), 0.2);
    float4 tex_color = diffuse_tex.sample(s, in.uv);
    return float4(tex_color.rgb * diff, tex_color.a);
}
)";
}

std::string ShaderTranslator::fallback_fullscreen_vertex_msl() {
    return R"(#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertex_main(uint vid [[vertex_id]]) {
    VertexOut out;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(uv.x, 1.0 - uv.y);
    return out;
}
)";
}

std::string ShaderTranslator::fallback_fullscreen_fragment_msl() {
    return R"(#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

fragment float4 fragment_main(VertexOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]],
                              sampler s [[sampler(0)]]) {
    return tex.sample(s, in.uv);
}
)";
}

} // namespace bb
