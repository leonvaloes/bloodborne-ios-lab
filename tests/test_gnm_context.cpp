#include "check.h"
#include "gnm_context.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

std::uint32_t header(std::uint32_t op, std::uint32_t count) {
    return 0xc0000000u | ((count - 1) << 16) | (op << 8);
}

void put(bb::GuestMemory& m, std::uint64_t address, const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (auto w : words) {
        for (unsigned i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    CHECK(m.store(address, bytes.data(), bytes.size()) == bb::MemoryError::ok);
}

inline std::uint32_t f2u(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(float));
    return u;
}

} // namespace

int main() {
    return run([] {
        // Aloca 1 MB para o guest testbed
        constexpr std::size_t mem_size = 1024 * 1024;
        bb::GuestMemory memory(mem_size);
        CHECK(memory.map(0x1000, mem_size - 0x1000, bb::read | bb::write) == bb::MemoryError::ok);

        bb::GnmContext ctx(memory);

        // =====================================================================
        // Teste 1: Estado inicial seguro e limpo
        // =====================================================================
        {
            CHECK(ctx.draw_calls().empty());
            CHECK(ctx.dispatch_calls().empty());
            CHECK(ctx.last_draw_call() == nullptr);
            CHECK(ctx.last_dispatch_call() == nullptr);
            CHECK(ctx.current_render_targets().active_count() == 0);
            CHECK(ctx.current_viewport(0).min_depth == 0.0f);
            CHECK(ctx.current_viewport(0).max_depth == 1.0f);
            CHECK(ctx.current_rasterizer().cull_mode == bb::GnmCullMode::Back);
            CHECK(ctx.current_rasterizer().front_face == bb::GnmFrontFace::Clockwise);
        }

        // =====================================================================
        // Teste 2: Rastreamento direto de Context Registers (CB, DB, Viewport, Scissor, Blend, Rasterizer)
        // =====================================================================
        {
            // Configura Render Target 0: base=0x4000 (shifted >> 8 = 0x40), pitch=1920, dim=(1920x1080), format=8_8_8_8 (BGRA)
            // CB_COLOR0_BASE = 0x318, CB_COLOR0_PITCH = 0x319, CB_COLOR0_INFO = 0x31C, CB_COLOR0_DIM = 0x31E
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_BASE, 0x40); // 0x40 << 8 = 0x4000
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_PITCH, 1920);
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_INFO, (7 << 2) | (2 << 6)); // fmt 7 (8_8_8_8), swap 2 (BGRA)
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_DIM, (1919 & 0x3FFF) | ((1079 & 0x3FFF) << 14));

            CHECK(ctx.current_render_targets().targets[0].is_valid());
            CHECK(ctx.current_render_targets().targets[0].base_address == 0x4000);
            CHECK(ctx.current_render_targets().targets[0].width == 1920);
            CHECK(ctx.current_render_targets().targets[0].height == 1080);
            CHECK(ctx.current_render_targets().targets[0].pitch == 1920);
            CHECK(ctx.current_render_targets().targets[0].format == bb::GnmSurfaceFormat::B8G8R8A8_Unorm);
            CHECK(ctx.current_render_targets().active_count() == 1);

            // Configura Render Target 1: HDR RGBA16F (fmt 8), base=0x8000, 1280x720
            const auto rt1_base = bb::gcn_reg::CB_COLOR0_BASE + bb::gcn_reg::CB_COLOR_STRIDE;
            ctx.set_context_reg(rt1_base + 0, 0x80);
            ctx.set_context_reg(rt1_base + 1, 1280);
            ctx.set_context_reg(rt1_base + 4, (8 << 2)); // fmt 8 (16_16_16_16)
            ctx.set_context_reg(rt1_base + 6, (1279 & 0x3FFF) | ((719 & 0x3FFF) << 14));

            CHECK(ctx.current_render_targets().targets[1].is_valid());
            CHECK(ctx.current_render_targets().targets[1].base_address == 0x8000);
            CHECK(ctx.current_render_targets().targets[1].width == 1280);
            CHECK(ctx.current_render_targets().targets[1].height == 720);
            CHECK(ctx.current_render_targets().targets[1].format == bb::GnmSurfaceFormat::R16G16B16A16_Float);
            CHECK(ctx.current_render_targets().active_count() == 2);

            // Target Mask
            ctx.set_context_reg(bb::gcn_reg::CB_TARGET_MASK, 0x0F);
            CHECK(ctx.current_render_targets().target_mask == 0x0F);

            // Depth/Stencil Surface: base=0x10000, pitch=1920, height=1080, D32_Float (fmt 2), LessEqual
            ctx.set_context_reg(bb::gcn_reg::DB_DEPTH_BASE, 0x100); // 0x100 << 8 = 0x10000
            ctx.set_context_reg(bb::gcn_reg::DB_DEPTH_SIZE, (239 & 0x3FFF) | ((134 & 0x3FFF) << 14)); // (239+1)*8=1920, (134+1)*8=1080
            ctx.set_context_reg(bb::gcn_reg::DB_Z_INFO, 2); // D32_Float
            ctx.set_context_reg(bb::gcn_reg::DB_DEPTH_CONTROL, 0x1 | 0x2 | (3 << 4)); // test=1, write=1, func=LessEqual(3)

            CHECK(ctx.current_depth_stencil().is_valid());
            CHECK(ctx.current_depth_stencil().depth_address == 0x10000);
            CHECK(ctx.current_depth_stencil().pitch == 1920);
            CHECK(ctx.current_depth_stencil().height == 1080);
            CHECK(ctx.current_depth_stencil().z_format == bb::GnmSurfaceFormat::D32_Float);
            CHECK(ctx.current_depth_stencil().depth_test_enabled);
            CHECK(ctx.current_depth_stencil().depth_write_enabled);
            CHECK(ctx.current_depth_stencil().depth_func == bb::GnmCompareFunc::LessEqual);

            // Viewport: 1920x1080 centralizado em (960, 540) com scale=(960, 540)
            ctx.set_context_reg(bb::gcn_reg::PA_CL_VPORT_XSCALE, f2u(960.0f));
            ctx.set_context_reg(bb::gcn_reg::PA_CL_VPORT_XOFFSET, f2u(960.0f));
            ctx.set_context_reg(bb::gcn_reg::PA_CL_VPORT_YSCALE, f2u(-540.0f));
            ctx.set_context_reg(bb::gcn_reg::PA_CL_VPORT_YOFFSET, f2u(540.0f));
            ctx.set_context_reg(bb::gcn_reg::PA_CL_VPORT_ZSCALE, f2u(0.5f));
            ctx.set_context_reg(bb::gcn_reg::PA_CL_VPORT_ZOFFSET, f2u(0.5f));

            const auto& vp0 = ctx.current_viewport(0);
            CHECK(std::abs(vp0.width - 1920.0f) < 1e-4f);
            CHECK(std::abs(vp0.height - 1080.0f) < 1e-4f);
            CHECK(std::abs(vp0.x - 0.0f) < 1e-4f);
            CHECK(std::abs(vp0.y - 0.0f) < 1e-4f);
            CHECK(std::abs(vp0.min_depth - 0.0f) < 1e-4f);
            CHECK(std::abs(vp0.max_depth - 1.0f) < 1e-4f);

            // Scissor: Window Scissor (10, 20) a (1910, 1070)
            ctx.set_context_reg(bb::gcn_reg::PA_SC_WINDOW_SCISSOR_TL, 10 | (20 << 16));
            ctx.set_context_reg(bb::gcn_reg::PA_SC_WINDOW_SCISSOR_BR, 1910 | (1070 << 16));
            const auto& ws = ctx.current_scissors().window_scissor;
            CHECK(ws.left == 10 && ws.top == 20 && ws.right == 1910 && ws.bottom == 1070);
            CHECK(ws.width() == 1900 && ws.height() == 1050);

            // Blend: Blend Target 0 habilitado, SrcAlpha / OneMinusSrcAlpha
            // bit 0: enable, color_src: SrcAlpha(4), color_dst: OneMinusSrcAlpha(5), op: Add(0)
            const std::uint32_t blend0_ctrl = 1 | (4 << 1) | (5 << 5) | (0 << 9);
            ctx.set_context_reg(bb::gcn_reg::CB_BLEND0_CONTROL, blend0_ctrl);
            const auto& bt0 = ctx.current_blend().targets[0];
            CHECK(bt0.blend_enable);
            CHECK(bt0.color_src_factor == bb::GnmBlendFactor::SrcAlpha);
            CHECK(bt0.color_dst_factor == bb::GnmBlendFactor::OneMinusSrcAlpha);
            CHECK(bt0.color_op == bb::GnmBlendOp::Add);

            // Blend Color
            ctx.set_context_reg(bb::gcn_reg::CB_BLEND_RED, f2u(0.25f));
            ctx.set_context_reg(bb::gcn_reg::CB_BLEND_GREEN, f2u(0.5f));
            ctx.set_context_reg(bb::gcn_reg::CB_BLEND_BLUE, f2u(0.75f));
            ctx.set_context_reg(bb::gcn_reg::CB_BLEND_ALPHA, f2u(1.0f));
            CHECK(std::abs(ctx.current_blend().blend_color[0] - 0.25f) < 1e-4f);
            CHECK(std::abs(ctx.current_blend().blend_color[1] - 0.5f) < 1e-4f);
            CHECK(std::abs(ctx.current_blend().blend_color[2] - 0.75f) < 1e-4f);
            CHECK(std::abs(ctx.current_blend().blend_color[3] - 1.0f) < 1e-4f);

            // Rasterizer: Cull None, CounterClockwise, line_width=16 (2.0f)
            ctx.set_context_reg(bb::gcn_reg::PA_SU_SC_MODE_CNTL, 0); // cull none, CCW
            ctx.set_context_reg(bb::gcn_reg::PA_SU_LINE_CNTL, 16);   // 16 / 8 = 2.0f
            ctx.set_context_reg(bb::gcn_reg::PA_SU_POLY_OFFSET_FRONT_SCALE, f2u(1.25f));
            ctx.set_context_reg(bb::gcn_reg::PA_SU_POLY_OFFSET_FRONT_OFFSET, f2u(2.5f));

            CHECK(ctx.current_rasterizer().cull_mode == bb::GnmCullMode::None);
            CHECK(ctx.current_rasterizer().front_face == bb::GnmFrontFace::CounterClockwise);
            CHECK(std::abs(ctx.current_rasterizer().line_width - 2.0f) < 1e-4f);
            CHECK(ctx.current_rasterizer().depth_bias_enable);
            CHECK(std::abs(ctx.current_rasterizer().slope_scaled_depth_bias - 1.25f) < 1e-4f);
            CHECK(std::abs(ctx.current_rasterizer().depth_bias - 2.5f) < 1e-4f);

            // Primitive Type
            ctx.set_context_reg(bb::gcn_reg::VGT_PRIMITIVE_TYPE, static_cast<std::uint32_t>(bb::GnmPrimitiveType::TriStrip));
            CHECK(ctx.current_pipeline_state().primitive_type == bb::GnmPrimitiveType::TriStrip);
        }

        // =====================================================================
        // Teste 3: Rastreamento de Shader Registers e User Data (VS, PS, CS)
        // =====================================================================
        {
            // VS Program Address: 0x0000000100020000
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_LO_VS, 0x00020000, false);
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_HI_VS, 0x00000001, false);
            CHECK(ctx.vs_program_address() == 0x0000000100020000ULL);

            // PS Program Address: 0x0000000100030000
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_LO_PS, 0x00030000, false);
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_HI_PS, 0x00000001, false);
            CHECK(ctx.ps_program_address() == 0x0000000100030000ULL);

            // CS Program Address: 0x0000000100040000
            ctx.set_sh_reg(bb::gcn_reg::COMPUTE_PGM_LO, 0x00040000, true);
            ctx.set_sh_reg(bb::gcn_reg::COMPUTE_PGM_HI, 0x00000001, true);
            CHECK(ctx.cs_program_address() == 0x0000000100040000ULL);

            // User Data VS
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_USER_DATA_VS_0 + 0, 0xAABBCCDD, false);
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_USER_DATA_VS_0 + 1, 0x11223344, false);
            CHECK(ctx.user_data(bb::ShaderStage::Vertex, 0) == 0xAABBCCDD);
            CHECK(ctx.user_data(bb::ShaderStage::Vertex, 1) == 0x11223344);

            // User Data PS
            ctx.set_sh_reg(bb::gcn_reg::SPI_SHADER_USER_DATA_PS_0 + 3, 0x55667788, false);
            CHECK(ctx.user_data(bb::ShaderStage::Pixel, 3) == 0x55667788);

            // User Data CS
            ctx.set_sh_reg(bb::gcn_reg::COMPUTE_USER_DATA_0 + 7, 0x99AABBCC, true);
            CHECK(ctx.user_data(bb::ShaderStage::Compute, 7) == 0x99AABBCC);

            // Verificação via helpers diretos
            ctx.set_vs_program(0x20000);
            ctx.set_ps_program(0x30000);
            ctx.set_cs_program(0x40000);
            CHECK(ctx.vs_program_address() == 0x20000);
            CHECK(ctx.ps_program_address() == 0x30000);
            CHECK(ctx.cs_program_address() == 0x40000);

            ctx.set_user_data(bb::ShaderStage::Vertex, 2, 0xDEADBEEF);
            CHECK(ctx.user_data(bb::ShaderStage::Vertex, 2) == 0xDEADBEEF);
        }

        // =====================================================================
        // Teste 4: Extração segura de Descritores de Buffer (V#) de GuestMemory
        // =====================================================================
        {
            // Cria descritor válido na memória guest: base=0x50000, stride=32, num_records=100
            bb::BufferDescriptor buf_in{};
            buf_in.base_address = 0x50000;
            buf_in.stride = 32;
            buf_in.num_records = 100;
            buf_in.format = 0x1234;

            auto buf_bytes = bb::GnmContext::encode_buffer_descriptor(buf_in);
            CHECK(memory.store(0x10000, buf_bytes.data(), buf_bytes.size()) == bb::MemoryError::ok);

            bb::BufferDescriptor buf_out{};
            CHECK(ctx.read_buffer_descriptor(0x10000, buf_out));
            CHECK(buf_out.base_address == 0x50000);
            CHECK(buf_out.stride == 32);
            CHECK(buf_out.num_records == 100);
            CHECK(buf_out.format == 0x1234);
            CHECK(buf_out.total_size_bytes() == 3200);
            CHECK(buf_out.is_valid());

            // Descritor bruto lido de endereço nulo ou não mapeado
            bb::BufferDescriptor bad_buf{};
            CHECK(!ctx.read_buffer_descriptor(0, bad_buf));
            CHECK(!ctx.read_buffer_descriptor(0x99999999, bad_buf));

            // Descritor com overflow intencional no cálculo de total_size (stride grande + records enorme)
            bb::BufferDescriptor ov_desc{};
            ov_desc.base_address = 0x50000;
            ov_desc.stride = 0x3FFF;
            ov_desc.num_records = 0xFFFFFFFF;
            auto ov_bytes = bb::GnmContext::encode_buffer_descriptor(ov_desc);
            CHECK(memory.store(0x10100, ov_bytes.data(), ov_bytes.size()) == bb::MemoryError::ok);
            CHECK(ctx.read_buffer_descriptor(0x10100, buf_out));
            // total_size_bytes deve lidar com overflow com segurança
            CHECK(buf_out.is_valid());

            // Extração inline de descritor a partir de User Data (slots 4..7)
            ctx.set_user_data(bb::ShaderStage::Vertex, 4, static_cast<std::uint32_t>(0x60000 & 0xFFFFFFFF));
            ctx.set_user_data(bb::ShaderStage::Vertex, 5, (16 << 16)); // stride=16, hi=0
            ctx.set_user_data(bb::ShaderStage::Vertex, 6, 256);        // num_records=256
            ctx.set_user_data(bb::ShaderStage::Vertex, 7, 0x4321);     // format
            bb::BufferDescriptor inline_buf{};
            CHECK(ctx.extract_buffer_from_user_data(bb::ShaderStage::Vertex, 4, inline_buf));
            CHECK(inline_buf.base_address == 0x60000);
            CHECK(inline_buf.stride == 16);
            CHECK(inline_buf.num_records == 256);
            CHECK(inline_buf.total_size_bytes() == 4096);
            CHECK(inline_buf.format == 0x4321);

            // Extração com slot_idx fora de limites (> 12)
            CHECK(!ctx.extract_buffer_from_user_data(bb::ShaderStage::Vertex, 13, inline_buf));
        }

        // =====================================================================
        // Teste 5: Extração segura de Descritores de Textura (T#) de GuestMemory
        // =====================================================================
        {
            bb::TextureDescriptor tex_in{};
            tex_in.base_address = 0x70000; // Alinhado a 256 bytes
            tex_in.format = 7;             // 8_8_8_8
            tex_in.width = 1920;
            tex_in.height = 1080;
            tex_in.pitch = 1920;
            tex_in.depth = 1;
            tex_in.num_mips = 5;
            tex_in.tile_mode = 2;

            auto tex_bytes = bb::GnmContext::encode_texture_descriptor(tex_in);
            CHECK(memory.store(0x20000, tex_bytes.data(), tex_bytes.size()) == bb::MemoryError::ok);

            bb::TextureDescriptor tex_out{};
            CHECK(ctx.read_texture_descriptor(0x20000, tex_out));
            CHECK(tex_out.base_address == 0x70000);
            CHECK(tex_out.format == 7);
            CHECK(tex_out.width == 1920);
            CHECK(tex_out.height == 1080);
            CHECK(tex_out.pitch == 1920);
            CHECK(tex_out.num_mips == 5);
            CHECK(tex_out.depth == 1);
            CHECK(tex_out.tile_mode == 2);
            CHECK(tex_out.is_valid());

            // Textura inválida por endereço não mapeado
            bb::TextureDescriptor bad_tex{};
            CHECK(!ctx.read_texture_descriptor(0x88888888, bad_tex));
            CHECK(!bad_tex.is_valid());
        }

        // =====================================================================
        // Teste 6: Extração de Tabelas de Recursos (Resource Table)
        // =====================================================================
        {
            constexpr std::uint64_t table_addr = 0x30000;
            // Preenche tabela com 4 descritores de buffer (cada um 16 bytes)
            for (std::uint32_t i = 0; i < 4; ++i) {
                bb::BufferDescriptor b{};
                b.base_address = 0x80000 + i * 0x1000;
                b.stride = 64;
                b.num_records = (i + 1) * 10;
                auto b_bytes = bb::GnmContext::encode_buffer_descriptor(b);
                CHECK(memory.store(table_addr + i * 16, b_bytes.data(), b_bytes.size()) == bb::MemoryError::ok);
            }

            bb::BufferDescriptor slot2_buf{};
            CHECK(ctx.read_buffer_from_table(table_addr, 2, slot2_buf));
            CHECK(slot2_buf.base_address == 0x82000);
            CHECK(slot2_buf.stride == 64);
            CHECK(slot2_buf.num_records == 30);

            // Preenche tabela de texturas (cada uma 32 bytes)
            constexpr std::uint64_t tex_table_addr = 0x31000;
            for (std::uint32_t i = 0; i < 2; ++i) {
                bb::TextureDescriptor t{};
                t.base_address = 0x90000 + i * 0x2000;
                t.width = (i + 1) * 256;
                t.height = (i + 1) * 256;
                t.pitch = t.width;
                t.num_mips = 1;
                auto t_bytes = bb::GnmContext::encode_texture_descriptor(t);
                CHECK(memory.store(tex_table_addr + i * 32, t_bytes.data(), t_bytes.size()) == bb::MemoryError::ok);
            }

            bb::TextureDescriptor slot1_tex{};
            CHECK(ctx.read_texture_from_table(tex_table_addr, 1, slot1_tex));
            CHECK(slot1_tex.base_address == 0x92000);
            CHECK(slot1_tex.width == 512);
            CHECK(slot1_tex.height == 512);

            // Acesso fora de limites na tabela (overflow em slot)
            bb::BufferDescriptor bad_table_buf{};
            CHECK(!ctx.read_buffer_from_table(table_addr, 0xFFFFFFFF, bad_table_buf));
        }

        // =====================================================================
        // Teste 7: Extração segura de Descritor de Sampler (S#)
        // =====================================================================
        {
            bb::SamplerDescriptor samp_in{};
            samp_in.clamp_x = 1; // ClampToEdge
            samp_in.clamp_y = 1;
            samp_in.clamp_z = 0; // Wrap
            samp_in.min_filter = 2; // Linear
            samp_in.mag_filter = 2; // Linear
            samp_in.mip_filter = 1; // Point
            samp_in.min_lod = 0.0f;
            samp_in.max_lod = 12.0f;
            samp_in.lod_bias = 0.5f;

            auto samp_bytes = bb::GnmContext::encode_sampler_descriptor(samp_in);
            CHECK(memory.store(0x40000, samp_bytes.data(), samp_bytes.size()) == bb::MemoryError::ok);

            bb::SamplerDescriptor samp_out{};
            CHECK(ctx.read_sampler_descriptor(0x40000, samp_out));
            CHECK(samp_out.clamp_x == 1);
            CHECK(samp_out.clamp_y == 1);
            CHECK(samp_out.min_filter == 2);
            CHECK(samp_out.mag_filter == 2);
            CHECK(samp_out.mip_filter == 1);
            CHECK(std::abs(samp_out.max_lod - 12.0f) < 0.1f);
            CHECK(std::abs(samp_out.lod_bias - 0.5f) < 0.1f);
        }

        // =====================================================================
        // Teste 8: Submissão de Command Buffer PM4 e Captura de Eventos de Draw e Dispatch
        // =====================================================================
        {
            ctx.reset();

            // Monta Command Buffer PM4:
            // 1. SET_CONTEXT_REG para configurar RT0 (1920x1080 BGRA em 0x50000)
            // 2. SET_CONTEXT_REG para configurar Viewport
            // 3. SET_SH_REG para configurar VS Program e PS Program
            // 4. DRAW_INDEX_AUTO (DrawAuto com 36 vértices)
            // 5. DRAW_INDEX_2 (DrawIndexed com 300 índices, buffer em 0x70000)
            // 6. SET_SH_REG para configurar CS Program
            // 7. DISPATCH_DIRECT (Compute Dispatch com (16, 16, 1) grupos)
            std::vector<std::uint32_t> pm4_cmds{
                // Configura RT0
                header(0x69, 3), bb::gcn_reg::CB_COLOR0_BASE, 0x500, 1920, // base=0x50000, pitch=1920
                header(0x69, 2), bb::gcn_reg::CB_COLOR0_INFO, (7 << 2) | (2 << 6), // BGRA
                header(0x69, 2), bb::gcn_reg::CB_COLOR0_DIM, (1919 & 0x3FFF) | ((1079 & 0x3FFF) << 14),

                // Configura Viewport 0: 1920x1080
                header(0x69, 7), bb::gcn_reg::PA_CL_VPORT_XSCALE,
                f2u(960.0f), f2u(960.0f), f2u(-540.0f), f2u(540.0f), f2u(0.5f), f2u(0.5f),

                // Configura Shaders VS e PS
                header(0x76, 3), bb::gcn_reg::SPI_SHADER_PGM_LO_VS, 0x110000, 0,
                header(0x76, 3), bb::gcn_reg::SPI_SHADER_PGM_LO_PS, 0x120000, 0,

                // Configura User Data VS 0 e 1 (Resource table pointer 0x30000)
                header(0x76, 3), bb::gcn_reg::SPI_SHADER_USER_DATA_VS_0, 0x30000, 0,

                // DRAW_INDEX_AUTO: 36 vertices (ex: cubo)
                header(0x2d, 2), 36, 0,

                // DRAW_INDEX_2: 300 indices, buffer=0x70000
                header(0x27, 5), 300, 0x70000, 0, 0, 299,

                // Configura Compute Shader e User Data CS
                header(0x76, 3) | 2, bb::gcn_reg::COMPUTE_PGM_LO, 0x130000, 0, // bit 2 indica compute
                header(0x76, 3) | 2, bb::gcn_reg::COMPUTE_USER_DATA_0, 0x31000, 0,

                // DISPATCH_DIRECT: (16, 16, 1) grupos
                header(0x15, 4), 16, 16, 1, 0
            };

            constexpr std::uint64_t pm4_addr = 0x5000;
            put(memory, pm4_addr, pm4_cmds);

            // Submete o buffer PM4 diretamente via GnmContext::submit
            auto status = ctx.submit(pm4_addr, static_cast<std::uint32_t>(pm4_cmds.size()));
            CHECK(status == bb::Pm4Status::ok);

            // Verifica que o estado atual do contexto foi atualizado corretamente
            CHECK(ctx.current_render_targets().targets[0].is_valid());
            CHECK(ctx.current_render_targets().targets[0].base_address == 0x50000);
            CHECK(ctx.current_render_targets().targets[0].width == 1920);
            CHECK(ctx.current_render_targets().targets[0].height == 1080);
            CHECK(ctx.vs_program_address() == 0x110000);
            CHECK(ctx.ps_program_address() == 0x120000);
            CHECK(ctx.cs_program_address() == 0x130000);
            CHECK(ctx.user_data(bb::ShaderStage::Vertex, 0) == 0x30000);
            CHECK(ctx.user_data(bb::ShaderStage::Compute, 0) == 0x31000);

            // Verifica as Draw Calls capturadas
            CHECK(ctx.draw_calls().size() == 2);

            // Draw Call 0: DRAW_INDEX_AUTO
            const auto& dc0 = ctx.draw_calls()[0];
            CHECK(!dc0.indexed);
            CHECK(dc0.vertex_count == 36);
            CHECK(dc0.index_count == 0);
            // Snapshot do pipeline no momento do Draw 0
            CHECK(dc0.pipeline_state.render_targets.targets[0].base_address == 0x50000);
            CHECK(dc0.pipeline_state.vs_program_address == 0x110000);
            CHECK(dc0.pipeline_state.ps_program_address == 0x120000);
            CHECK(dc0.pipeline_state.vs_user_data[0] == 0x30000);

            // Draw Call 1: DRAW_INDEX_2
            const auto& dc1 = ctx.draw_calls()[1];
            CHECK(dc1.indexed);
            CHECK(dc1.index_count == 300);
            CHECK(dc1.index_buffer_address == 0x70000);
            CHECK(dc1.pipeline_state.vs_program_address == 0x110000);

            // Verifica consulta tipada de draw call atual
            CHECK(ctx.current_draw_call().indexed);
            CHECK(ctx.current_draw_call().index_count == 300);
            CHECK(ctx.last_draw_call() == &ctx.draw_calls().back());

            // Verifica o Dispatch capturado
            CHECK(ctx.dispatch_calls().size() == 1);
            const auto& disp0 = ctx.dispatch_calls()[0];
            CHECK(disp0.threadgroups_x == 16);
            CHECK(disp0.threadgroups_y == 16);
            CHECK(disp0.threadgroups_z == 1);
            CHECK(disp0.pipeline_state.cs_program_address == 0x130000);
            CHECK(disp0.pipeline_state.cs_user_data[0] == 0x31000);

            // Verifica consulta tipada de dispatch call atual
            CHECK(ctx.current_dispatch_call().threadgroups_x == 16);
            CHECK(ctx.last_dispatch_call() == &ctx.dispatch_calls().back());

            // Limpa as chamadas de draw / dispatch
            ctx.clear_calls();
            CHECK(ctx.draw_calls().empty());
            CHECK(ctx.dispatch_calls().empty());
            CHECK(ctx.last_draw_call() == nullptr);
            CHECK(ctx.last_dispatch_call() == nullptr);
        }

        // =====================================================================
        // Teste 9: Modificações incrementais entre Draws mantêm snapshots corretos
        // =====================================================================
        {
            ctx.reset();

            // Draw 1 usa VS=0x1000 e RT0=0x20000
            ctx.set_vs_program(0x1000);
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_BASE, 0x200); // 0x20000
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_INFO, (7 << 2)); // fmt 7
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_DIM, (799 & 0x3FFF) | ((599 & 0x3FFF) << 14)); // 800x600

            // Emite Draw 1
            bb::Pm4Event ev1{};
            ev1.kind = bb::Pm4Kind::draw_auto;
            ev1.args[0] = 3; // Triângulo
            ctx.on_pm4_event(ev1);

            // Altera estado: VS=0x2000 e RT0=0x30000 (1920x1080)
            ctx.set_vs_program(0x2000);
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_BASE, 0x300); // 0x30000
            ctx.set_context_reg(bb::gcn_reg::CB_COLOR0_DIM, (1919 & 0x3FFF) | ((1079 & 0x3FFF) << 14));

            // Emite Draw 2
            bb::Pm4Event ev2{};
            ev2.kind = bb::Pm4Kind::draw_auto;
            ev2.args[0] = 6; // Quad
            ctx.on_pm4_event(ev2);

            CHECK(ctx.draw_calls().size() == 2);
            // Snapshot do Draw 1 não foi afetado pelas mudanças do Draw 2!
            CHECK(ctx.draw_calls()[0].pipeline_state.vs_program_address == 0x1000);
            CHECK(ctx.draw_calls()[0].pipeline_state.render_targets.targets[0].base_address == 0x20000);
            CHECK(ctx.draw_calls()[0].pipeline_state.render_targets.targets[0].width == 800);
            CHECK(ctx.draw_calls()[0].pipeline_state.render_targets.targets[0].height == 600);

            // Snapshot do Draw 2 tem o novo estado
            CHECK(ctx.draw_calls()[1].pipeline_state.vs_program_address == 0x2000);
            CHECK(ctx.draw_calls()[1].pipeline_state.render_targets.targets[0].base_address == 0x30000);
            CHECK(ctx.draw_calls()[1].pipeline_state.render_targets.targets[0].width == 1920);
            CHECK(ctx.draw_calls()[1].pipeline_state.render_targets.targets[0].height == 1080);
        }

        // =====================================================================
        // Teste 10: Tratamento Rigoroso de Bounds e Erros de Memória
        // =====================================================================
        {
            ctx.reset();

            // Command buffer com endereço desbalanceado ou inválido
            CHECK(ctx.submit(0x1001, 10) == bb::Pm4Status::invalid); // Endereço não alinhado a 4 bytes
            CHECK(ctx.submit(0x900000, 10) == bb::Pm4Status::unmapped); // Memória não mapeada
            CHECK(ctx.submit(0x1000, 0) == bb::Pm4Status::invalid); // Zero palavras

            // Command buffer com contagem truncada
            put(memory, 0x2000, {header(0x69, 10), 0x318, 1, 2}); // declara 10 dwords, fornece apenas 4
            CHECK(ctx.submit(0x2000, 4) == bb::Pm4Status::truncated);

            // Extração de descritor de buffer com endereço que cruza a fronteira da região mapeada
            bb::BufferDescriptor bound_buf{};
            CHECK(!ctx.read_buffer_descriptor(0x1000 + mem_size - 8, bound_buf)); // Faltam bytes para 16 bytes
            CHECK(!bound_buf.is_valid());

            // Extração de descritor de textura com endereço próximo ao fim da memória
            bb::TextureDescriptor bound_tex{};
            CHECK(!ctx.read_texture_descriptor(0x1000 + mem_size - 16, bound_tex)); // Faltam bytes para 32 bytes
            CHECK(!bound_tex.is_valid());
        }
    });
}
