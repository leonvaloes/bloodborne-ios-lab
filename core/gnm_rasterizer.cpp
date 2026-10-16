#include "gnm_rasterizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bb {

namespace {

struct Vertex2D {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

inline float edge_function(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

inline float clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

} // namespace

RasterizerStatus GnmRasterizer::clear_render_target(
    GuestMemory& memory,
    const RenderTarget& target,
    float r, float g, float b, float a
) {
    if (target.base_address == 0 || target.width == 0 || target.height == 0) {
        return RasterizerStatus::InvalidRenderTarget;
    }
    std::uint32_t bpp = 4;
    if (target.format != GnmSurfaceFormat::B8G8R8A8_Unorm &&
        target.format != GnmSurfaceFormat::R8G8B8A8_Unorm) {
        return RasterizerStatus::UnsupportedFormat;
    }
    const std::uint32_t pitch = target.pitch > 0 ? target.pitch : target.width;
    const std::size_t total_bytes = static_cast<std::size_t>(pitch) * target.height * bpp;
    if (target.base_address > UINT64_MAX - total_bytes) {
        return RasterizerStatus::BufferOverflow;
    }

    std::uint8_t u_r = static_cast<std::uint8_t>(clamp01(r) * 255.0f + 0.5f);
    std::uint8_t u_g = static_cast<std::uint8_t>(clamp01(g) * 255.0f + 0.5f);
    std::uint8_t u_b = static_cast<std::uint8_t>(clamp01(b) * 255.0f + 0.5f);
    std::uint8_t u_a = static_cast<std::uint8_t>(clamp01(a) * 255.0f + 0.5f);

    std::uint32_t pixel_word = 0;
    if (target.format == GnmSurfaceFormat::B8G8R8A8_Unorm) {
        pixel_word = static_cast<std::uint32_t>(u_b) |
                     (static_cast<std::uint32_t>(u_g) << 8) |
                     (static_cast<std::uint32_t>(u_r) << 16) |
                     (static_cast<std::uint32_t>(u_a) << 24);
    } else {
        pixel_word = static_cast<std::uint32_t>(u_r) |
                     (static_cast<std::uint32_t>(u_g) << 8) |
                     (static_cast<std::uint32_t>(u_b) << 16) |
                     (static_cast<std::uint32_t>(u_a) << 24);
    }

    std::vector<std::uint32_t> row_buffer(pitch, pixel_word);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        std::uint64_t row_addr = target.base_address + static_cast<std::uint64_t>(y) * pitch * bpp;
        if (memory.store(row_addr, row_buffer.data(), pitch * bpp) != MemoryError::ok) {
            return RasterizerStatus::InvalidMemory;
        }
    }
    return RasterizerStatus::Ok;
}

RasterizerStatus GnmRasterizer::execute_draw(
    GuestMemory& memory,
    const DrawCall& draw_call,
    RasterizerStats* out_stats
) {
    const auto& rt = draw_call.pipeline_state.render_targets.targets[0];
    if (rt.base_address == 0 || rt.width == 0 || rt.height == 0) {
        return RasterizerStatus::InvalidRenderTarget;
    }
    if (rt.format != GnmSurfaceFormat::B8G8R8A8_Unorm &&
        rt.format != GnmSurfaceFormat::R8G8B8A8_Unorm) {
        return RasterizerStatus::UnsupportedFormat;
    }
    const std::uint32_t bpp = 4;
    const std::uint32_t pitch = rt.pitch > 0 ? rt.pitch : rt.width;
    const std::size_t total_bytes = static_cast<std::size_t>(pitch) * rt.height * bpp;
    if (rt.base_address > UINT64_MAX - total_bytes) {
        return RasterizerStatus::BufferOverflow;
    }

    // 1. Extração do Buffer de Vértices
    BufferDescriptor vb_desc{};
    bool found_vb = false;
    // Procura em slots comuns de User Data (0..3, 4..7, 8..11, 12..15)
    for (std::uint32_t slot = 0; slot <= 12; slot += 4) {
        std::array<std::uint32_t, 4> raw_desc{};
        for (std::uint32_t j = 0; j < 4; ++j) {
            raw_desc[j] = draw_call.pipeline_state.vs_user_data[slot + j];
        }
        std::uint64_t base = (static_cast<std::uint64_t>(raw_desc[1] & 0xFFFF) << 32) | raw_desc[0];
        std::uint32_t stride = (raw_desc[1] >> 16) & 0x3FFF;
        std::uint32_t num_rec = raw_desc[2];
        if (base != 0 && (num_rec > 0 || stride > 0)) {
            vb_desc.base_address = base;
            vb_desc.stride = stride > 0 ? stride : 16;
            vb_desc.num_records = num_rec;
            found_vb = true;
            break;
        }
    }

    if (!found_vb) {
        return RasterizerStatus::InvalidVertexBuffer;
    }

    // 2. Extração de Índices se Indexado. Limite de trabalho para não alocar
    // diretamente quantidades arbitrárias controladas pelo guest.
    constexpr std::uint32_t max_vertices_per_draw = 1'048'576;
    const auto requested = draw_call.indexed ? draw_call.index_count :
        (draw_call.vertex_count ? draw_call.vertex_count : vb_desc.num_records);
    if (requested > max_vertices_per_draw) return RasterizerStatus::BufferOverflow;
    std::vector<std::uint32_t> indices;
    if (draw_call.indexed) {
        if (draw_call.index_buffer_address == 0 || draw_call.index_count == 0) {
            return RasterizerStatus::InvalidIndexBuffer;
        }
        const std::uint64_t index_end = static_cast<std::uint64_t>(draw_call.start_index) + draw_call.index_count;
        const std::uint64_t index_stride = draw_call.index_type == GnmIndexType::Index16 ? 2 : 4;
        if (index_end > (UINT64_MAX - draw_call.index_buffer_address) / index_stride)
            return RasterizerStatus::BufferOverflow;
        indices.resize(draw_call.index_count);
        if (draw_call.index_type == GnmIndexType::Index16) {
            for (std::uint32_t i = 0; i < draw_call.index_count; ++i) {
                std::uint16_t idx16 = 0;
                std::uint64_t addr = draw_call.index_buffer_address + (draw_call.start_index + i) * 2;
                if (memory.load(addr, &idx16, 2) != MemoryError::ok) {
                    return RasterizerStatus::InvalidIndexBuffer;
                }
                indices[i] = idx16;
            }
        } else {
            for (std::uint32_t i = 0; i < draw_call.index_count; ++i) {
                std::uint32_t idx32 = 0;
                std::uint64_t addr = draw_call.index_buffer_address + (draw_call.start_index + i) * 4;
                if (memory.load(addr, &idx32, 4) != MemoryError::ok) {
                    return RasterizerStatus::InvalidIndexBuffer;
                }
                indices[i] = idx32;
            }
        }
    } else {
        const std::uint32_t count = requested;
        if (static_cast<std::uint64_t>(draw_call.start_index) + count > UINT32_MAX)
            return RasterizerStatus::BufferOverflow;
        indices.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            indices[i] = draw_call.start_index + i;
        }
    }

    if (indices.size() < 3) {
        return RasterizerStatus::Ok; // Nada a desenhar
    }
    // Somente triângulos completos são lidos pelo rasterizador.
    indices.resize((indices.size() / 3) * 3);

    // 3. Parâmetros de Viewport e Scissor
    const auto& vp = draw_call.pipeline_state.viewports.viewports[0];
    float sx = vp.scale_x;
    float ox = vp.offset_x;
    float sy = vp.scale_y;
    float oy = vp.offset_y;
    if (sx == 0.0f) {
        sx = rt.width * 0.5f;
        ox = sx;
    }
    if (sy == 0.0f) {
        sy = -static_cast<float>(rt.height) * 0.5f;
        oy = static_cast<float>(rt.height) * 0.5f;
    }

    const auto& sc = draw_call.pipeline_state.scissors.scissors[0];
    int sc_left = std::max(0, static_cast<int>(sc.left));
    int sc_right = sc.right > 0 ? std::min(static_cast<int>(rt.width) - 1, static_cast<int>(sc.right) - 1) : static_cast<int>(rt.width) - 1;
    int sc_top = std::max(0, static_cast<int>(sc.top));
    int sc_bottom = sc.bottom > 0 ? std::min(static_cast<int>(rt.height) - 1, static_cast<int>(sc.bottom) - 1) : static_cast<int>(rt.height) - 1;

    // Validar antes de escrever qualquer pixel: não tratar leitura ausente
    // como posição (0,0,0) e gerar triângulos falsos.
    for (std::uint32_t index : indices) {
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * vb_desc.stride;
        const std::uint64_t needed = vb_desc.stride >= 28 ? 28 : 12;
        if (vb_desc.stride < 12 || offset > UINT64_MAX - vb_desc.base_address ||
            vb_desc.base_address + offset > UINT64_MAX - needed)
            return RasterizerStatus::InvalidVertexBuffer;
        std::uint8_t probe[28]{};
        if (memory.load(vb_desc.base_address + offset, probe, static_cast<std::size_t>(needed)) != MemoryError::ok)
            return RasterizerStatus::InvalidVertexBuffer;
    }

    // Função auxiliar para carregar um vértice da GuestMemory
    auto load_vertex = [&](std::uint32_t index) -> Vertex2D {
        Vertex2D v{};
        std::uint64_t v_addr = vb_desc.base_address + static_cast<std::uint64_t>(index) * vb_desc.stride;
        float pos[3] = {};
        if (memory.load(v_addr, pos, 12) == MemoryError::ok) {
            v.x = pos[0];
            v.y = pos[1];
            v.z = pos[2];
        }
        if (vb_desc.stride >= 28) {
            float col[4] = {};
            if (memory.load(v_addr + 12, col, 16) == MemoryError::ok) {
                v.r = col[0];
                v.g = col[1];
                v.b = col[2];
                v.a = col[3];
            }
        } else {
            // Cor padrão de blend ou branca sólida
            const auto& bc = draw_call.pipeline_state.blend.blend_color;
            if (bc[0] > 0.0f || bc[1] > 0.0f || bc[2] > 0.0f || bc[3] > 0.0f) {
                v.r = bc[0]; v.g = bc[1]; v.b = bc[2]; v.a = bc[3];
            }
        }
        // Transforma NDC -> Screen
        v.x = (v.x * sx) + ox;
        v.y = (v.y * sy) + oy;
        return v;
    };

    std::uint32_t total_triangles = 0;
    std::uint32_t total_pixels = 0;

    // 4. Rasterização de Triângulos (TriList)
    const std::size_t num_prims = indices.size() / 3;
    for (std::size_t prim = 0; prim < num_prims; ++prim) {
        Vertex2D v0 = load_vertex(indices[prim * 3]);
        Vertex2D v1 = load_vertex(indices[prim * 3 + 1]);
        Vertex2D v2 = load_vertex(indices[prim * 3 + 2]);

        float area = edge_function(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
        if (std::abs(area) < 1e-5f) continue; // Triângulo degenerado

        int min_x = std::max(sc_left, static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x}))));
        int max_x = std::min(sc_right, static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x}))));
        int min_y = std::max(sc_top, static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y}))));
        int max_y = std::min(sc_bottom, static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y}))));

        if (min_x > max_x || min_y > max_y) continue;

        ++total_triangles;

        for (int py = min_y; py <= max_y; ++py) {
            float y = static_cast<float>(py) + 0.5f;
            for (int px = min_x; px <= max_x; ++px) {
                float x = static_cast<float>(px) + 0.5f;

                float w0 = edge_function(v1.x, v1.y, v2.x, v2.y, x, y);
                float w1 = edge_function(v2.x, v2.y, v0.x, v0.y, x, y);
                float w2 = edge_function(v0.x, v0.y, v1.x, v1.y, x, y);

                bool inside = (area > 0.0f) ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                            : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);

                if (!inside) continue;

                float b0 = w0 / area;
                float b1 = w1 / area;
                float b2 = w2 / area;

                float r = clamp01(b0 * v0.r + b1 * v1.r + b2 * v2.r);
                float g = clamp01(b0 * v0.g + b1 * v1.g + b2 * v2.g);
                float b = clamp01(b0 * v0.b + b1 * v1.b + b2 * v2.b);
                float a = clamp01(b0 * v0.a + b1 * v1.a + b2 * v2.a);

                std::uint8_t u_r = static_cast<std::uint8_t>(r * 255.0f + 0.5f);
                std::uint8_t u_g = static_cast<std::uint8_t>(g * 255.0f + 0.5f);
                std::uint8_t u_b = static_cast<std::uint8_t>(b * 255.0f + 0.5f);
                std::uint8_t u_a = static_cast<std::uint8_t>(a * 255.0f + 0.5f);

                std::uint8_t pixel_bytes[4];
                if (rt.format == GnmSurfaceFormat::B8G8R8A8_Unorm) {
                    pixel_bytes[0] = u_b;
                    pixel_bytes[1] = u_g;
                    pixel_bytes[2] = u_r;
                    pixel_bytes[3] = u_a;
                } else {
                    pixel_bytes[0] = u_r;
                    pixel_bytes[1] = u_g;
                    pixel_bytes[2] = u_b;
                    pixel_bytes[3] = u_a;
                }

                std::uint64_t px_addr = rt.base_address + (static_cast<std::uint64_t>(py) * pitch + px) * bpp;
                if (memory.store(px_addr, pixel_bytes, 4) == MemoryError::ok) {
                    ++total_pixels;
                }
            }
        }
    }

    if (out_stats) {
        out_stats->triangles_drawn += total_triangles;
        out_stats->pixels_written += total_pixels;
    }
    return RasterizerStatus::Ok;
}

RasterizerStatus GnmRasterizer::execute_draw_calls(
    GuestMemory& memory,
    const std::vector<DrawCall>& draw_calls,
    RasterizerStats* out_stats
) {
    for (const auto& dc : draw_calls) {
        RasterizerStatus st = execute_draw(memory, dc, out_stats);
        if (st != RasterizerStatus::Ok) {
            return st;
        }
    }
    return RasterizerStatus::Ok;
}

} // namespace bb
