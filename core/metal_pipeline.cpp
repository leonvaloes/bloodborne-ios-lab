#include "metal_pipeline.h"
#include <cmath>
#include <cstring>
#include <utility>

namespace bb {

const char* mesh_preparation_status_string(MeshPreparationStatus status) {
    switch (status) {
    case MeshPreparationStatus::Ready: return "ready";
    case MeshPreparationStatus::Empty: return "empty";
    case MeshPreparationStatus::TooLarge: return "too_large";
    case MeshPreparationStatus::InvalidIndex: return "invalid_index";
    case MeshPreparationStatus::NonFinite: return "non_finite";
    case MeshPreparationStatus::InvalidBuffer: return "invalid_buffer";
    }
    return "unknown";
}

MeshPreparationStatus prepare_metal_mesh(const FlverModel& model, MetalMeshBuffer& out,
                                         size_t max_buffer_bytes) {
    out = {};
    // Validate before convert_to_metal_buffers: its counts and base-vertex offsets are uint32_t.
    size_t vertices = 0, indices = 0;
    for (const auto& submesh : model.submeshes) {
        const size_t nv = submesh.vertices.size(), ni = submesh.indices.size();
        const size_t vertex_limit = max_buffer_bytes / (12 * sizeof(float));
        const size_t index_limit = max_buffer_bytes / sizeof(uint32_t);
        if (vertices > vertex_limit || indices > index_limit ||
            nv > static_cast<size_t>(UINT32_MAX) - vertices ||
            ni > static_cast<size_t>(UINT32_MAX) - indices ||
            nv > vertex_limit - vertices || ni > index_limit - indices)
            return MeshPreparationStatus::TooLarge;
        for (uint32_t index : submesh.indices)
            if (index >= nv) return MeshPreparationStatus::InvalidIndex;
        for (const auto& vertex : submesh.vertices) {
            for (float value : vertex.position) if (!std::isfinite(value)) return MeshPreparationStatus::NonFinite;
            for (float value : vertex.normal) if (!std::isfinite(value)) return MeshPreparationStatus::NonFinite;
            for (float value : vertex.uv) if (!std::isfinite(value)) return MeshPreparationStatus::NonFinite;
            for (float value : vertex.tangent) if (!std::isfinite(value)) return MeshPreparationStatus::NonFinite;
        }
        vertices += nv;
        indices += ni;
    }
    if (!vertices || !indices || indices % 3) return MeshPreparationStatus::Empty;
    auto candidate = FlverParser::convert_to_metal_buffers(model);
    if (candidate.vertex_count != vertices || candidate.index_count != indices ||
        candidate.vertex_stride_bytes != 12 * sizeof(float) ||
        candidate.vertex_data.size() != vertices * 12 || candidate.index_data.size() != indices)
        return MeshPreparationStatus::InvalidBuffer;
    for (uint32_t index : candidate.index_data)
        if (index >= vertices) return MeshPreparationStatus::InvalidBuffer;
    for (float value : candidate.bounding_sphere)
        if (!std::isfinite(value)) return MeshPreparationStatus::NonFinite;
    out = std::move(candidate);
    return MeshPreparationStatus::Ready;
}

bool RenderPipelineConfig::validate(std::string& out_error) const {
    if (color_format == MetalPixelFormat::Invalid) {
        out_error = "Invalid color attachment format";
        return false;
    }
    if (depth_format != MetalPixelFormat::Depth32Float && depth_format != MetalPixelFormat::Depth32Float_Stencil8) {
        out_error = "3D occlusion pipeline requires a 32-bit float depth format";
        return false;
    }
    if (sample_count != 1 && sample_count != 4) {
        out_error = "Unsupported MSAA sample count (only 1 or 4 supported)";
        return false;
    }
    return true;
}

Matrix4x4 Matrix4x4::identity() {
    Matrix4x4 res{};
    res.m[0] = 1.0f; res.m[5] = 1.0f; res.m[10] = 1.0f; res.m[15] = 1.0f;
    return res;
}

// Column-major perspective projection (Metal clip space: Z in [0, 1])
Matrix4x4 Matrix4x4::perspective(float fov_radians, float aspect, float z_near, float z_far) {
    Matrix4x4 res{};
    const float tan_half_fov = std::tan(fov_radians * 0.5f);
    res.m[0] = 1.0f / (aspect * tan_half_fov);
    res.m[5] = 1.0f / tan_half_fov;
    res.m[10] = z_far / (z_far - z_near);
    res.m[11] = 1.0f;
    res.m[14] = -(z_far * z_near) / (z_far - z_near);
    res.m[15] = 0.0f;
    return res;
}

Matrix4x4 Matrix4x4::look_at(const float* eye, const float* target, const float* up) {
    Matrix4x4 res{};

    // forward (z axis)
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    float f_len = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (f_len > 1e-6f) { f[0] /= f_len; f[1] /= f_len; f[2] /= f_len; }

    // side (x axis) = normalize(cross(up, forward))
    float s[3] = {
        up[1] * f[2] - up[2] * f[1],
        up[2] * f[0] - up[0] * f[2],
        up[0] * f[1] - up[1] * f[0]
    };
    float s_len = std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    if (s_len > 1e-6f) { s[0] /= s_len; s[1] /= s_len; s[2] /= s_len; }

    // true up (y axis) = cross(forward, side)
    float u[3] = {
        f[1] * s[2] - f[2] * s[1],
        f[2] * s[0] - f[0] * s[2],
        f[0] * s[1] - f[1] * s[0]
    };

    // Column-major view matrix
    res.m[0] = s[0];  res.m[4] = s[1];  res.m[8]  = s[2];  res.m[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    res.m[1] = u[0];  res.m[5] = u[1];  res.m[9]  = u[2];  res.m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    res.m[2] = f[0];  res.m[6] = f[1];  res.m[10] = f[2];  res.m[14] = -(f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
    res.m[3] = 0.0f;  res.m[7] = 0.0f;  res.m[11] = 0.0f;  res.m[15] = 1.0f;

    return res;
}

Matrix4x4 Matrix4x4::rotation_y(float angle_radians) {
    Matrix4x4 res = Matrix4x4::identity();
    float c = std::cos(angle_radians);
    float s = std::sin(angle_radians);
    res.m[0] = c;
    res.m[2] = -s;
    res.m[8] = s;
    res.m[10] = c;
    return res;
}

Matrix4x4 Matrix4x4::rotation_x(float angle_radians) {
    Matrix4x4 res = Matrix4x4::identity();
    float c = std::cos(angle_radians);
    float s = std::sin(angle_radians);
    res.m[5] = c;
    res.m[6] = s;
    res.m[9] = -s;
    res.m[10] = c;
    return res;
}

Matrix4x4 Matrix4x4::multiply(const Matrix4x4& a, const Matrix4x4& b) {
    Matrix4x4 out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out.m[c * 4 + r] =
                a.m[0 * 4 + r] * b.m[c * 4 + 0] +
                a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                a.m[2 * 4 + r] * b.m[c * 4 + 2] +
                a.m[3 * 4 + r] * b.m[c * 4 + 3];
        }
    }
    return out;
}

Matrix4x4 Matrix4x4::normal_matrix(const Matrix4x4& model_view) {
    // For standard rotation/uniform scale, the upper 3x3 of model_view is already orthogonal
    Matrix4x4 norm = Matrix4x4::identity();
    norm.m[0] = model_view.m[0]; norm.m[1] = model_view.m[1]; norm.m[2] = model_view.m[2];
    norm.m[4] = model_view.m[4]; norm.m[5] = model_view.m[5]; norm.m[6] = model_view.m[6];
    norm.m[8] = model_view.m[8]; norm.m[9] = model_view.m[9]; norm.m[10] = model_view.m[10];
    return norm;
}

void Metal3ArgumentTable::bind_texture(uint32_t slot, uint64_t gpu_address) {
    if (slot < MaxTextures) {
        texture_gpu_addresses[slot] = gpu_address;
    }
}

void Metal3ArgumentTable::bind_sampler(uint32_t slot, uint64_t sampler_handle) {
    if (slot < MaxSamplers) {
        sampler_descriptors[slot] = sampler_handle;
    }
}

void Metal3ArgumentTable::bind_buffer(uint32_t slot, uint64_t gpu_address) {
    if (slot < MaxBuffers) {
        uniform_buffer_addresses[slot] = gpu_address;
    }
}

bool Metal3ArgumentTable::validate() const {
    // Pointers on Apple A17 Pro 64-bit architecture must be aligned to their natural size
    for (size_t i = 0; i < MaxBuffers; ++i) {
        if (uniform_buffer_addresses[i] != 0 && (uniform_buffer_addresses[i] & 0x7) != 0) {
            return false;
        }
    }
    return true;
}

bool MetalPipelineManager::is_hdr_format(MetalPixelFormat format) {
    return format == MetalPixelFormat::RGBA16Float || format == MetalPixelFormat::RG11B10Float;
}

bool MetalPipelineManager::is_depth_format(MetalPixelFormat format) {
    return format == MetalPixelFormat::Depth32Float || format == MetalPixelFormat::Depth32Float_Stencil8;
}

const char* MetalPipelineManager::format_to_string(MetalPixelFormat format) {
    switch (format) {
        case MetalPixelFormat::BGRA8Unorm: return "MTLPixelFormatBGRA8Unorm";
        case MetalPixelFormat::RGBA16Float: return "MTLPixelFormatRGBA16Float (HDR)";
        case MetalPixelFormat::RG11B10Float: return "MTLPixelFormatRG11B10Float (Compact HDR)";
        case MetalPixelFormat::Depth32Float: return "MTLPixelFormatDepth32Float";
        case MetalPixelFormat::Depth32Float_Stencil8: return "MTLPixelFormatDepth32Float_Stencil8";
        default: return "Invalid";
    }
}

} // namespace bb
