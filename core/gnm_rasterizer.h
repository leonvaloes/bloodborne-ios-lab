#pragma once

#include "guest_memory.h"
#include "gnm_context.h"
#include <cstddef>
#include <cstdint>

namespace bb {

enum class RasterizerStatus {
    Ok = 0,
    InvalidMemory,
    InvalidRenderTarget,
    InvalidTopology,
    InvalidVertexBuffer,
    InvalidIndexBuffer,
    BufferOverflow,
    UnsupportedFormat
};

struct RasterizerStats {
    std::uint32_t triangles_drawn = 0;
    std::uint32_t pixels_written = 0;
};

class GnmRasterizer {
public:
    GnmRasterizer() = default;

    // Limpeza de Render Target com cor especificada (0.0f .. 1.0f)
    RasterizerStatus clear_render_target(
        GuestMemory& memory,
        const RenderTarget& target,
        float r, float g, float b, float a
    );

    // Executa uma chamada de desenho em memória guest sobre o Render Target ativo
    RasterizerStatus execute_draw(
        GuestMemory& memory,
        const DrawCall& draw_call,
        RasterizerStats* out_stats = nullptr
    );

    // Executa uma lista completa de DrawCalls geradas por comandos PM4
    RasterizerStatus execute_draw_calls(
        GuestMemory& memory,
        const std::vector<DrawCall>& draw_calls,
        RasterizerStats* out_stats = nullptr
    );
};

} // namespace bb
