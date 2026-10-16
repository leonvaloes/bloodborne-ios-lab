#include "check.h"
#include "gnm_rasterizer.h"
#include "video_out.h"
#include <algorithm>
#include <cmath>
#include <vector>

int main() { return run([] {
    // Memória guest com 1 MB mapeada para buffers e render targets
    bb::GuestMemory memory(1024 * 1024);
    CHECK(memory.map(0x10000, 256 * 1024, bb::read | bb::write) == bb::MemoryError::ok); // Render Target (64x64x4 = 16 KB)
    CHECK(memory.map(0x50000, 64 * 1024, bb::read | bb::write) == bb::MemoryError::ok);  // Vertex Buffer

    bb::GnmRasterizer rasterizer;

    // =========================================================================
    // 1. Teste de Clear do Render Target
    // =========================================================================
    bb::RenderTarget rt{};
    rt.base_address = 0x10000;
    rt.width = 64;
    rt.height = 64;
    rt.pitch = 64;
    rt.format = bb::GnmSurfaceFormat::B8G8R8A8_Unorm;

    auto invalid_target = rt;
    invalid_target.base_address = 0x10000 + 256 * 1024 - 4;
    CHECK(rasterizer.clear_render_target(memory, invalid_target, 1, 0, 0, 1) == bb::RasterizerStatus::InvalidRenderTarget);
    CHECK(rasterizer.clear_render_target(memory, rt, 0.2f, 0.4f, 0.6f, 1.0f) == bb::RasterizerStatus::Ok);

    // Amostragem nos cantos e no centro
    std::uint8_t px[4] = {};
    // Canto superior esquerdo (0, 0)
    CHECK(memory.load(0x10000, px, 4) == bb::MemoryError::ok);
    CHECK(px[0] == 153); // B = round(0.6 * 255)
    CHECK(px[1] == 102); // G = round(0.4 * 255)
    CHECK(px[2] == 51);  // R = round(0.2 * 255)
    CHECK(px[3] == 255); // A = 255

    // Centro (32, 32)
    std::uint64_t center_addr = 0x10000 + (32 * 64 + 32) * 4;
    CHECK(memory.load(center_addr, px, 4) == bb::MemoryError::ok);
    CHECK(px[0] == 153 && px[1] == 102 && px[2] == 51 && px[3] == 255);

    // =========================================================================
    // 2. Desenho de Triângulo Vermelho Sólido
    // =========================================================================
    // Vértices em NDC:
    // V0: (-0.6, -0.6)
    // V1: ( 0.6, -0.6)
    // V2: ( 0.0,  0.6)
    // Cada vértice: 3 floats posição (12 bytes) + 4 floats cor (16 bytes) = 28 bytes stride
    struct VertexData {
        float x, y, z;
        float r, g, b, a;
    };
    const VertexData triangle[3] = {
        { -0.6f, -0.6f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f },
        {  0.6f, -0.6f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f },
        {  0.0f,  0.6f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f }
    };
    CHECK(memory.store(0x50000, triangle, sizeof(triangle)) == bb::MemoryError::ok);

    bb::DrawCall dc{};
    dc.indexed = false;
    dc.vertex_count = 3;
    dc.start_index = 0;
    dc.pipeline_state.render_targets.targets[0] = rt;

    // Viewport: mapeia NDC [-1..1] para [0..64]
    dc.pipeline_state.viewports.viewports[0].scale_x = 32.0f;
    dc.pipeline_state.viewports.viewports[0].offset_x = 32.0f;
    dc.pipeline_state.viewports.viewports[0].scale_y = -32.0f;
    dc.pipeline_state.viewports.viewports[0].offset_y = 32.0f;

    // Scissor cobrindo a superfície toda
    dc.pipeline_state.scissors.scissors[0].left = 0;
    dc.pipeline_state.scissors.scissors[0].top = 0;
    dc.pipeline_state.scissors.scissors[0].right = 64;
    dc.pipeline_state.scissors.scissors[0].bottom = 64;

    // Codifica V#0 BufferDescriptor em vs_user_data slots 0..3:
    // base_address = 0x50000, stride = 28, num_records = 3
    bb::BufferDescriptor vb{};
    vb.base_address = 0x50000;
    vb.stride = 28;
    vb.num_records = 3;
    auto encoded = bb::GnmContext::encode_buffer_descriptor(vb);
    std::memcpy(&dc.pipeline_state.vs_user_data[0], encoded.data(), 16);

    bb::RasterizerStats stats{};
    auto invalid_draw_target = dc;
    invalid_draw_target.pipeline_state.render_targets.targets[0] = invalid_target;
    CHECK(rasterizer.execute_draw(memory, invalid_draw_target, &stats) == bb::RasterizerStatus::InvalidRenderTarget);
    CHECK(stats.pixels_written == 0);
    CHECK(rasterizer.execute_draw(memory, dc, &stats) == bb::RasterizerStatus::Ok);
    CHECK(stats.triangles_drawn == 1);
    CHECK(stats.pixels_written > 100);

    // O centro (32, 32) deve estar dentro do triângulo e ser vermelho puro (B=0, G=0, R=255, A=255)
    CHECK(memory.load(center_addr, px, 4) == bb::MemoryError::ok);
    CHECK(px[0] == 0);   // B
    CHECK(px[1] == 0);   // G
    CHECK(px[2] == 255); // R
    CHECK(px[3] == 255); // A

    // O canto externo (2, 2) deve permanecer com a cor de fundo (clear color)
    std::uint64_t corner_addr = 0x10000 + (2 * 64 + 2) * 4;
    CHECK(memory.load(corner_addr, px, 4) == bb::MemoryError::ok);
    CHECK(px[0] == 153 && px[1] == 102 && px[2] == 51 && px[3] == 255);

    bb::DrawCall invalid = dc;
    invalid.vertex_count = 1'048'577;
    CHECK(rasterizer.execute_draw(memory, invalid) == bb::RasterizerStatus::BufferOverflow);
    invalid.vertex_count = 3;
    invalid.start_index = UINT32_MAX;
    CHECK(rasterizer.execute_draw(memory, invalid) == bb::RasterizerStatus::BufferOverflow);
    invalid.start_index = 10'000;
    CHECK(rasterizer.execute_draw(memory, invalid) == bb::RasterizerStatus::InvalidVertexBuffer);
    // Uma falha de vértices não pode alterar pixels já desenhados.
    CHECK(memory.load(center_addr, px, 4) == bb::MemoryError::ok);
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255);

    // =========================================================================
    // 3. Conexão e Verificação via VideoOutManager
    // =========================================================================
    struct SimpleSink : bb::VideoOutSink {
        void on_flip(const bb::FlipEvent&) override {}
    } sink;

    bb::VideoOutManager vom;
    vom.attach_guest_memory(&memory, &sink);

    const bb::VideoBuffer vbuf{0x10000, 64, 64, 64, 4, 0x80000000, 1};
    CHECK(vom.register_buffer(0, vbuf) == bb::VideoStatus::ok);
    CHECK(vom.submit_flip(0, 1) == bb::VideoStatus::ok);

    bb::HostFrameBuffer frame;
    CHECK(vom.read_current_frame_pixels(frame));
    CHECK(frame.valid);
    CHECK(frame.width == 64 && frame.height == 64 && frame.pitch == 64);
    CHECK(frame.pixel_data.size() == 64 * 64 * 4);

    // Checagem dos pixels diretamente na estrutura de frame que o Metal consome
    std::size_t center_offset = (32 * 64 + 32) * 4;
    CHECK(frame.pixel_data[center_offset + 0] == 0);   // B
    CHECK(frame.pixel_data[center_offset + 1] == 0);   // G
    CHECK(frame.pixel_data[center_offset + 2] == 255); // R
    CHECK(frame.pixel_data[center_offset + 3] == 255); // A

    std::size_t corner_offset = (2 * 64 + 2) * 4;
    CHECK(frame.pixel_data[corner_offset + 0] == 153);
    CHECK(frame.pixel_data[corner_offset + 1] == 102);
    CHECK(frame.pixel_data[corner_offset + 2] == 51);
    CHECK(frame.pixel_data[corner_offset + 3] == 255);
}); }
