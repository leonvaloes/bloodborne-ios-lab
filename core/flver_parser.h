#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bb {

struct FlverVertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3]   = {0.0f, 1.0f, 0.0f};
    float tangent[4]  = {1.0f, 0.0f, 0.0f, 1.0f};
    float uv[2]       = {0.0f, 0.0f};
    float bone_weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    uint16_t bone_indices[4] = {0, 0, 0, 0};
};

struct FlverMaterial {
    std::string name;
    std::string mtd_path;
    uint32_t texture_index = 0;
    uint32_t texture_count = 0;
    uint32_t flags = 0;
};

struct FlverTexture {
    std::string path;
    std::string type; // e.g. "g_DiffuseTexture", "g_SpecularTexture", "g_BumpmapTexture"
    float scale[2] = {1.0f, 1.0f};
};

struct FlverBone {
    std::string name;
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3]    = {0.0f, 0.0f, 0.0f}; // Euler radians
    float scale[3]       = {1.0f, 1.0f, 1.0f};
    int16_t parent_index = -1;
    int16_t child_index = -1;
    int16_t next_sibling_index = -1;
    int16_t prev_sibling_index = -1;
    float bounding_box_min[3] = {0.0f, 0.0f, 0.0f};
    float bounding_box_max[3] = {0.0f, 0.0f, 0.0f};
};

struct FlverDummy {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float forward[3]  = {0.0f, 0.0f, 1.0f};
    float up[3]       = {0.0f, 1.0f, 0.0f};
    int16_t reference_id = 0;
    int16_t parent_bone_index = -1;
    int16_t attach_bone_index = -1;
};

struct FlverSubmesh {
    uint32_t material_index = 0;
    std::vector<FlverVertex> vertices;
    std::vector<uint32_t> indices; // Unwound triangle index list
    float bounding_box_min[3] = {0.0f, 0.0f, 0.0f};
    float bounding_box_max[3] = {0.0f, 0.0f, 0.0f};
};

struct FlverModel {
    uint32_t version = 0;
    bool is_little_endian = true;
    float bounding_box_min[3] = {0.0f, 0.0f, 0.0f};
    float bounding_box_max[3] = {0.0f, 0.0f, 0.0f};
    std::vector<FlverMaterial> materials;
    std::vector<FlverTexture> textures;
    std::vector<FlverBone> bones;
    std::vector<FlverDummy> dummies;
    std::vector<FlverSubmesh> submeshes;

    uint32_t total_vertex_count() const {
        uint32_t count = 0;
        for (const auto& sm : submeshes) count += static_cast<uint32_t>(sm.vertices.size());
        return count;
    }

    uint32_t total_triangle_count() const {
        uint32_t count = 0;
        for (const auto& sm : submeshes) count += static_cast<uint32_t>(sm.indices.size() / 3);
        return count;
    }
};

// Ready for Apple Metal Unified Memory (MTLResourceStorageModeShared on A17 Pro)
struct MetalMeshBuffer {
    std::vector<float> vertex_data; // 12 floats per vertex: px,py,pz, nx,ny,nz, u,v, tx,ty,tz,tw
    std::vector<uint32_t> index_data;
    uint32_t vertex_stride_bytes = 12 * sizeof(float); // 48 bytes
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    float bounding_sphere[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // cx, cy, cz, radius
};

class FlverParser {
public:
    static bool is_flver_data(const uint8_t* data, size_t size);
    static bool is_dcx_data(const uint8_t* data, size_t size);

    // Decompress .dcx container (FromSoftware DCX DFLT/Deflate)
    static bool decompress_dcx(const uint8_t* dcx_data, size_t dcx_size, std::vector<uint8_t>& out_uncompressed, std::string& out_error);

    // Parse raw .flver or compressed .flver.dcx data
    static bool parse(const uint8_t* data, size_t size, FlverModel& out_model, std::string& out_error);

    // Parse model from disk
    static bool parse_file(const std::string& file_path, FlverModel& out_model, std::string& out_error);

    // Convert parsed model into Metal 3 GPU vertex & index buffer format
    static MetalMeshBuffer convert_to_metal_buffers(const FlverModel& model);

    // Synthetic models for unit tests and offline rendering verification
    static std::vector<uint8_t> create_synthetic_flver_cube();
    static std::vector<uint8_t> create_synthetic_flver_saw_cleaver();
};

} // namespace bb
