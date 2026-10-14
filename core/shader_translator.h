#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace bb {

enum class ShaderStage {
    Vertex,
    Fragment,
    Compute,
    Unknown
};

enum class ResourceType {
    UniformBuffer,
    StorageBuffer,
    SampledTexture,
    StorageTexture,
    Sampler,
    PushConstant
};

struct ShaderResourceBinding {
    std::string name;
    uint32_t set = 0;
    uint32_t binding = 0;
    uint32_t msl_buffer_index = 0;
    uint32_t msl_texture_index = 0;
    uint32_t msl_sampler_index = 0;
    ResourceType type = ResourceType::UniformBuffer;
    uint32_t size_bytes = 0;
};

struct VertexAttributeBinding {
    uint32_t location = 0;
    uint32_t msl_attribute_index = 0;
    std::string name;
    std::string type_name; // e.g. "float3", "float4", "float2"
    uint32_t component_count = 3;
};

struct ShaderReflection {
    ShaderStage stage = ShaderStage::Unknown;
    std::string entry_point_name = "main0";
    std::vector<ShaderResourceBinding> resources;
    std::vector<VertexAttributeBinding> vertex_inputs;
    std::vector<VertexAttributeBinding> stage_outputs;
    bool uses_argument_buffers = false;
    bool uses_push_constants = false;
    uint32_t push_constant_size = 0;
    bool has_position_builtin = false;
    bool has_frag_coord_builtin = false;
};

struct TranslationOptions {
    uint32_t msl_version = 30000;          // MSL 3.0 (Metal 3 for Apple A17 Pro)
    bool enable_argument_buffers = true;    // Metal 3 Tier 2 argument buffers
    bool flip_vertex_y = true;             // Invert Vulkan clip-space Y for Metal viewport
    uint32_t argument_buffer_index = 0;    // [[buffer(0)]] reserved for Argument Buffer
    uint32_t default_uniform_buffer_index = 1; // [[buffer(1)]]
    uint32_t push_constant_buffer_index = 2;   // [[buffer(2)]]
    uint32_t vertex_stage_in_index = 0;        // [[stage_in]]
};

struct TranslationResult {
    bool success = false;
    std::string msl_source;
    std::string error_message;
    ShaderReflection reflection;
};

// SPIR-V Opcode constants (standard SPIR-V 1.0 - 1.6)
namespace spirv {
    constexpr uint32_t MagicNumber = 0x07230203;
    constexpr uint32_t MagicNumberReversed = 0x03022307;

    constexpr uint32_t OpNop = 0;
    constexpr uint32_t OpSource = 3;
    constexpr uint32_t OpName = 5;
    constexpr uint32_t OpMemberName = 6;
    constexpr uint32_t OpExtInstImport = 11;
    constexpr uint32_t OpExtInst = 12;
    constexpr uint32_t OpMemoryModel = 14;
    constexpr uint32_t OpEntryPoint = 15;
    constexpr uint32_t OpExecutionMode = 16;
    constexpr uint32_t OpCapability = 17;
    constexpr uint32_t OpTypeVoid = 19;
    constexpr uint32_t OpTypeBool = 20;
    constexpr uint32_t OpTypeInt = 21;
    constexpr uint32_t OpTypeFloat = 22;
    constexpr uint32_t OpTypeVector = 23;
    constexpr uint32_t OpTypeMatrix = 24;
    constexpr uint32_t OpTypeImage = 25;
    constexpr uint32_t OpTypeSampler = 26;
    constexpr uint32_t OpTypeSampledImage = 27;
    constexpr uint32_t OpTypeArray = 28;
    constexpr uint32_t OpTypeRuntimeArray = 29;
    constexpr uint32_t OpTypeStruct = 30;
    constexpr uint32_t OpTypePointer = 32;
    constexpr uint32_t OpTypeFunction = 33;
    constexpr uint32_t OpConstantTrue = 41;
    constexpr uint32_t OpConstantFalse = 42;
    constexpr uint32_t OpConstant = 43;
    constexpr uint32_t OpConstantComposite = 44;
    constexpr uint32_t OpVariable = 59;
    constexpr uint32_t OpLoad = 61;
    constexpr uint32_t OpStore = 62;
    constexpr uint32_t OpAccessChain = 65;
    constexpr uint32_t OpDecorate = 71;
    constexpr uint32_t OpMemberDecorate = 72;
    constexpr uint32_t OpVectorShuffle = 79;
    constexpr uint32_t OpCompositeConstruct = 80;
    constexpr uint32_t OpCompositeExtract = 81;
    constexpr uint32_t OpCompositeInsert = 82;
    constexpr uint32_t OpFNegate = 127;
    constexpr uint32_t OpFAdd = 129;
    constexpr uint32_t OpFSub = 131;
    constexpr uint32_t OpFMul = 133;
    constexpr uint32_t OpFDiv = 136;
    constexpr uint32_t OpDot = 148;
    constexpr uint32_t OpMatrixTimesVector = 145;
    constexpr uint32_t OpMatrixTimesMatrix = 146;
    constexpr uint32_t OpVectorTimesScalar = 142;
    constexpr uint32_t OpIAdd = 128;
    constexpr uint32_t OpISub = 130;
    constexpr uint32_t OpIMul = 132;
    constexpr uint32_t OpBitwiseAnd = 197;
    constexpr uint32_t OpBitwiseOr = 198;
    constexpr uint32_t OpBitwiseXor = 199;
    constexpr uint32_t OpSelect = 169;
    constexpr uint32_t OpImageSampleImplicitLod = 87;
    constexpr uint32_t OpImageSampleExplicitLod = 88;
    constexpr uint32_t OpLabel = 248;
    constexpr uint32_t OpBranch = 249;
    constexpr uint32_t OpBranchConditional = 250;
    constexpr uint32_t OpReturn = 253;
    constexpr uint32_t OpReturnValue = 254;
    constexpr uint32_t OpFunction = 54;
    constexpr uint32_t OpFunctionParameter = 55;
    constexpr uint32_t OpFunctionEnd = 56;

    // Execution Model
    constexpr uint32_t ExecutionModelVertex = 0;
    constexpr uint32_t ExecutionModelFragment = 4;
    constexpr uint32_t ExecutionModelGLCompute = 5;

    // Storage Class
    constexpr uint32_t StorageClassUniformConstant = 0;
    constexpr uint32_t StorageClassInput = 1;
    constexpr uint32_t StorageClassUniform = 2;
    constexpr uint32_t StorageClassOutput = 3;
    constexpr uint32_t StorageClassWorkgroup = 4;
    constexpr uint32_t StorageClassCrossWorkgroup = 5;
    constexpr uint32_t StorageClassPrivate = 6;
    constexpr uint32_t StorageClassFunction = 7;
    constexpr uint32_t StorageClassPushConstant = 9;
    constexpr uint32_t StorageClassStorageBuffer = 12;

    // Decoration
    constexpr uint32_t DecorationBlock = 2;
    constexpr uint32_t DecorationBufferBlock = 3;
    constexpr uint32_t DecorationColMajor = 5;
    constexpr uint32_t DecorationRowMajor = 6;
    constexpr uint32_t DecorationArrayStride = 7;
    constexpr uint32_t DecorationMatrixStride = 8;
    constexpr uint32_t DecorationBuiltIn = 11;
    constexpr uint32_t DecorationLocation = 30;
    constexpr uint32_t DecorationBinding = 33;
    constexpr uint32_t DecorationDescriptorSet = 34;
    constexpr uint32_t DecorationOffset = 35;

    // BuiltIn
    constexpr uint32_t BuiltInPosition = 0;
    constexpr uint32_t BuiltInPointSize = 1;
    constexpr uint32_t BuiltInVertexId = 5;
    constexpr uint32_t BuiltInInstanceId = 6;
    constexpr uint32_t BuiltInFragCoord = 15;
    constexpr uint32_t BuiltInPointCoord = 16;
    constexpr uint32_t BuiltInFrontFacing = 17;
    constexpr uint32_t BuiltInFragDepth = 22;
    constexpr uint32_t BuiltInVertexIndex = 42;
    constexpr uint32_t BuiltInInstanceIndex = 43;
} // namespace spirv

class ShaderTranslator {
public:
    // Header validation
    static bool validate_spirv_header(const uint32_t* words, size_t word_count, std::string& out_error);

    // Full translation from SPIR-V to Metal Shading Language (MSL)
    static TranslationResult translate_spirv_to_msl(const uint32_t* words, size_t word_count, const TranslationOptions& options = {});
    static TranslationResult translate_spirv_to_msl(const std::vector<uint32_t>& words, const TranslationOptions& options = {});

    // Reflection extraction without full code generation
    static ShaderReflection reflect(const uint32_t* words, size_t word_count);

    // Synthetic shader generators for testing and baseline pipelines
    static std::vector<uint32_t> build_basic_vertex_spirv();
    static std::vector<uint32_t> build_basic_fragment_spirv();
    static std::vector<uint32_t> build_lit_textured_spirv();
};

} // namespace bb
