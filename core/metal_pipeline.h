#pragma once

#include "flver_parser.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace bb {

enum class MetalPixelFormat {
    BGRA8Unorm,
    RGBA16Float,      // HDR Render Target (16-bit float per channel)
    RG11B10Float,     // Compact HDR Render Target
    Depth32Float,     // 32-bit floating point depth buffer (Z-buffer)
    Depth32Float_Stencil8,
    Invalid
};

enum class MetalCompareFunction {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class MetalCullMode {
    None,
    Front,
    Back
};

enum class MetalWinding {
    Clockwise,
    CounterClockwise
};

struct DepthStencilStateDescriptor {
    bool depth_write_enabled = true;
    MetalCompareFunction depth_compare_function = MetalCompareFunction::LessEqual;
    bool stencil_enabled = false;

    bool is_valid() const {
        return true;
    }
};

struct RenderPipelineConfig {
    MetalPixelFormat color_format = MetalPixelFormat::RGBA16Float; // HDR by default
    MetalPixelFormat depth_format = MetalPixelFormat::Depth32Float; // 3D depth buffer
    uint32_t sample_count = 1; // 1 or 4
    MetalCullMode cull_mode = MetalCullMode::Back;
    MetalWinding winding = MetalWinding::Clockwise;
    bool alpha_to_coverage_enabled = false;

    bool validate(std::string& out_error) const;
};

// 4x4 column-major matrix matching Metal float4x4 / simd_float4x4
struct alignas(16) Matrix4x4 {
    float m[16];

    static Matrix4x4 identity();
    static Matrix4x4 perspective(float fov_radians, float aspect, float z_near, float z_far);
    static Matrix4x4 look_at(const float* eye, const float* target, const float* up);
    static Matrix4x4 rotation_y(float angle_radians);
    static Matrix4x4 rotation_x(float angle_radians);
    static Matrix4x4 multiply(const Matrix4x4& a, const Matrix4x4& b);
    static Matrix4x4 normal_matrix(const Matrix4x4& model_view);
};

struct alignas(16) LightingUniforms {
    float dir_light_direction[4] = {0.577f, -0.577f, 0.577f, 0.0f}; // Normalized
    float dir_light_color[4]     = {1.0f, 0.95f, 0.85f, 1.2f};      // Warm sunlight
    float ambient_light[4]       = {0.12f, 0.14f, 0.18f, 1.0f};     // Cool ambient
    float camera_position[4]     = {0.0f, 1.5f, 3.5f, 1.0f};
    float specular_power         = 32.0f;
    float roughness              = 0.35f;
    float padding[2]             = {0.0f, 0.0f};
};

struct alignas(16) ModelViewProjectionUniforms {
    Matrix4x4 model;
    Matrix4x4 view;
    Matrix4x4 projection;
    Matrix4x4 mvp;
    Matrix4x4 normal_matrix;
};

// -----------------------------------------------------------------------------
// Metal 3 Tier 2 Argument Buffer Table (Apple A17 Pro GPU)
// -----------------------------------------------------------------------------
// On A17 Pro (Apple9/Metal3), Tier 2 Argument Buffers encode 64-bit GPU virtual
// addresses directly in unified memory.
struct alignas(8) Metal3ArgumentTable {
    static constexpr size_t MaxTextures = 32;
    static constexpr size_t MaxSamplers = 8;
    static constexpr size_t MaxBuffers  = 8;

    uint64_t texture_gpu_addresses[MaxTextures] = {0};
    uint64_t sampler_descriptors[MaxSamplers]   = {0};
    uint64_t uniform_buffer_addresses[MaxBuffers] = {0};

    void bind_texture(uint32_t slot, uint64_t gpu_address);
    void bind_sampler(uint32_t slot, uint64_t sampler_handle);
    void bind_buffer(uint32_t slot, uint64_t gpu_address);
    bool validate() const;
};

// CPU-side gate before conversion/allocation. Never claims a drawable was presented.
// The byte limit applies independently to vertex and index buffers.
enum class MeshPreparationStatus {
    Ready, Empty, TooLarge, InvalidIndex, NonFinite, InvalidBuffer
};
MeshPreparationStatus prepare_metal_mesh(const FlverModel& model, MetalMeshBuffer& out,
                                         size_t max_buffer_bytes);
const char* mesh_preparation_status_string(MeshPreparationStatus status);

class MetalPipelineManager {
public:
    static bool is_hdr_format(MetalPixelFormat format);
    static bool is_depth_format(MetalPixelFormat format);
    static const char* format_to_string(MetalPixelFormat format);
};

} // namespace bb
