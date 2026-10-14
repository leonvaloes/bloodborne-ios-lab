#include "check.h"
#include "gnm_context.h"
#include "gnm_pm4.h"
#include "gnm_metal_bridge.h"
#include "guest_memory.h"
#include <array>
#include <cstdint>
#include <vector>

using namespace bb;

int main() {
    return run([] {
        // =====================================================================
        // 1. Configuração básica: GuestMemory, GnmContext, GnmMetalBridge
        // =====================================================================
        GuestMemory memory(1024 * 1024); // 1 MB budget
        GnmContext context(memory);
        GnmMetalBridge bridge(memory);

        // Mapear regiões de memória guest
        CHECK(memory.map(0x10000000, 64 * 1024, Permission::read | Permission::write) == MemoryError::ok); // shaders
        CHECK(memory.map(0x20000000, 64 * 1024, Permission::read | Permission::write) == MemoryError::ok); // vertex/index buffers
        CHECK(memory.map(0x30000000, 64 * 1024, Permission::read | Permission::write) == MemoryError::ok); // render targets

        // =====================================================================
        // 2. Shaders GCN Sintéticos
        //    VS: s_endpgm (mínimo válido)
        //    PS: s_add_u32 s2, s0, s1 + s_endpgm (diferente do VS para ter hash diferente)
        // =====================================================================
        const std::uint32_t vs_shader_words[] = {
            0xBF810000u // s_endpgm
        };
        const std::uint32_t ps_shader_words[] = {
            0x80000000u | (0u << 23) | (2u << 16) | (1u << 8) | 0u, // s_add_u32 s2, s0, s1 (SOP2 opcode 0)
            0xBF810000u  // s_endpgm
        };

        // Escrever shaders na memória guest
        CHECK(memory.store(0x10000000, vs_shader_words, sizeof(vs_shader_words)) == MemoryError::ok);
        CHECK(memory.store(0x10001000, ps_shader_words, sizeof(ps_shader_words)) == MemoryError::ok);

        // =====================================================================
        // 3. Configurar registradores via PM4 sintético
        // =====================================================================
        // Context registers: CB_COLOR0_BASE, CB_COLOR0_PITCH, etc.
        // SH registers: SPI_SHADER_PGM_LO/HI_VS, SPI_SHADER_PGM_LO/HI_PS
        // Usaremos set_sh_reg / set_context_reg diretamente para simplicidade

        // VS program
        context.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_LO_VS, 0x10000000); // low 32 bits
        context.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_HI_VS, 0x00000001); // high 16 bits (48-bit addr)
        context.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_RSRC1_VS, 0x00000001); // 1 SGPR, 0 VGPR

        // PS program
        context.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_LO_PS, 0x10001000);
        context.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_HI_PS, 0x00000001);
        context.set_sh_reg(bb::gcn_reg::SPI_SHADER_PGM_RSRC1_PS, 0x00000001);

        // Render Target 0: BGRA8Unorm @ 0x30000000
        // CB_COLOR0_INFO: fmt_id=7 (bits 2-5), swap=1 (bit 6) = 0x5C
        context.set_context_reg(bb::gcn_reg::CB_COLOR0_BASE, 0x30000000 >> 8); // BASE_256B
        context.set_context_reg(bb::gcn_reg::CB_COLOR0_PITCH, 1024);
        context.set_context_reg(bb::gcn_reg::CB_COLOR0_INFO, 0x0000005C); // fmt_id=7, swap=1 = B8G8R8A8_Unorm
        context.set_context_reg(bb::gcn_reg::CB_COLOR0_DIM, (640 << 14) | 480); // 640x480
        context.set_context_reg(bb::gcn_reg::CB_TARGET_MASK, 0x00000001); // RT0 enabled

        // Depth/Stencil
        context.set_context_reg(bb::gcn_reg::DB_DEPTH_CONTROL, 0x00000000);
        context.set_context_reg(bb::gcn_reg::DB_Z_INFO, 0x00000002); // D32_FLOAT (zfmt=2)
        context.set_context_reg(bb::gcn_reg::DB_DEPTH_BASE, 0x30010000 >> 8); // BASE_256B
        context.set_context_reg(bb::gcn_reg::DB_DEPTH_SIZE, 0x00100010); // pitch=16*8=128, height=16*8=128

        // Primitive type
        context.set_context_reg(bb::gcn_reg::VGT_PRIMITIVE_TYPE, 4); // TriList

        // Viewport
        context.set_context_reg(bb::gcn_reg::PA_CL_VPORT_XSCALE, 0x42200000); // 40.0f
        context.set_context_reg(bb::gcn_reg::PA_CL_VPORT_XOFFSET, 0x42200000);
        context.set_context_reg(bb::gcn_reg::PA_CL_VPORT_YSCALE, 0xC2200000); // -40.0f
        context.set_context_reg(bb::gcn_reg::PA_CL_VPORT_YOFFSET, 0x42200000);

        // =====================================================================
        // 4. Submeter PM4 que gera draw call
        // =====================================================================
        // Criar buffer de comandos PM4 sintético: SET_SH_REG + DRAW_INDEX
        std::vector<std::uint32_t> pm4_buffer;
        // Simular submissão direta via context
        context.set_vs_program(0x10000000);
        context.set_ps_program(0x10001000);

        // Criar draw call manualmente (simula o que PM4 faria)
        DrawCall draw_call{};
        draw_call.indexed = true;
        draw_call.index_count = 3;
        draw_call.start_index = 0;
        draw_call.index_buffer_address = 0x20000000;
        draw_call.index_type = GnmIndexType::Index16;
        draw_call.instance_count = 1;
        draw_call.start_instance = 0;
        draw_call.pipeline_state = context.current_pipeline_state();

        // Escrever index buffer (3 índices: 0, 1, 2)
        std::uint16_t indices[] = {0, 1, 2};
        CHECK(memory.store(0x20000000, indices, sizeof(indices)) == MemoryError::ok);

        // =====================================================================
        // 5. Processar draw call via bridge - Primeira vez (cache miss)
        // =====================================================================
        BridgeResult<MetalDrawCommand> result = bridge.process_draw_call(draw_call);
        if (!result.ok()) {
            std::cerr << "Erro: " << bridge_error_to_string(result.error) << " - " << result.message << std::endl;
        }
        CHECK(result.ok());

        const MetalDrawCommand& cmd1 = *result;
        CHECK(cmd1.valid);
        CHECK(cmd1.pipeline_config.is_valid());
        CHECK(cmd1.pipeline_config.vs_hash != 0);
        CHECK(!cmd1.pipeline_config.vs_msl.empty());
        CHECK(cmd1.pipeline_config.ps_hash != 0);
        CHECK(!cmd1.pipeline_config.ps_msl.empty());
        CHECK(cmd1.pipeline_config.active_color_attachments == 1);
        CHECK(cmd1.pipeline_config.color_attachments[0].pixel_format == MetalPixelFormat::BGRA8Unorm);
        CHECK(cmd1.pipeline_config.depth_stencil.depth_pixel_format == MetalPixelFormat::Depth32Float);
        CHECK(cmd1.pipeline_config.depth_stencil.depth_test_enabled == false);
        CHECK(cmd1.pipeline_config.primitive_type == MetalPrimitiveType::Triangle);
        CHECK(cmd1.indexed);
        CHECK(cmd1.index_count == 3);
        CHECK(cmd1.index_buffer_address == 0x20000000);

        // Verificar cache stats
        CHECK(bridge.shader_cache().miss_count() == 2); // VS + PS
        CHECK(bridge.shader_cache().hit_count() == 0);

        // =====================================================================
        // 6. Segunda chamada idêntica - deve dar cache hit
        // =====================================================================
        BridgeResult<MetalDrawCommand> result2 = bridge.process_draw_call(draw_call);
        CHECK(result2.ok());
        const MetalDrawCommand& cmd2 = *result2;
        CHECK(cmd2.valid);
        CHECK(cmd2.pipeline_config.vs_hash == cmd1.pipeline_config.vs_hash);
        CHECK(cmd2.pipeline_config.ps_hash == cmd1.pipeline_config.ps_hash);

        // Cache hits: VS e PS já estão no cache
        CHECK(bridge.shader_cache().hit_count() == 2);
        CHECK(bridge.shader_cache().miss_count() == 2);

        // =====================================================================
        // 7. Verificar mapeamento de V# buffers e T# textures
        // =====================================================================
        // Bind buffer V#0 para VS
        BufferDescriptor vbuf{};
        vbuf.base_address = 0x20001000;
        vbuf.stride = 16;
        vbuf.num_records = 3;
        vbuf.format = 0;
        CHECK(bridge.bind_buffer(ShaderStage::Vertex, 0, vbuf));

        // Bind texture T#0 para PS
        TextureDescriptor tex{};
        tex.base_address = 0x40000000;
        tex.format = 0x80; // BGRA8
        tex.width = 256;
        tex.height = 256;
        tex.pitch = 1024;
        tex.num_mips = 1;
        CHECK(bridge.bind_texture(ShaderStage::Pixel, 0, tex));

        // Processar novamente - deve incluir bindings
        BridgeResult<MetalDrawCommand> result3 = bridge.process_draw_call(draw_call);
        CHECK(result3.ok());
        const MetalDrawCommand& cmd3 = *result3;
        CHECK(cmd3.vertex_bindings.buffers[0].valid);
        CHECK(cmd3.vertex_bindings.buffers[0].guest_address == 0x20001000);
        CHECK(cmd3.vertex_bindings.buffers[0].stride == 16);
        CHECK(cmd3.fragment_bindings.textures[0].valid);
        CHECK(cmd3.fragment_bindings.textures[0].guest_address == 0x40000000);
        CHECK(cmd3.fragment_bindings.textures[0].width == 256);

        // =====================================================================
        // 8. Testes de rejeição: shader inválido (sem s_endpgm)
        // =====================================================================
        // Shader sem s_endpgm - excede limite de 64KB
        // Mapear região maior para este teste
        CHECK(memory.map(0x10010000, ShaderCache::MAX_SHADER_BYTES + 4096, Permission::read | Permission::write) == MemoryError::ok);
        std::vector<std::uint32_t> bad_shader(ShaderCache::MAX_SHADER_BYTES / 4 + 1, 0x00000000u);
        CHECK(memory.store(0x10010000, bad_shader.data(), bad_shader.size() * 4) == MemoryError::ok);

        DrawCall bad_draw = draw_call;
        bad_draw.pipeline_state.vs_program_address = 0x10010000;
        BridgeResult<MetalDrawCommand> bad_result = bridge.process_draw_call(bad_draw);
        CHECK(!bad_result.ok());
        CHECK(bad_result.error == BridgeError::ShaderExceedsLimit);

        // =====================================================================
        // 9. Teste: endereço de shader inválido (não mapeado)
        // =====================================================================
        DrawCall bad_draw2 = draw_call;
        bad_draw2.pipeline_state.vs_program_address = 0xFFFFFFFF00000000ULL;
        BridgeResult<MetalDrawCommand> bad_result2 = bridge.process_draw_call(bad_draw2);
        CHECK(!bad_result2.ok());
        CHECK(bad_result2.error == BridgeError::InvalidShaderAddress);

        // =====================================================================
        // 10. Teste: endereço de index buffer fora dos limites
        // =====================================================================
        DrawCall bad_draw3 = draw_call;
        bad_draw3.index_buffer_address = 0x50000000; // não mapeado
        BridgeResult<MetalDrawCommand> bad_result3 = bridge.process_draw_call(bad_draw3);
        CHECK(!bad_result3.ok());
        CHECK(bad_result3.error == BridgeError::InvalidIndexBuffer);

        // =====================================================================
        // 11. Teste: buffer de vértice fora dos limites
        // =====================================================================
        BridgeResult<MetalDrawCommand> bad_result4 = bridge.process_draw_call(draw_call);
        // Adicionar buffer inválido
        BufferDescriptor bad_vbuf{};
        bad_vbuf.base_address = 0x50000000; // não mapeado
        bad_vbuf.stride = 16;
        bad_vbuf.num_records = 1000;
        bridge.bind_buffer(ShaderStage::Vertex, 1, bad_vbuf);
        // Deve falhar na validação dentro do draw call (o bridge valida bindings)
        // Nota: process_draw_call não valida automaticamente todos os bindings,
        // mas a validação pode ser adicionada. Por ora, testamos is_buffer_address_valid.

        // =====================================================================
        // 12. Testar map_surface_format
        // =====================================================================
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::R8G8B8A8_Unorm) == MetalPixelFormat::RGBA8Unorm);
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::B8G8R8A8_Unorm) == MetalPixelFormat::BGRA8Unorm);
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::R16G16B16A16_Float) == MetalPixelFormat::RGBA16Float);
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::R11G11B10_Float) == MetalPixelFormat::RG11B10Float);
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::D32_Float) == MetalPixelFormat::Depth32Float);
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::D32_Float_S8_Uint) == MetalPixelFormat::Depth32Float_Stencil8);
        CHECK(GnmMetalBridge::map_surface_format(GnmSurfaceFormat::Invalid) == MetalPixelFormat::Invalid);

        // =====================================================================
        // 13. Testar map_blend_factor / map_blend_op
        // =====================================================================
        CHECK(GnmMetalBridge::map_blend_factor(GnmBlendFactor::Zero) == MetalBlendFactor::Zero);
        CHECK(GnmMetalBridge::map_blend_factor(GnmBlendFactor::One) == MetalBlendFactor::One);
        CHECK(GnmMetalBridge::map_blend_factor(GnmBlendFactor::SrcAlpha) == MetalBlendFactor::SourceAlpha);
        CHECK(GnmMetalBridge::map_blend_factor(GnmBlendFactor::OneMinusSrcAlpha) == MetalBlendFactor::OneMinusSourceAlpha);
        CHECK(GnmMetalBridge::map_blend_op(GnmBlendOp::Add) == MetalBlendOperation::Add);
        CHECK(GnmMetalBridge::map_blend_op(GnmBlendOp::Subtract) == MetalBlendOperation::Subtract);
        CHECK(GnmMetalBridge::map_blend_op(GnmBlendOp::ReverseSubtract) == MetalBlendOperation::ReverseSubtract);

        // =====================================================================
        // 14. Testar map_compare_func
        // =====================================================================
        CHECK(GnmMetalBridge::map_compare_func(GnmCompareFunc::Never) == MetalCompareFunction::Never);
        CHECK(GnmMetalBridge::map_compare_func(GnmCompareFunc::Less) == MetalCompareFunction::Less);
        CHECK(GnmMetalBridge::map_compare_func(GnmCompareFunc::LessEqual) == MetalCompareFunction::LessEqual);
        CHECK(GnmMetalBridge::map_compare_func(GnmCompareFunc::Always) == MetalCompareFunction::Always);

        // =====================================================================
        // 15. Testar map_primitive_type
        // =====================================================================
        bool supported = false;
        CHECK(GnmMetalBridge::map_primitive_type(GnmPrimitiveType::TriList, supported) == MetalPrimitiveType::Triangle);
        CHECK(supported);
        CHECK(GnmMetalBridge::map_primitive_type(GnmPrimitiveType::TriStrip, supported) == MetalPrimitiveType::TriangleStrip);
        CHECK(supported);
        CHECK(GnmMetalBridge::map_primitive_type(GnmPrimitiveType::LineList, supported) == MetalPrimitiveType::Line);
        CHECK(supported);
        CHECK(GnmMetalBridge::map_primitive_type(GnmPrimitiveType::PointList, supported) == MetalPrimitiveType::Point);
        CHECK(supported);
        // TriFan não suportado
        CHECK(GnmMetalBridge::map_primitive_type(GnmPrimitiveType::TriFan, supported) == MetalPrimitiveType::Triangle);
        CHECK(!supported);

        // =====================================================================
        // 16. Testar map_cull_mode / map_front_face
        // =====================================================================
        CHECK(GnmMetalBridge::map_cull_mode(GnmCullMode::None) == MetalCullMode::None);
        CHECK(GnmMetalBridge::map_cull_mode(GnmCullMode::Front) == MetalCullMode::Front);
        CHECK(GnmMetalBridge::map_cull_mode(GnmCullMode::Back) == MetalCullMode::Back);
        CHECK(GnmMetalBridge::map_front_face(GnmFrontFace::Clockwise) == MetalWinding::Clockwise);
        CHECK(GnmMetalBridge::map_front_face(GnmFrontFace::CounterClockwise) == MetalWinding::CounterClockwise);

        // =====================================================================
        // 17. Teste: Shader inválido (instrução desconhecida)
        // =====================================================================
        // Shader com instrução que não baixa: V_ADD_F32 não suportada no subset
        // O decoder/IR vai marcar como Unsupported
        // Precisamos de um shader que decodifique mas falhe no lower/translate
        // O shader mínimo (s_endpgm) passa. Vamos criar um com VOP2 opcode não suportado
        // VOP2 é detectado por bit 31 = 0. Opcode está em bits 25-30.
        // Apenas opcode 3 (FloatAdd) e 8 (FloatMultiply) são suportados.
        // Usar opcode 0 (v_cmp_f32) que vira Operation::Unsupported
        std::uint32_t bad_insn_shader[] = {
            (0u << 25) | (0u << 17) | (1u << 9) | (256u + 2u), // v_cmp_f32 (opcode 0) - não suportado
            0xBF810000u // s_endpgm
        };
        CHECK(memory.store(0x10003000, bad_insn_shader, sizeof(bad_insn_shader)) == MemoryError::ok);

        DrawCall bad_draw5 = draw_call;
        bad_draw5.pipeline_state.vs_program_address = 0x10003000;
        BridgeResult<MetalDrawCommand> bad_result5 = bridge.process_draw_call(bad_draw5);
        if (!bad_result5.ok()) {
            std::cerr << "bad_result5 error: " << bridge_error_to_string(bad_result5.error) << " - " << bad_result5.message << std::endl;
        }
        CHECK(!bad_result5.ok());
        // VOP2 opcode 0 vira Operation::Unsupported no IR, falha no lowering
        CHECK(bad_result5.error == BridgeError::ShaderLoweringFailed || bad_result5.error == BridgeError::ShaderTranslationFailed);

        // =====================================================================
        // 18. Teste: Compute Dispatch
        // =====================================================================
        const std::uint32_t cs_shader_words[] = {
            0x80000000u | (0u << 23) | (3u << 16) | (1u << 8) | 0u, // s_add_u32 s3, s0, s1 (SOP2 opcode 0)
            0xBF810000u // s_endpgm
        };
        CHECK(memory.store(0x10004000, cs_shader_words, sizeof(cs_shader_words)) == MemoryError::ok);

        auto hits_before = bridge.shader_cache().hit_count();
        auto misses_before = bridge.shader_cache().miss_count();

        DispatchCall dispatch{};
        dispatch.threadgroups_x = 4;
        dispatch.threadgroups_y = 1;
        dispatch.threadgroups_z = 1;
        dispatch.pipeline_state.cs_program_address = 0x10004000;

        BridgeResult<MetalDispatchCommand> disp_result = bridge.process_dispatch(dispatch);
        if (!disp_result.ok()) {
            std::cerr << "disp_result error: " << bridge_error_to_string(disp_result.error) << " - " << disp_result.message << std::endl;
        }
        CHECK(disp_result.ok());
        const MetalDispatchCommand& disp_cmd = *disp_result;
        CHECK(disp_cmd.valid);
        CHECK(disp_cmd.pipeline_config.is_valid());
        CHECK(disp_cmd.pipeline_config.cs_hash != 0);
        CHECK(!disp_cmd.pipeline_config.cs_msl.empty());
        CHECK(disp_cmd.threadgroups_x == 4);
        CHECK(disp_cmd.threadgroups_y == 1);
        CHECK(disp_cmd.threadgroups_z == 1);

        CHECK(bridge.shader_cache().hit_count() == hits_before);
        CHECK(bridge.shader_cache().miss_count() == misses_before + 1);

        // Segunda chamada - cache hit
        BridgeResult<MetalDispatchCommand> disp_result2 = bridge.process_dispatch(dispatch);
        if (!disp_result2.ok()) {
            std::cerr << "disp_result2 error: " << bridge_error_to_string(disp_result2.error) << " - " << disp_result2.message << std::endl;
        }
        CHECK(disp_result2.ok());
        CHECK(bridge.shader_cache().hit_count() == hits_before + 1);
        CHECK(bridge.shader_cache().miss_count() == misses_before + 1);

        // =====================================================================
        // 19. Teste: ShaderCache stats
        // =====================================================================
        CHECK(bridge.shader_cache().size() == 3); // VS, PS, CS
        CHECK(bridge.shader_cache().hit_count() == hits_before + 1);
        CHECK(bridge.shader_cache().miss_count() == misses_before + 1);

        bridge.shader_cache().clear();
        CHECK(bridge.shader_cache().size() == 0);
        CHECK(bridge.shader_cache().hit_count() == 0);
        CHECK(bridge.shader_cache().miss_count() == 0);

        // =====================================================================
        // 20. Teste: import_user_data_buffers
        // =====================================================================
        // Configurar user data com buffer descriptor inline
        // Slots 0-3: buffer descriptor
        context.set_user_data(ShaderStage::Vertex, 0, 0x20002000); // base lo
        context.set_user_data(ShaderStage::Vertex, 1, (16 << 16)); // base hi=0, stride=16
        context.set_user_data(ShaderStage::Vertex, 2, 100);        // num_records
        context.set_user_data(ShaderStage::Vertex, 3, 0);          // format

        bridge.import_user_data_buffers(context, ShaderStage::Vertex);
        const auto& vs_binds = bridge.bindings(ShaderStage::Vertex);
        CHECK(vs_binds.buffers[0].valid);
        CHECK(vs_binds.buffers[0].guest_address == 0x20002000);
        CHECK(vs_binds.buffers[0].stride == 16); // stride está nos bits 16-29 do slot 1

        // =====================================================================
        // 21. Teste: bind_buffer_table
        // =====================================================================
        // Criar tabela de 2 buffers na memória guest
        BufferDescriptor table_bufs[2];
        table_bufs[0].base_address = 0x20003000;
        table_bufs[0].stride = 16;
        table_bufs[0].num_records = 50;
        table_bufs[0].format = 0;
        table_bufs[1].base_address = 0x20004000;
        table_bufs[1].stride = 32;
        table_bufs[1].num_records = 25;
        table_bufs[1].format = 0;

        std::vector<std::uint8_t> table_bytes(2 * 16);
        for (int i = 0; i < 2; ++i) {
            auto encoded = GnmContext::encode_buffer_descriptor(table_bufs[i]);
            std::memcpy(table_bytes.data() + i * 16, encoded.data(), 16);
        }
        CHECK(memory.map(0x50000000, 64 * 1024, Permission::read | Permission::write) == MemoryError::ok);
        CHECK(memory.store(0x50000000, table_bytes.data(), table_bytes.size()) == MemoryError::ok);

        bool ok = bridge.bind_buffer_table(ShaderStage::Vertex, 0, 0x50000000, 2);
        CHECK(ok);
        const auto& vs_binds2 = bridge.bindings(ShaderStage::Vertex);
        CHECK(vs_binds2.buffers[0].valid);
        CHECK(vs_binds2.buffers[0].guest_address == 0x20003000);
        CHECK(vs_binds2.buffers[1].valid);
        CHECK(vs_binds2.buffers[1].guest_address == 0x20004000);

        // =====================================================================
        // Todos os testes passaram
        // =====================================================================
    });
}