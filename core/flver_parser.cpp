#include "flver_parser.h"
#include "miniz.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace bb {

namespace {

inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t read_i32_le(const uint8_t* p) {
    return static_cast<int32_t>(read_u32_le(p));
}

inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline float read_f32_le(const uint8_t* p) {
    float val;
    std::memcpy(&val, p, sizeof(float));
    return val;
}

std::string read_flver_string(const uint8_t* data, size_t size, uint32_t offset, bool is_unicode) {
    if (offset == 0 || offset >= size) return "";
    std::string result;
    if (is_unicode) {
        for (size_t i = offset; i + 1 < size; i += 2) {
            uint16_t ch = read_u16_le(&data[i]);
            if (ch == 0) break;
            if (ch < 128) result.push_back(static_cast<char>(ch));
            else result.push_back('?');
        }
    } else {
        for (size_t i = offset; i < size; ++i) {
            if (data[i] == 0) break;
            result.push_back(static_cast<char>(data[i]));
        }
    }
    return result;
}

struct LayoutMemberInfo {
    int32_t struct_offset = 0;
    int32_t type = 0;
    int32_t semantic = 0;
    int32_t index = 0;
};

struct BufferLayoutInfo {
    std::vector<LayoutMemberInfo> members;
};

} // namespace

bool FlverParser::is_flver_data(const uint8_t* data, size_t size) {
    if (!data || size < 8) return false;
    return std::memcmp(data, "FLVER\0", 6) == 0;
}

bool FlverParser::is_dcx_data(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    return std::memcmp(data, "DCX\0", 4) == 0;
}

bool FlverParser::decompress_dcx(const uint8_t* dcx_data, size_t dcx_size, std::vector<uint8_t>& out_uncompressed, std::string& out_error) {
    if (!is_dcx_data(dcx_data, dcx_size)) {
        out_error = "Data does not start with DCX magic";
        return false;
    }

    // Locate DCS chunk for uncompressed size
    size_t dcs_pos = 0;
    bool found_dcs = false;
    for (size_t i = 0; i + 4 <= dcx_size; ++i) {
        if (std::memcmp(&dcx_data[i], "DCS\0", 4) == 0) {
            dcs_pos = i;
            found_dcs = true;
            break;
        }
    }

    if (!found_dcs || dcs_pos + 12 > dcx_size) {
        out_error = "Malformed DCX: DCS chunk missing";
        return false;
    }

    uint32_t uncompressed_size = read_u32_be(&dcx_data[dcs_pos + 4]);
    uint32_t compressed_size   = read_u32_be(&dcx_data[dcs_pos + 8]);

    // Locate DCA chunk where compressed stream begins
    size_t dca_pos = 0;
    bool found_dca = false;
    for (size_t i = dcs_pos; i + 8 <= dcx_size; ++i) {
        if (std::memcmp(&dcx_data[i], "DCA\0", 4) == 0) {
            dca_pos = i;
            found_dca = true;
            break;
        }
    }

    if (!found_dca) {
        out_error = "Malformed DCX: DCA chunk missing";
        return false;
    }

    size_t stream_start = dca_pos + 8;
    if (stream_start + compressed_size > dcx_size) {
        compressed_size = static_cast<uint32_t>(dcx_size - stream_start);
    }

    out_uncompressed.resize(uncompressed_size);
    mz_ulong dest_len = uncompressed_size;
    int status = mz_uncompress(out_uncompressed.data(), &dest_len,
                               &dcx_data[stream_start], compressed_size);

    if (status != MZ_OK) {
        out_error = "Failed to decompress DCX stream: mz error " + std::to_string(status);
        out_uncompressed.clear();
        return false;
    }

    out_uncompressed.resize(dest_len);
    return true;
}

bool FlverParser::parse(const uint8_t* data, size_t size, FlverModel& out_model, std::string& out_error) {
    std::vector<uint8_t> decompressed_buffer;
    if (is_dcx_data(data, size)) {
        if (!decompress_dcx(data, size, decompressed_buffer, out_error)) {
            return false;
        }
        data = decompressed_buffer.data();
        size = decompressed_buffer.size();
    }

    if (!is_flver_data(data, size)) {
        out_error = "Invalid FLVER magic header";
        return false;
    }

    if (size < 0x80) {
        out_error = "FLVER data is too small to contain header (< 128 bytes)";
        return false;
    }

    out_model.is_little_endian = (data[6] == 'L');
    out_model.version = read_u32_le(&data[8]);
    uint32_t data_offset = read_u32_le(&data[0x0C]);
    uint32_t dummy_count = read_u32_le(&data[0x14]);
    uint32_t material_count = read_u32_le(&data[0x18]);
    uint32_t bone_count = read_u32_le(&data[0x1C]);
    uint32_t mesh_count = read_u32_le(&data[0x20]);
    uint32_t vertex_buffer_count = read_u32_le(&data[0x24]);

    out_model.bounding_box_min[0] = read_f32_le(&data[0x28]);
    out_model.bounding_box_min[1] = read_f32_le(&data[0x2C]);
    out_model.bounding_box_min[2] = read_f32_le(&data[0x30]);
    out_model.bounding_box_max[0] = read_f32_le(&data[0x34]);
    out_model.bounding_box_max[1] = read_f32_le(&data[0x38]);
    out_model.bounding_box_max[2] = read_f32_le(&data[0x3C]);

    uint8_t global_index_size = data[0x48];
    bool is_unicode = (data[0x49] != 0);
    uint32_t face_set_count = read_u32_le(&data[0x50]);
    uint32_t layout_count = read_u32_le(&data[0x54]);
    uint32_t texture_count = read_u32_le(&data[0x58]);

    size_t cursor = 0x80;

    // 1. Dummies (64 bytes each)
    if (cursor + dummy_count * 64 > size) { out_error = "FLVER truncated in dummy table"; return false; }
    out_model.dummies.resize(dummy_count);
    for (uint32_t i = 0; i < dummy_count; ++i) {
        auto& d = out_model.dummies[i];
        d.position[0] = read_f32_le(&data[cursor + 0]);
        d.position[1] = read_f32_le(&data[cursor + 4]);
        d.position[2] = read_f32_le(&data[cursor + 8]);
        d.forward[0]  = read_f32_le(&data[cursor + 16]);
        d.forward[1]  = read_f32_le(&data[cursor + 20]);
        d.forward[2]  = read_f32_le(&data[cursor + 24]);
        d.up[0]       = read_f32_le(&data[cursor + 32]);
        d.up[1]       = read_f32_le(&data[cursor + 36]);
        d.up[2]       = read_f32_le(&data[cursor + 40]);
        d.reference_id = static_cast<int16_t>(read_u16_le(&data[cursor + 28]));
        d.parent_bone_index = static_cast<int16_t>(read_u16_le(&data[cursor + 30]));
        d.attach_bone_index = static_cast<int16_t>(read_u16_le(&data[cursor + 44]));
        cursor += 64;
    }

    // 2. Materials (32 bytes each)
    if (cursor + material_count * 32 > size) { out_error = "FLVER truncated in material table"; return false; }
    out_model.materials.resize(material_count);
    for (uint32_t i = 0; i < material_count; ++i) {
        auto& m = out_model.materials[i];
        uint32_t name_off = read_u32_le(&data[cursor + 0]);
        uint32_t mtd_off  = read_u32_le(&data[cursor + 4]);
        m.texture_count   = read_u32_le(&data[cursor + 8]);
        m.texture_index   = read_u32_le(&data[cursor + 12]);
        m.flags           = read_u32_le(&data[cursor + 16]);
        m.name = read_flver_string(data, size, name_off, is_unicode);
        m.mtd_path = read_flver_string(data, size, mtd_off, is_unicode);
        cursor += 32;
    }

    // 3. Bones (128 bytes each in FLVER2)
    if (cursor + bone_count * 128 > size) { out_error = "FLVER truncated in bone table"; return false; }
    out_model.bones.resize(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i) {
        auto& b = out_model.bones[i];
        b.translation[0] = read_f32_le(&data[cursor + 0]);
        b.translation[1] = read_f32_le(&data[cursor + 4]);
        b.translation[2] = read_f32_le(&data[cursor + 8]);
        uint32_t name_off = read_u32_le(&data[cursor + 12]);
        b.rotation[0]    = read_f32_le(&data[cursor + 16]);
        b.rotation[1]    = read_f32_le(&data[cursor + 20]);
        b.rotation[2]    = read_f32_le(&data[cursor + 24]);
        b.parent_index   = static_cast<int16_t>(read_u16_le(&data[cursor + 28]));
        b.child_index    = static_cast<int16_t>(read_u16_le(&data[cursor + 30]));
        b.scale[0]       = read_f32_le(&data[cursor + 32]);
        b.scale[1]       = read_f32_le(&data[cursor + 36]);
        b.scale[2]       = read_f32_le(&data[cursor + 40]);
        b.next_sibling_index = static_cast<int16_t>(read_u16_le(&data[cursor + 44]));
        b.prev_sibling_index = static_cast<int16_t>(read_u16_le(&data[cursor + 46]));
        b.bounding_box_min[0] = read_f32_le(&data[cursor + 48]);
        b.bounding_box_min[1] = read_f32_le(&data[cursor + 52]);
        b.bounding_box_min[2] = read_f32_le(&data[cursor + 56]);
        b.bounding_box_max[0] = read_f32_le(&data[cursor + 64]);
        b.bounding_box_max[1] = read_f32_le(&data[cursor + 68]);
        b.bounding_box_max[2] = read_f32_le(&data[cursor + 72]);
        b.name = read_flver_string(data, size, name_off, is_unicode);
        cursor += 128;
    }

    // 4. Meshes (48 bytes each)
    struct MeshHeaderInfo {
        uint32_t material_index = 0;
        int32_t face_set_count = 0;
        int32_t face_set_indices_offset = 0;
        int32_t vertex_buffer_count = 0;
        int32_t vertex_buffer_indices_offset = 0;
    };
    if (cursor + mesh_count * 48 > size) { out_error = "FLVER truncated in mesh table"; return false; }
    std::vector<MeshHeaderInfo> mesh_headers(mesh_count);
    for (uint32_t i = 0; i < mesh_count; ++i) {
        mesh_headers[i].material_index = read_u32_le(&data[cursor + 4]);
        mesh_headers[i].face_set_count = read_i32_le(&data[cursor + 32]);
        mesh_headers[i].face_set_indices_offset = read_i32_le(&data[cursor + 36]);
        mesh_headers[i].vertex_buffer_count = read_i32_le(&data[cursor + 40]);
        mesh_headers[i].vertex_buffer_indices_offset = read_i32_le(&data[cursor + 44]);
        cursor += 48;
    }

    // 5. FaceSets (32 bytes each)
    struct FaceSetInfo {
        uint32_t flags = 0;
        uint8_t triangle_strip = 0;
        uint8_t cull_back_faces = 0;
        int32_t index_count = 0;
        int32_t indices_offset = 0;
        int32_t indices_size = 0;
        int32_t index_size = 0;
    };
    if (cursor + face_set_count * 32 > size) { out_error = "FLVER truncated in face set table"; return false; }
    std::vector<FaceSetInfo> face_sets(face_set_count);
    for (uint32_t i = 0; i < face_set_count; ++i) {
        face_sets[i].flags = read_u32_le(&data[cursor + 0]);
        face_sets[i].triangle_strip = data[cursor + 4];
        face_sets[i].cull_back_faces = data[cursor + 5];
        face_sets[i].index_count = read_i32_le(&data[cursor + 8]);
        face_sets[i].indices_offset = read_i32_le(&data[cursor + 12]);
        face_sets[i].indices_size = read_i32_le(&data[cursor + 16]);
        face_sets[i].index_size = read_i32_le(&data[cursor + 24]);
        cursor += 32;
    }

    // 6. VertexBuffers (32 bytes each)
    struct VertexBufferInfo {
        int32_t layout_index = 0;
        int32_t buffer_index = 0;
        int32_t vertex_size = 0;
        int32_t vertex_count = 0;
        int32_t buffer_length = 0;
        int32_t buffer_offset = 0;
    };
    if (cursor + vertex_buffer_count * 32 > size) { out_error = "FLVER truncated in vertex buffer table"; return false; }
    std::vector<VertexBufferInfo> vertex_buffers(vertex_buffer_count);
    for (uint32_t i = 0; i < vertex_buffer_count; ++i) {
        vertex_buffers[i].layout_index = read_i32_le(&data[cursor + 0]);
        vertex_buffers[i].buffer_index = read_i32_le(&data[cursor + 4]);
        vertex_buffers[i].vertex_size = read_i32_le(&data[cursor + 8]);
        vertex_buffers[i].vertex_count = read_i32_le(&data[cursor + 12]);
        vertex_buffers[i].buffer_length = read_i32_le(&data[cursor + 24]);
        vertex_buffers[i].buffer_offset = read_i32_le(&data[cursor + 28]);
        cursor += 32;
    }

    // 7. BufferLayouts (16 bytes each)
    if (cursor + layout_count * 16 > size) { out_error = "FLVER truncated in layout table"; return false; }
    std::vector<BufferLayoutInfo> layouts(layout_count);
    for (uint32_t i = 0; i < layout_count; ++i) {
        int32_t member_count = read_i32_le(&data[cursor + 0]);
        int32_t members_offset = read_i32_le(&data[cursor + 12]);
        cursor += 16;

        if (members_offset > 0 && static_cast<size_t>(members_offset + member_count * 20) <= size) {
            layouts[i].members.resize(member_count);
            for (int32_t m = 0; m < member_count; ++m) {
                size_t mpos = static_cast<size_t>(members_offset + m * 20);
                layouts[i].members[m].struct_offset = read_i32_le(&data[mpos + 4]);
                layouts[i].members[m].type          = read_i32_le(&data[mpos + 8]);
                layouts[i].members[m].semantic      = read_i32_le(&data[mpos + 12]);
                layouts[i].members[m].index         = read_i32_le(&data[mpos + 16]);
            }
        }
    }

    // 8. Textures (32 bytes each)
    if (cursor + texture_count * 32 > size) { out_error = "FLVER truncated in texture table"; return false; }
    out_model.textures.resize(texture_count);
    for (uint32_t i = 0; i < texture_count; ++i) {
        auto& t = out_model.textures[i];
        uint32_t path_off = read_u32_le(&data[cursor + 0]);
        uint32_t type_off = read_u32_le(&data[cursor + 4]);
        t.scale[0] = read_f32_le(&data[cursor + 8]);
        t.scale[1] = read_f32_le(&data[cursor + 12]);
        t.path = read_flver_string(data, size, path_off, is_unicode);
        t.type = read_flver_string(data, size, type_off, is_unicode);
        cursor += 32;
    }

    // Assemble Submeshes
    out_model.submeshes.resize(mesh_count);
    for (uint32_t mi = 0; mi < mesh_count; ++mi) {
        auto& submesh = out_model.submeshes[mi];
        const auto& mh = mesh_headers[mi];
        submesh.material_index = mh.material_index;

        // Decode Vertices from vertex buffer
        uint32_t vb_index = mi; // default 1:1 mapping
        if (mh.vertex_buffer_indices_offset > 0 && static_cast<size_t>(mh.vertex_buffer_indices_offset + 4) <= size) {
            vb_index = read_u32_le(&data[mh.vertex_buffer_indices_offset]);
        }

        if (vb_index < vertex_buffers.size()) {
            const auto& vb = vertex_buffers[vb_index];
            const size_t vb_data_pos = static_cast<size_t>(data_offset + vb.buffer_offset);
            const auto& layout = (vb.layout_index >= 0 && static_cast<size_t>(vb.layout_index) < layouts.size())
                                     ? layouts[vb.layout_index]
                                     : layouts[0];

            if (vb_data_pos + vb.buffer_length <= size && vb.vertex_size > 0) {
                submesh.vertices.resize(vb.vertex_count);
                for (int32_t v = 0; v < vb.vertex_count; ++v) {
                    const uint8_t* vbytes = &data[vb_data_pos + v * vb.vertex_size];
                    auto& vert = submesh.vertices[v];

                    for (const auto& mem : layout.members) {
                        const uint8_t* mptr = vbytes + mem.struct_offset;
                        if (mem.semantic == 0) { // Position
                            if (mem.type == 2) { // Float3
                                vert.position[0] = read_f32_le(mptr);
                                vert.position[1] = read_f32_le(mptr + 4);
                                vert.position[2] = read_f32_le(mptr + 8);
                            }
                        } else if (mem.semantic == 3) { // Normal
                            if (mem.type == 2) { // Float3
                                vert.normal[0] = read_f32_le(mptr);
                                vert.normal[1] = read_f32_le(mptr + 4);
                                vert.normal[2] = read_f32_le(mptr + 8);
                            } else if (mem.type == 17 || mem.type == 22) { // Byte4B
                                vert.normal[0] = (static_cast<float>(mptr[0]) - 127.0f) / 127.0f;
                                vert.normal[1] = (static_cast<float>(mptr[1]) - 127.0f) / 127.0f;
                                vert.normal[2] = (static_cast<float>(mptr[2]) - 127.0f) / 127.0f;
                            }
                        } else if (mem.semantic == 6) { // Tangent
                            if (mem.type == 17 || mem.type == 22) {
                                vert.tangent[0] = (static_cast<float>(mptr[0]) - 127.0f) / 127.0f;
                                vert.tangent[1] = (static_cast<float>(mptr[1]) - 127.0f) / 127.0f;
                                vert.tangent[2] = (static_cast<float>(mptr[2]) - 127.0f) / 127.0f;
                                vert.tangent[3] = (static_cast<float>(mptr[3]) - 127.0f) / 127.0f;
                            }
                        } else if (mem.semantic == 5 || mem.semantic == 10) { // UV
                            if (mem.type == 1) { // Float2
                                vert.uv[0] = read_f32_le(mptr);
                                vert.uv[1] = read_f32_le(mptr + 4);
                            } else if (mem.type == 19 || mem.type == 21) { // Short2ToFloat2 / Short4
                                int16_t u_raw = static_cast<int16_t>(read_u16_le(mptr));
                                int16_t v_raw = static_cast<int16_t>(read_u16_le(mptr + 2));
                                vert.uv[0] = static_cast<float>(u_raw) / 1024.0f;
                                vert.uv[1] = static_cast<float>(v_raw) / 1024.0f;
                            }
                        }
                    }
                }
            }
        }

        // Decode FaceSet indices
        uint32_t fs_index = mi;
        if (mh.face_set_indices_offset > 0 && static_cast<size_t>(mh.face_set_indices_offset + 4) <= size) {
            fs_index = read_u32_le(&data[mh.face_set_indices_offset]);
        }

        if (fs_index < face_sets.size()) {
            const auto& fs = face_sets[fs_index];
            const size_t fs_data_pos = static_cast<size_t>(data_offset + fs.indices_offset);
            int idx_bits = fs.index_size > 0 ? fs.index_size : (global_index_size > 0 ? global_index_size : 16);

            if (fs_data_pos + fs.indices_size <= size && fs.index_count > 0) {
                std::vector<uint32_t> raw_indices(fs.index_count);
                if (idx_bits == 32) {
                    for (int32_t k = 0; k < fs.index_count; ++k) {
                        raw_indices[k] = read_u32_le(&data[fs_data_pos + k * 4]);
                    }
                } else {
                    for (int32_t k = 0; k < fs.index_count; ++k) {
                        raw_indices[k] = read_u16_le(&data[fs_data_pos + k * 2]);
                    }
                }

                // Unwind indices into standard triangle list
                submesh.indices.reserve(fs.index_count);
                if (fs.triangle_strip != 0) {
                    for (size_t k = 0; k + 2 < raw_indices.size(); ++k) {
                        uint32_t i0 = raw_indices[k];
                        uint32_t i1 = raw_indices[k + 1];
                        uint32_t i2 = raw_indices[k + 2];
                        if (i0 == 0xFFFF || i1 == 0xFFFF || i2 == 0xFFFF || i0 == 0xFFFFFFFF || i1 == 0xFFFFFFFF || i2 == 0xFFFFFFFF) continue;
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue; // degenerate
                        if ((k % 2) == 1) std::swap(i1, i2);
                        submesh.indices.push_back(i0);
                        submesh.indices.push_back(i1);
                        submesh.indices.push_back(i2);
                    }
                } else {
                    for (size_t k = 0; k + 2 < raw_indices.size(); k += 3) {
                        submesh.indices.push_back(raw_indices[k]);
                        submesh.indices.push_back(raw_indices[k + 1]);
                        submesh.indices.push_back(raw_indices[k + 2]);
                    }
                }
            }
        }
    }

    return true;
}

bool FlverParser::parse_file(const std::string& file_path, FlverModel& out_model, std::string& out_error) {
    std::filesystem::path fpath = std::filesystem::u8path(file_path);
    std::ifstream file(fpath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        out_error = "Could not open file: " + file_path;
        return false;
    }
    const auto file_size = file.tellg();
    if (file_size <= 0) {
        out_error = "Empty file: " + file_path;
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        out_error = "Failed to read file: " + file_path;
        return false;
    }
    return parse(buffer.data(), buffer.size(), out_model, out_error);
}

MetalMeshBuffer FlverParser::convert_to_metal_buffers(const FlverModel& model) {
    MetalMeshBuffer mb{};
    const uint32_t total_verts = model.total_vertex_count();
    const uint32_t total_indices = model.total_triangle_count() * 3;

    mb.vertex_data.reserve(total_verts * 12);
    mb.index_data.reserve(total_indices);

    float min_pt[3] = {1e9f, 1e9f, 1e9f};
    float max_pt[3] = {-1e9f, -1e9f, -1e9f};

    uint32_t base_vertex = 0;
    for (const auto& sm : model.submeshes) {
        for (const auto& v : sm.vertices) {
            // Position (3 floats)
            mb.vertex_data.push_back(v.position[0]);
            mb.vertex_data.push_back(v.position[1]);
            mb.vertex_data.push_back(v.position[2]);

            // Normal (3 floats)
            mb.vertex_data.push_back(v.normal[0]);
            mb.vertex_data.push_back(v.normal[1]);
            mb.vertex_data.push_back(v.normal[2]);

            // UV (2 floats)
            mb.vertex_data.push_back(v.uv[0]);
            mb.vertex_data.push_back(v.uv[1]);

            // Tangent (4 floats)
            mb.vertex_data.push_back(v.tangent[0]);
            mb.vertex_data.push_back(v.tangent[1]);
            mb.vertex_data.push_back(v.tangent[2]);
            mb.vertex_data.push_back(v.tangent[3]);

            for (int k = 0; k < 3; ++k) {
                if (v.position[k] < min_pt[k]) min_pt[k] = v.position[k];
                if (v.position[k] > max_pt[k]) max_pt[k] = v.position[k];
            }
        }

        for (uint32_t idx : sm.indices) {
            mb.index_data.push_back(base_vertex + idx);
        }

        base_vertex += static_cast<uint32_t>(sm.vertices.size());
    }

    mb.vertex_count = base_vertex;
    mb.index_count = static_cast<uint32_t>(mb.index_data.size());

    // Calculate Bounding Sphere
    if (mb.vertex_count > 0) {
        float cx = (min_pt[0] + max_pt[0]) * 0.5f;
        float cy = (min_pt[1] + max_pt[1]) * 0.5f;
        float cz = (min_pt[2] + max_pt[2]) * 0.5f;

        float max_r2 = 0.0f;
        for (uint32_t i = 0; i < mb.vertex_count; ++i) {
            float px = mb.vertex_data[i * 12 + 0] - cx;
            float py = mb.vertex_data[i * 12 + 1] - cy;
            float pz = mb.vertex_data[i * 12 + 2] - cz;
            float r2 = px * px + py * py + pz * pz;
            if (r2 > max_r2) max_r2 = r2;
        }

        mb.bounding_sphere[0] = cx;
        mb.bounding_sphere[1] = cy;
        mb.bounding_sphere[2] = cz;
        mb.bounding_sphere[3] = std::sqrt(max_r2);
    }

    return mb;
}

// -----------------------------------------------------------------------------
// Synthetic FLVER2 builders for testing
// -----------------------------------------------------------------------------

std::vector<uint8_t> FlverParser::create_synthetic_flver_cube() {
    std::vector<uint8_t> data(1024, 0);

    // Header
    std::memcpy(&data[0], "FLVER\0", 6);
    data[6] = 'L'; data[7] = '\0';
    uint32_t ver = 0x20014;
    std::memcpy(&data[8], &ver, 4);

    uint32_t data_off = 512;
    uint32_t mat_cnt = 1;
    uint32_t bone_cnt = 1;
    uint32_t mesh_cnt = 1;
    uint32_t vb_cnt = 1;
    uint32_t fs_cnt = 1;
    uint32_t lo_cnt = 1;

    std::memcpy(&data[0x0C], &data_off, 4);
    std::memcpy(&data[0x18], &mat_cnt, 4);
    std::memcpy(&data[0x1C], &bone_cnt, 4);
    std::memcpy(&data[0x20], &mesh_cnt, 4);
    std::memcpy(&data[0x24], &vb_cnt, 4);
    std::memcpy(&data[0x50], &fs_cnt, 4);
    std::memcpy(&data[0x54], &lo_cnt, 4);

    // Material at 0x80 (32 bytes)
    // Bone at 0xA0 (128 bytes) -> ends at 0x120
    // Mesh at 0x120 (48 bytes)
    int32_t mesh_fs_cnt = 1;
    int32_t mesh_vb_cnt = 1;
    std::memcpy(&data[0x120 + 32], &mesh_fs_cnt, 4);
    std::memcpy(&data[0x120 + 40], &mesh_vb_cnt, 4);

    // FaceSet at 0x150 (32 bytes)
    int32_t fs_idx_cnt = 36;
    int32_t fs_idx_off = 0;
    int32_t fs_idx_sz = 36 * 2;
    int32_t fs_idx_bits = 16;
    std::memcpy(&data[0x150 + 8], &fs_idx_cnt, 4);
    std::memcpy(&data[0x150 + 12], &fs_idx_off, 4);
    std::memcpy(&data[0x150 + 16], &fs_idx_sz, 4);
    std::memcpy(&data[0x150 + 24], &fs_idx_bits, 4);

    // VertexBuffer at 0x170 (32 bytes)
    int32_t vb_vsz = 28;
    int32_t vb_vcnt = 8;
    int32_t vb_len = 8 * 28;
    int32_t vb_off = 72; // after 36*2 indices
    std::memcpy(&data[0x170 + 8], &vb_vsz, 4);
    std::memcpy(&data[0x170 + 12], &vb_vcnt, 4);
    std::memcpy(&data[0x170 + 24], &vb_len, 4);
    std::memcpy(&data[0x170 + 28], &vb_off, 4);

    // Layout at 0x190 (16 bytes)
    int32_t lo_mcnt = 3;
    int32_t lo_moff = 0x1A0;
    std::memcpy(&data[0x190 + 0], &lo_mcnt, 4);
    std::memcpy(&data[0x190 + 12], &lo_moff, 4);

    // Layout members at 0x1A0: Position (Float3=2, off=0), Normal (Float3=2, off=12), UV (Float2=1, off=24)
    int32_t m0[5] = {0, 0, 2, 0, 0};
    int32_t m1[5] = {0, 12, 2, 3, 0};
    int32_t m2[5] = {0, 24, 1, 5, 0};
    std::memcpy(&data[0x1A0], m0, 20);
    std::memcpy(&data[0x1A0 + 20], m1, 20);
    std::memcpy(&data[0x1A0 + 40], m2, 20);

    // Indices at data_off (512)
    uint16_t cube_indices[36] = {
        0, 1, 2, 2, 3, 0,
        4, 5, 6, 6, 7, 4,
        0, 4, 7, 7, 3, 0,
        1, 5, 6, 6, 2, 1,
        3, 2, 6, 6, 7, 3,
        0, 1, 5, 5, 4, 0
    };
    std::memcpy(&data[data_off], cube_indices, sizeof(cube_indices));

    // Vertices at data_off + 72 (584)
    float cube_verts[8][7] = {
        {-0.5f, -0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 0.0f},
        { 0.5f, -0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 1.0f},
        {-0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f,  0.0f, 0.0f, -1.0f, 0.0f},
        { 0.5f, -0.5f, -0.5f,  0.0f, 0.0f, -1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f,  0.0f, 0.0f, -1.0f, 1.0f},
        {-0.5f,  0.5f, -0.5f,  0.0f, 0.0f, -1.0f, 0.0f}
    };
    for (int i = 0; i < 8; ++i) {
        size_t vpos = data_off + 72 + i * 28;
        std::memcpy(&data[vpos], cube_verts[i], 28);
    }

    return data;
}

std::vector<uint8_t> FlverParser::create_synthetic_flver_saw_cleaver() {
    return create_synthetic_flver_cube();
}

} // namespace bb
