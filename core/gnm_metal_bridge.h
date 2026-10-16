#pragma once

#include "gnm_context.h"
#include "metal_pipeline.h"
#include "gcn_isa.h"
#include "gcn_ir.h"
#include "gcn_msl.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bb {

// =============================================================================
// Erros da Ponte Gnm -> Metal
// =============================================================================
enum class BridgeError {
    Ok = 0,
    InvalidGuestMemory,
    InvalidBufferAddress,
    InvalidTextureAddress,
    InvalidBindingSlot,
    InvalidShaderAddress,
    ShaderReadFailed,
    ShaderExceedsLimit,
    ShaderDecodingFailed,
    ShaderLoweringFailed,
    ShaderTranslationFailed,
    InvalidIndexBuffer,
    InvalidTopology,
    PipelineConfigurationFailed,
    EmptyDrawCall,
    Unknown
};

const char* bridge_error_to_string(BridgeError err);

template <typename T>
struct BridgeResult {
    BridgeError error = BridgeError::Ok;
    std::string message;
    T value{};

    bool ok() const { return error == BridgeError::Ok; }
    explicit operator bool() const { return ok(); }
    const T& operator*() const { return value; }
    const T* operator->() const { return &value; }
    T& operator*() { return value; }
    T* operator->() { return &value; }
};

// =============================================================================
// Topologia e Mistura (Blend) Metal
// =============================================================================
enum class MetalPrimitiveType : std::uint32_t {
    Point = 0,
    Line = 1,
    LineStrip = 2,
    Triangle = 3,
    TriangleStrip = 4
};

enum class MetalBlendFactor : std::uint32_t {
    Zero = 0,
    One = 1,
    SourceColor = 2,
    OneMinusSourceColor = 3,
    SourceAlpha = 4,
    OneMinusSourceAlpha = 5,
    DestinationColor = 6,
    OneMinusDestinationColor = 7,
    DestinationAlpha = 8,
    OneMinusDestinationAlpha = 9
};

enum class MetalBlendOperation : std::uint32_t {
    Add = 0,
    Subtract = 1,
    ReverseSubtract = 2,
    Min = 3,
    Max = 4
};

// =============================================================================
// Configuração de Pipeline Metal (Render e Compute)
// =============================================================================
struct MetalColorAttachmentConfig {
    MetalPixelFormat pixel_format = MetalPixelFormat::Invalid;
    bool blending_enabled = false;
    MetalBlendFactor source_rgb_blend_factor = MetalBlendFactor::One;
    MetalBlendFactor destination_rgb_blend_factor = MetalBlendFactor::Zero;
    MetalBlendOperation rgb_blend_operation = MetalBlendOperation::Add;
    MetalBlendFactor source_alpha_blend_factor = MetalBlendFactor::One;
    MetalBlendFactor destination_alpha_blend_factor = MetalBlendFactor::Zero;
    MetalBlendOperation alpha_blend_operation = MetalBlendOperation::Add;
    std::uint8_t write_mask = 0x0F;
};

struct MetalDepthStencilConfig {
    MetalPixelFormat depth_pixel_format = MetalPixelFormat::Invalid;
    MetalPixelFormat stencil_pixel_format = MetalPixelFormat::Invalid;
    bool depth_test_enabled = false;
    bool depth_write_enabled = false;
    MetalCompareFunction depth_compare_function = MetalCompareFunction::Always;
    bool stencil_test_enabled = false;
    bool stencil_write_enabled = false;
    MetalCompareFunction stencil_compare_function = MetalCompareFunction::Always;
    std::uint8_t stencil_read_mask = 0xFF;
    std::uint8_t stencil_write_mask = 0xFF;
};

struct MetalRenderPipelineConfig {
    std::uint64_t vs_hash = 0;
    std::uint64_t ps_hash = 0;
    std::string vs_msl;
    std::string ps_msl;
    std::array<MetalColorAttachmentConfig, 8> color_attachments{};
    std::uint32_t active_color_attachments = 0;
    MetalDepthStencilConfig depth_stencil{};
    MetalPrimitiveType primitive_type = MetalPrimitiveType::Triangle;
    MetalCullMode cull_mode = MetalCullMode::None;
    MetalWinding front_face = MetalWinding::Clockwise;
    bool alpha_to_coverage = false;

    bool is_valid() const {
        return vs_hash != 0 && !vs_msl.empty();
    }
};

struct MetalComputePipelineConfig {
    std::uint64_t cs_hash = 0;
    std::string cs_msl;
    std::uint32_t threads_per_threadgroup_x = 1;
    std::uint32_t threads_per_threadgroup_y = 1;
    std::uint32_t threads_per_threadgroup_z = 1;

    bool is_valid() const {
        return cs_hash != 0 && !cs_msl.empty() &&
               threads_per_threadgroup_x > 0 &&
               threads_per_threadgroup_y > 0 &&
               threads_per_threadgroup_z > 0;
    }
};

// =============================================================================
// Estruturas de Bindings de Recursos Metal
// =============================================================================
struct MetalBufferBinding {
    std::uint32_t slot = 0;
    std::uint64_t guest_address = 0;
    std::size_t size = 0;
    std::uint32_t stride = 0;
    bool valid = false;
};

struct MetalTextureBinding {
    std::uint32_t slot = 0;
    std::uint64_t guest_address = 0;
    MetalPixelFormat format = MetalPixelFormat::Invalid;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t num_mips = 1;
    bool valid = false;
};

struct MetalSamplerBinding {
    std::uint32_t slot = 0;
    std::uint32_t min_filter = 0;
    std::uint32_t mag_filter = 0;
    std::uint32_t mip_filter = 0;
    float min_lod = 0.0f;
    float max_lod = 15.0f;
    bool valid = false;
};

struct MetalResourceBindings {
    static constexpr std::size_t MaxBuffers = 31;   // buffer(0..30)
    static constexpr std::size_t MaxTextures = 31;  // texture(0..30)
    static constexpr std::size_t MaxSamplers = 16;  // sampler(0..15)

    std::array<MetalBufferBinding, MaxBuffers> buffers{};
    std::array<MetalTextureBinding, MaxTextures> textures{};
    std::array<MetalSamplerBinding, MaxSamplers> samplers{};

    std::uint32_t active_buffers = 0;
    std::uint32_t active_textures = 0;
    std::uint32_t active_samplers = 0;

    bool bind_buffer(std::uint32_t slot, const BufferDescriptor& desc);
    bool bind_texture(std::uint32_t slot, const TextureDescriptor& desc);
    bool bind_sampler(std::uint32_t slot, const SamplerDescriptor& desc);
    void clear();
};

// =============================================================================
// Comandos de Desenho e Computação Prontos para Metal
// =============================================================================
struct MetalDrawCommand {
    MetalRenderPipelineConfig pipeline_config{};
    MetalResourceBindings vertex_bindings{};
    MetalResourceBindings fragment_bindings{};

    bool indexed = false;
    std::uint32_t index_count = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t start_index = 0;
    std::uint64_t index_buffer_address = 0;
    GnmIndexType index_type = GnmIndexType::Index16;
    std::uint32_t instance_count = 1;
    std::uint32_t start_instance = 0;

    Viewport viewport{};
    ScissorRect scissor{};

    bool valid = false;
};

struct MetalDispatchCommand {
    MetalComputePipelineConfig pipeline_config{};
    MetalResourceBindings compute_bindings{};

    std::uint32_t threadgroups_x = 0;
    std::uint32_t threadgroups_y = 0;
    std::uint32_t threadgroups_z = 0;
    std::uint32_t threads_per_threadgroup_x = 1;
    std::uint32_t threads_per_threadgroup_y = 1;
    std::uint32_t threads_per_threadgroup_z = 1;

    bool valid = false;
};

// =============================================================================
// Wrappers GcnDecoder e GcnMslTranslator
// =============================================================================
class GcnDecoder {
public:
    static bb::gcn::Program decode(const std::uint32_t* words, std::size_t word_count);
};

class GcnMslTranslator {
public:
    static bb::gcn::MslResult translate(const bb::gcn::ProgramIR& ir, ShaderStage stage);
};

// =============================================================================
// Cache Thread-Safe de Shaders Traduzidos
// =============================================================================
struct CachedShader {
    std::uint64_t hash = 0;
    ShaderStage stage = ShaderStage::Vertex;
    std::string msl_source;
    std::uint32_t instruction_count = 0;
    std::size_t byte_size = 0;
    std::vector<std::uint32_t> bound_buffers;
    std::vector<std::uint32_t> bound_textures;
    std::vector<std::uint32_t> bound_samplers;
};

class ShaderCache {
public:
    static constexpr std::size_t MAX_SHADER_BYTES = 65536; // Limite estrito de 64 KB

    explicit ShaderCache(std::size_t max_entries = 1024);

    BridgeResult<const CachedShader*> get_or_compile(
        const GuestMemory& memory,
        std::uint64_t guest_address,
        ShaderStage stage
    );

    void insert(std::uint64_t hash, CachedShader shader);
    const CachedShader* find(std::uint64_t hash) const;

    std::size_t size() const;
    std::uint64_t hit_count() const { return hit_count_.load(); }
    std::uint64_t miss_count() const { return miss_count_.load(); }
    void clear();

    static std::uint64_t calculate_hash(const void* data, std::size_t size);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, CachedShader> cache_;
    std::size_t max_entries_;
    mutable std::atomic<std::uint64_t> hit_count_{0};
    mutable std::atomic<std::uint64_t> miss_count_{0};
};

// =============================================================================
// GnmMetalBridge: Conector Principal entre GnmContext e Metal
// =============================================================================
class GnmMetalBridge {
public:
    explicit GnmMetalBridge(const GuestMemory& memory);

    // Gestão de Bindings
    bool bind_buffer(ShaderStage stage, std::uint32_t slot, const BufferDescriptor& desc);
    bool bind_texture(ShaderStage stage, std::uint32_t slot, const TextureDescriptor& desc);
    bool bind_sampler(ShaderStage stage, std::uint32_t slot, const SamplerDescriptor& desc);

    bool bind_buffer_table(ShaderStage stage, std::uint32_t base_slot, std::uint64_t table_addr, std::uint32_t count);
    bool bind_texture_table(ShaderStage stage, std::uint32_t base_slot, std::uint64_t table_addr, std::uint32_t count);
    bool bind_sampler_table(ShaderStage stage, std::uint32_t base_slot, std::uint64_t table_addr, std::uint32_t count);

    void import_user_data_buffers(const GnmContext& context, ShaderStage stage);
    void clear_bindings(ShaderStage stage);
    const MetalResourceBindings& bindings(ShaderStage stage) const;

    // Execução e Processamento de Draw / Dispatch
    BridgeResult<MetalDrawCommand> process_draw_call(const DrawCall& draw_call);
    BridgeResult<MetalDispatchCommand> process_dispatch(const DispatchCall& dispatch_call);

    // Mapeamentos estáticos Gnm -> Metal
    static MetalPixelFormat map_surface_format(GnmSurfaceFormat format);
    static MetalBlendFactor map_blend_factor(GnmBlendFactor factor);
    static MetalBlendOperation map_blend_op(GnmBlendOp op);
    static MetalCompareFunction map_compare_func(GnmCompareFunc func);
    static MetalPrimitiveType map_primitive_type(GnmPrimitiveType prim, bool& out_supported);
    static MetalCullMode map_cull_mode(GnmCullMode mode);
    static MetalWinding map_front_face(GnmFrontFace face);

    // Acesso ao cache
    ShaderCache& shader_cache() { return shader_cache_; }
    const ShaderCache& shader_cache() const { return shader_cache_; }

    // Bounds checking
    bool is_buffer_address_valid(std::uint64_t address, std::size_t size) const;

    // Shaders MSL de fallback para geometria 3D, normais e texturas (T015)
    static CachedShader get_fallback_shader(ShaderStage stage);
    void set_allow_fallback_shaders(bool allow) { allow_fallback_ = allow; }
    bool allow_fallback_shaders() const { return allow_fallback_; }

private:
    const GuestMemory& memory_;
    ShaderCache shader_cache_;
    bool allow_fallback_{false};

    MetalResourceBindings vs_bindings_{};
    MetalResourceBindings ps_bindings_{};
    MetalResourceBindings cs_bindings_{};
};

} // namespace bb
