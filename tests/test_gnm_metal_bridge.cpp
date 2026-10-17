#include "check.h"
#include "gnm_context.h"
#include "gnm_pm4.h"
#include "gnm_metal_bridge.h"
#include "guest_memory.h"
#include <array>
#include <cstdint>
#include <unordered_map>
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
        CHECK(cmd1.pipeline_config.ps_msl.find("fragment GcnFragmentOut gcn_fragment") != std::string::npos);
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
        DrawCall bad_index_offset = draw_call;
        bad_index_offset.start_index = 32767; // ultrapassa região de 64 KiB
        auto bad_offset = bridge.process_draw_call(bad_index_offset);
        CHECK(!bad_offset.ok());
        CHECK(bad_offset.error == BridgeError::InvalidIndexBuffer);

        // Bytecode idêntico em VS e PS não pode compartilhar MSL de estágios diferentes.
        DrawCall same_shader = draw_call;
        same_shader.pipeline_state.ps_program_address = same_shader.pipeline_state.vs_program_address;
        auto same_result = bridge.process_draw_call(same_shader);
        CHECK(same_result.ok());
        CHECK(same_result->pipeline_config.vs_hash != same_result->pipeline_config.ps_hash);
        CHECK(same_result->pipeline_config.ps_msl.find("fragment GcnFragmentOut") != std::string::npos);
        CHECK(bridge.shader_cache().size() == 3);

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
        auto invalid_binding = bridge.process_draw_call(draw_call);
        CHECK(!invalid_binding.ok());
        CHECK(invalid_binding.error == BridgeError::InvalidBufferAddress);
        bridge.clear_bindings(ShaderStage::Vertex);

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
        CHECK(bridge.shader_cache().size() == 4); // VS, PS, PS com bytecode de VS, CS
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
        // 22. Teste T131: Construtor combinado de fontes MSL (Joint VS + PS)
        // =====================================================================
        std::string vs_code = "vertex VertexOut vertex_main(VertexIn in [[stage_in]]) { return {}; }";
        std::string ps_code = "fragment float4 fragment_main(VertexOut in [[stage_in]]) { return float4(1); }";

        CHECK(build_combined_msl_source("", "") == "");
        CHECK(build_combined_msl_source("", ps_code) == "");
        CHECK(build_combined_msl_source(vs_code, "") == vs_code);

        std::string combined = build_combined_msl_source(vs_code, ps_code);
        CHECK(combined.find("vertex_main") != std::string::npos);
        CHECK(combined.find("fragment_main") != std::string::npos);
        CHECK(combined.find("\n\n") != std::string::npos);

        // =====================================================================
        // 23. Teste T132: Preservação de Bindings por Draw Call
        // =====================================================================
        bridge.clear_bindings(ShaderStage::Vertex);
        bridge.clear_bindings(ShaderStage::Pixel);

        BufferDescriptor buf_a{};
        buf_a.base_address = 0x20000000;
        buf_a.stride = 16;
        buf_a.num_records = 10;
        bridge.bind_buffer(ShaderStage::Vertex, 0, buf_a);

        TextureDescriptor tex_a{};
        tex_a.base_address = 0x30000000;
        tex_a.width = 64;
        tex_a.height = 64;
        tex_a.pitch = 64;
        tex_a.format = static_cast<std::uint32_t>(GnmSurfaceFormat::B8G8R8A8_Unorm);
        bridge.bind_texture(ShaderStage::Pixel, 0, tex_a);

        auto cmd_res1 = bridge.process_draw_call(draw_call);
        CHECK(cmd_res1.ok());
        const MetalDrawCommand draw_cmd_a = *cmd_res1;
        CHECK(draw_cmd_a.vertex_bindings.buffers[0].valid);
        CHECK(draw_cmd_a.vertex_bindings.buffers[0].guest_address == 0x20000000);
        CHECK(draw_cmd_a.fragment_bindings.textures[0].valid);
        CHECK(draw_cmd_a.fragment_bindings.textures[0].guest_address == 0x30000000);

        // Atualizar bindings para o segundo draw
        BufferDescriptor buf_b{};
        buf_b.base_address = 0x20001000;
        buf_b.stride = 32;
        buf_b.num_records = 20;
        bridge.bind_buffer(ShaderStage::Vertex, 0, buf_b);

        TextureDescriptor tex_b{};
        tex_b.base_address = 0x30010000;
        tex_b.width = 128;
        tex_b.height = 128;
        tex_b.pitch = 128;
        tex_b.format = static_cast<std::uint32_t>(GnmSurfaceFormat::B8G8R8A8_Unorm);
        bridge.bind_texture(ShaderStage::Pixel, 0, tex_b);

        auto cmd_res2 = bridge.process_draw_call(draw_call);
        CHECK(cmd_res2.ok());
        const MetalDrawCommand draw_cmd_b = *cmd_res2;
        CHECK(draw_cmd_b.vertex_bindings.buffers[0].valid);
        CHECK(draw_cmd_b.vertex_bindings.buffers[0].guest_address == 0x20001000);
        CHECK(draw_cmd_b.fragment_bindings.textures[0].valid);
        CHECK(draw_cmd_b.fragment_bindings.textures[0].guest_address == 0x30010000);

        // draw_cmd_a preserva os bindings anteriores de forma isolada e imutável
        CHECK(draw_cmd_a.vertex_bindings.buffers[0].guest_address == 0x20000000);
        CHECK(draw_cmd_a.fragment_bindings.textures[0].guest_address == 0x30000000);

        // =====================================================================
        // 24. Teste T108: MetalPipelineCacheKey & MetalDepthStencilCacheKey
        // =====================================================================
        MetalRenderPipelineConfig pcfg1{};
        pcfg1.vs_hash = 0x1234;
        pcfg1.ps_hash = 0x5678;
        pcfg1.active_color_attachments = 1;
        pcfg1.color_attachments[0].pixel_format = MetalPixelFormat::BGRA8Unorm;
        pcfg1.color_attachments[0].blending_enabled = true;
        pcfg1.color_attachments[0].source_rgb_blend_factor = MetalBlendFactor::SourceAlpha;
        pcfg1.color_attachments[0].destination_rgb_blend_factor = MetalBlendFactor::OneMinusSourceAlpha;
        pcfg1.depth_stencil.depth_pixel_format = MetalPixelFormat::Depth32Float;

        MetalRenderPipelineConfig pcfg2 = pcfg1; // Cópia idêntica
        auto key1 = MetalPipelineCacheKey::from_pipeline_config(pcfg1);
        auto key2 = MetalPipelineCacheKey::from_pipeline_config(pcfg2);
        CHECK(key1 == key2);
        CHECK(MetalPipelineCacheKeyHash()(key1) == MetalPipelineCacheKeyHash()(key2));

        // Alteração no blend: chaves devem diferir mesmo com shaders idênticos
        pcfg2.color_attachments[0].blending_enabled = false;
        auto key3 = MetalPipelineCacheKey::from_pipeline_config(pcfg2);
        CHECK(!(key1 == key3));

        // Alteração no formato de profundidade: chaves devem diferir
        pcfg2.color_attachments[0].blending_enabled = true;
        pcfg2.depth_stencil.depth_pixel_format = MetalPixelFormat::Depth16Unorm;
        auto key4 = MetalPipelineCacheKey::from_pipeline_config(pcfg2);
        CHECK(!(key1 == key4));

        // DepthStencilCacheKey
        MetalDepthStencilConfig ds_cfg1{};
        ds_cfg1.depth_pixel_format = MetalPixelFormat::Depth32Float;
        ds_cfg1.depth_test_enabled = true;
        ds_cfg1.depth_write_enabled = true;
        ds_cfg1.depth_compare_function = MetalCompareFunction::LessEqual;

        MetalDepthStencilConfig ds_cfg2 = ds_cfg1;
        auto ds_key1 = MetalDepthStencilCacheKey::from_config(ds_cfg1);
        auto ds_key2 = MetalDepthStencilCacheKey::from_config(ds_cfg2);
        CHECK(ds_key1 == ds_key2);
        CHECK(MetalDepthStencilCacheKeyHash()(ds_key1) == MetalDepthStencilCacheKeyHash()(ds_key2));

        ds_cfg2.depth_compare_function = MetalCompareFunction::Greater;
        auto ds_key3 = MetalDepthStencilCacheKey::from_config(ds_cfg2);
        CHECK(!(ds_key1 == ds_key3));

        // =====================================================================
        // 25. Teste T133: Samplers e MetalSamplerCacheKey
        // =====================================================================
        SamplerDescriptor samp_desc{};
        samp_desc.min_filter = 1; // Linear
        samp_desc.mag_filter = 1; // Linear
        samp_desc.mip_filter = 2; // Linear mip
        samp_desc.min_lod = 0.5f;
        samp_desc.max_lod = 8.0f;
        bridge.bind_sampler(ShaderStage::Pixel, 0, samp_desc);

        const auto& p_binds = bridge.bindings(ShaderStage::Pixel);
        CHECK(p_binds.samplers[0].valid);
        CHECK(p_binds.samplers[0].min_filter == 1);
        CHECK(p_binds.samplers[0].min_lod == 0.5f);

        auto samp_key1 = MetalSamplerCacheKey::from_binding(p_binds.samplers[0]);
        auto samp_key2 = samp_key1;
        CHECK(samp_key1 == samp_key2);
        CHECK(MetalSamplerCacheKeyHash()(samp_key1) == MetalSamplerCacheKeyHash()(samp_key2));

        MetalSamplerBinding samp_b2 = p_binds.samplers[0];
        samp_b2.min_filter = 0; // Nearest
        auto samp_key3 = MetalSamplerCacheKey::from_binding(samp_b2);
        CHECK(!(samp_key1 == samp_key3));

        // =====================================================================
        // 26. Teste T134: Validador de Compute Dispatch
        // =====================================================================
        MetalDispatchCommand valid_disp{};
        valid_disp.valid = true;
        valid_disp.pipeline_config.cs_hash = 0xABCDE;
        valid_disp.pipeline_config.cs_msl = "kernel void compute_main() {}";
        valid_disp.threadgroups_x = 2;
        valid_disp.threadgroups_y = 2;
        valid_disp.threadgroups_z = 1;
        valid_disp.threads_per_threadgroup_x = 64;
        valid_disp.threads_per_threadgroup_y = 1;
        valid_disp.threads_per_threadgroup_z = 1;

        auto v_res = validate_compute_dispatch(valid_disp);
        CHECK(v_res.ok());
        CHECK(v_res.value == true);

        // Inválido por flag
        valid_disp.valid = false;
        CHECK(!validate_compute_dispatch(valid_disp).ok());
        valid_disp.valid = true;

        // Inválido por threadgroups = 0
        valid_disp.threadgroups_x = 0;
        CHECK(!validate_compute_dispatch(valid_disp).ok());
        valid_disp.threadgroups_x = 2;

        // Inválido por falta de MSL
        valid_disp.pipeline_config.cs_msl = "";
        CHECK(!validate_compute_dispatch(valid_disp).ok());

        // =====================================================================
        // 27. Teste T135: Validação de Pré-condições de Execução de Draws
        // =====================================================================
        CHECK(!validate_draw_execution_preconditions(nullptr, 0));
        CHECK(!validate_draw_execution_preconditions(nullptr, 5));

        MetalDrawCommand empty_cmds[2]{};
        empty_cmds[0].valid = false;
        empty_cmds[1].valid = false;
        CHECK(!validate_draw_execution_preconditions(empty_cmds, 2));

        empty_cmds[1].valid = true;
        CHECK(validate_draw_execution_preconditions(empty_cmds, 2));

        // =====================================================================
        // 28. Teste T107: Consumo de Dados Reais da Memória Guest
        // =====================================================================
        // Validar que os bytes de vértices/índices residem no endereço guest correto
        std::uint8_t guest_sentinel_bytes[64];
        for (int i = 0; i < 64; ++i) guest_sentinel_bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
        CHECK(memory.store(0x20000000, guest_sentinel_bytes, sizeof(guest_sentinel_bytes)) == MemoryError::ok);

        std::uint8_t readback[64]{};
        CHECK(memory.load(0x20000000, readback, sizeof(readback)) == MemoryError::ok);
        for (int i = 0; i < 64; ++i) {
            CHECK(readback[i] == static_cast<std::uint8_t>(0xA0 + i));
        }

        // =====================================================================
        // 29. Teste T168: Múltiplos Render Targets (MRT) e Formatos Distintos
        // =====================================================================
        // A. Pass com 2 Render Targets (RT0 = BGRA8Unorm, RT1 = RGBA16Float)
        DrawCall mrt_draw = draw_call;
        mrt_draw.pipeline_state.render_targets = RenderTargetsState{};
        mrt_draw.pipeline_state.render_targets.target_mask = 0x00000003; // RT0 e RT1 ativos

        // RT0: BGRA8Unorm @ 0x30000000
        mrt_draw.pipeline_state.render_targets.targets[0].base_address = 0x30000000;
        mrt_draw.pipeline_state.render_targets.targets[0].width = 640;
        mrt_draw.pipeline_state.render_targets.targets[0].height = 480;
        mrt_draw.pipeline_state.render_targets.targets[0].pitch = 640;
        mrt_draw.pipeline_state.render_targets.targets[0].format = GnmSurfaceFormat::B8G8R8A8_Unorm;
        mrt_draw.pipeline_state.render_targets.targets[0].enabled = true;

        // RT1: RGBA16Float @ 0x30020000
        mrt_draw.pipeline_state.render_targets.targets[1].base_address = 0x30020000;
        mrt_draw.pipeline_state.render_targets.targets[1].width = 640;
        mrt_draw.pipeline_state.render_targets.targets[1].height = 480;
        mrt_draw.pipeline_state.render_targets.targets[1].pitch = 640;
        mrt_draw.pipeline_state.render_targets.targets[1].format = GnmSurfaceFormat::R16G16B16A16_Float;
        mrt_draw.pipeline_state.render_targets.targets[1].enabled = true;

        auto mrt_result = bridge.process_draw_call(mrt_draw);
        CHECK(mrt_result.ok());
        const MetalDrawCommand& mrt_cmd = *mrt_result;
        CHECK(mrt_cmd.valid);
        CHECK(mrt_cmd.pipeline_config.active_color_attachments == 2);
        CHECK(mrt_cmd.pipeline_config.color_attachments[0].pixel_format == MetalPixelFormat::BGRA8Unorm);
        CHECK(mrt_cmd.pipeline_config.color_attachments[1].pixel_format == MetalPixelFormat::RGBA16Float);

        // B. Pass com 4 Render Targets de formatos heterogêneos
        // RT0=BGRA8, RT1=RGBA8, RT2=RGBA16Float, RT3=RG11B10Float
        DrawCall mrt4_draw = mrt_draw;
        mrt4_draw.pipeline_state.render_targets.target_mask = 0x0000000F;
        mrt4_draw.pipeline_state.render_targets.targets[1].format = GnmSurfaceFormat::R8G8B8A8_Unorm;
        mrt4_draw.pipeline_state.render_targets.targets[1].enabled = true;
        mrt4_draw.pipeline_state.render_targets.targets[2].base_address = 0x30030000;
        mrt4_draw.pipeline_state.render_targets.targets[2].width = 640;
        mrt4_draw.pipeline_state.render_targets.targets[2].height = 480;
        mrt4_draw.pipeline_state.render_targets.targets[2].pitch = 640;
        mrt4_draw.pipeline_state.render_targets.targets[2].format = GnmSurfaceFormat::R16G16B16A16_Float;
        mrt4_draw.pipeline_state.render_targets.targets[2].enabled = true;

        mrt4_draw.pipeline_state.render_targets.targets[3].base_address = 0x30040000;
        mrt4_draw.pipeline_state.render_targets.targets[3].width = 640;
        mrt4_draw.pipeline_state.render_targets.targets[3].height = 480;
        mrt4_draw.pipeline_state.render_targets.targets[3].pitch = 640;
        mrt4_draw.pipeline_state.render_targets.targets[3].format = GnmSurfaceFormat::R11G11B10_Float;
        mrt4_draw.pipeline_state.render_targets.targets[3].enabled = true;

        auto mrt4_result = bridge.process_draw_call(mrt4_draw);
        CHECK(mrt4_result.ok());
        CHECK(mrt4_result->pipeline_config.active_color_attachments == 4);
        CHECK(mrt4_result->pipeline_config.color_attachments[0].pixel_format == MetalPixelFormat::BGRA8Unorm);
        CHECK(mrt4_result->pipeline_config.color_attachments[1].pixel_format == MetalPixelFormat::RGBA8Unorm);
        CHECK(mrt4_result->pipeline_config.color_attachments[2].pixel_format == MetalPixelFormat::RGBA16Float);
        CHECK(mrt4_result->pipeline_config.color_attachments[3].pixel_format == MetalPixelFormat::RG11B10Float);

        // C. Rejeição com diagnóstico quando um dos attachments tem formato inválido
        DrawCall bad_rt_draw = mrt_draw;
        bad_rt_draw.pipeline_state.render_targets.targets[1].format = GnmSurfaceFormat::Invalid;
        auto bad_rt_res = bridge.process_draw_call(bad_rt_draw);
        CHECK(!bad_rt_res.ok());
        CHECK(bad_rt_res.error == BridgeError::PipelineConfigurationFailed);
        CHECK(bad_rt_res.message.find("unsupported render target format") != std::string::npos ||
              bad_rt_res.message.find("attachment 1") != std::string::npos);

        // D. Suporte a Depth-Only pass (active_rt == 0 com D32_Float)
        DrawCall depth_only_draw = draw_call;
        depth_only_draw.pipeline_state.render_targets = RenderTargetsState{}; // sem targets de cor
        depth_only_draw.pipeline_state.depth_stencil.z_format = GnmSurfaceFormat::D32_Float;
        depth_only_draw.pipeline_state.depth_stencil.depth_test_enabled = true;
        depth_only_draw.pipeline_state.depth_stencil.depth_write_enabled = true;
        auto depth_res = bridge.process_draw_call(depth_only_draw);
        CHECK(depth_res.ok());
        CHECK(depth_res->pipeline_config.active_color_attachments == 0);
        CHECK(depth_res->pipeline_config.depth_stencil.depth_pixel_format == MetalPixelFormat::Depth32Float);
        CHECK(depth_res->pipeline_config.depth_stencil.depth_test_enabled == true);

        // E. Rejeição de draw sem targets de cor E sem depth attachment
        DrawCall no_target_draw = depth_only_draw;
        no_target_draw.pipeline_state.depth_stencil.z_format = GnmSurfaceFormat::Invalid;
        auto no_target_res = bridge.process_draw_call(no_target_draw);
        CHECK(!no_target_res.ok());
        CHECK(no_target_res.error == BridgeError::PipelineConfigurationFailed);
        CHECK(no_target_res.message.find("no active color targets and no depth attachment") != std::string::npos);

        // F. Diferenciação no cache de pipeline Metal entre 1 RT vs 2 RTs vs formatos distintos
        auto key_1rt = MetalPipelineCacheKey::from_pipeline_config(cmd1.pipeline_config); // 1 RT BGRA8
        auto key_2rt = MetalPipelineCacheKey::from_pipeline_config(mrt_cmd.pipeline_config);        // 2 RT BGRA8 + RGBA16F
        CHECK(!(key_1rt == key_2rt));
        CHECK(MetalPipelineCacheKeyHash()(key_1rt) != MetalPipelineCacheKeyHash()(key_2rt));

        // =====================================================================
        // 30. Teste T173: Transporte de Cor Constante de Blend e Depth Bias
        // =====================================================================
        // A. Draw call com Blend Color constante (R=0.25, G=0.5, B=0.75, A=1.0)
        DrawCall blend_color_draw = draw_call;
        blend_color_draw.pipeline_state.blend.blend_color = {0.25f, 0.5f, 0.75f, 1.0f};

        auto bc_res = bridge.process_draw_call(blend_color_draw);
        CHECK(bc_res.ok());
        CHECK(std::abs(bc_res->blend_color[0] - 0.25f) < 1e-4f);
        CHECK(std::abs(bc_res->blend_color[1] - 0.5f) < 1e-4f);
        CHECK(std::abs(bc_res->blend_color[2] - 0.75f) < 1e-4f);
        CHECK(std::abs(bc_res->blend_color[3] - 1.0f) < 1e-4f);

        // B. Draw call com Depth Bias habilitado
        DrawCall depth_bias_draw = draw_call;
        depth_bias_draw.pipeline_state.rasterizer.depth_bias_enable = true;
        depth_bias_draw.pipeline_state.rasterizer.depth_bias = 2.5f;
        depth_bias_draw.pipeline_state.rasterizer.slope_scaled_depth_bias = 1.25f;
        depth_bias_draw.pipeline_state.rasterizer.depth_bias_clamp = 0.5f;

        auto db_res = bridge.process_draw_call(depth_bias_draw);
        CHECK(db_res.ok());
        CHECK(db_res->depth_bias_enable == true);
        CHECK(std::abs(db_res->depth_bias - 2.5f) < 1e-4f);
        CHECK(std::abs(db_res->slope_scaled_depth_bias - 1.25f) < 1e-4f);
        CHECK(std::abs(db_res->depth_bias_clamp - 0.5f) < 1e-4f);

        // C. Alternância: draw subsequente sem depth bias e com blend color padrão
        DrawCall clean_draw = draw_call;
        clean_draw.pipeline_state.rasterizer.depth_bias_enable = false;
        clean_draw.pipeline_state.rasterizer.depth_bias = 0.0f;
        clean_draw.pipeline_state.rasterizer.slope_scaled_depth_bias = 0.0f;
        clean_draw.pipeline_state.rasterizer.depth_bias_clamp = 0.0f;
        clean_draw.pipeline_state.blend.blend_color = {0.0f, 0.0f, 0.0f, 0.0f};

        auto clean_res = bridge.process_draw_call(clean_draw);
        CHECK(clean_res.ok());
        CHECK(clean_res->depth_bias_enable == false);
        CHECK(clean_res->depth_bias == 0.0f);
        CHECK(clean_res->blend_color[0] == 0.0f && clean_res->blend_color[3] == 0.0f);

        // =====================================================================
        // 31. Teste T163: UVW Clamp Address Modes e Anisotropia em Samplers
        // =====================================================================
        SamplerDescriptor uvw_samp{};
        uvw_samp.min_filter = 1;
        uvw_samp.mag_filter = 1;
        uvw_samp.mip_filter = 1;
        uvw_samp.clamp_x = 2; // ClampToEdge
        uvw_samp.clamp_y = 3; // ClampToBorderColor
        uvw_samp.clamp_z = 4; // MirrorRepeat
        uvw_samp.aniso_ratio = 4; // 16x anisotropy
        bridge.bind_sampler(ShaderStage::Pixel, 1, uvw_samp);

        const auto& p_binds_uvw = bridge.bindings(ShaderStage::Pixel);
        CHECK(p_binds_uvw.samplers[1].valid);
        CHECK(p_binds_uvw.samplers[1].clamp_x == 2);
        CHECK(p_binds_uvw.samplers[1].clamp_y == 3);
        CHECK(p_binds_uvw.samplers[1].clamp_z == 4);
        CHECK(p_binds_uvw.samplers[1].max_anisotropy == 16);

        auto uvw_key1 = MetalSamplerCacheKey::from_binding(p_binds_uvw.samplers[1]);
        auto uvw_key2 = uvw_key1;
        CHECK(uvw_key1 == uvw_key2);
        CHECK(MetalSamplerCacheKeyHash()(uvw_key1) == MetalSamplerCacheKeyHash()(uvw_key2));

        // Diferenciação de chave por clamp_x
        MetalSamplerBinding uvw_mod = p_binds_uvw.samplers[1];
        uvw_mod.clamp_x = 0; // Repeat
        auto uvw_key_diff_x = MetalSamplerCacheKey::from_binding(uvw_mod);
        CHECK(!(uvw_key1 == uvw_key_diff_x));

        // Diferenciação de chave por clamp_y
        uvw_mod = p_binds_uvw.samplers[1];
        uvw_mod.clamp_y = 0;
        auto uvw_key_diff_y = MetalSamplerCacheKey::from_binding(uvw_mod);
        CHECK(!(uvw_key1 == uvw_key_diff_y));

        // Diferenciação de chave por clamp_z
        uvw_mod = p_binds_uvw.samplers[1];
        uvw_mod.clamp_z = 0;
        auto uvw_key_diff_z = MetalSamplerCacheKey::from_binding(uvw_mod);
        CHECK(!(uvw_key1 == uvw_key_diff_z));

        // Diferenciação de chave por max_anisotropy
        uvw_mod = p_binds_uvw.samplers[1];
        uvw_mod.max_anisotropy = 1;
        auto uvw_key_diff_aniso = MetalSamplerCacheKey::from_binding(uvw_mod);
        CHECK(!(uvw_key1 == uvw_key_diff_aniso));
        CHECK(MetalSamplerCacheKeyHash()(uvw_key1) != MetalSamplerCacheKeyHash()(uvw_key_diff_aniso));

        // =====================================================================
        // 32. Teste T172: Cálculo de Tamanho de Texel e Limites de Upload
        // =====================================================================
        // A. Verificação de get_texel_byte_size para cada formato mapeado
        CHECK(get_texel_byte_size(MetalPixelFormat::RGBA16Float) == 8);
        CHECK(get_texel_byte_size(MetalPixelFormat::RGBA8Unorm) == 4);
        CHECK(get_texel_byte_size(MetalPixelFormat::BGRA8Unorm) == 4);
        CHECK(get_texel_byte_size(MetalPixelFormat::RG11B10Float) == 4);
        CHECK(get_texel_byte_size(MetalPixelFormat::Depth32Float) == 4);
        CHECK(get_texel_byte_size(MetalPixelFormat::Depth32Float_Stencil8) == 4);
        CHECK(get_texel_byte_size(MetalPixelFormat::Depth16Unorm) == 2);
        CHECK(get_texel_byte_size(MetalPixelFormat::Stencil8) == 1);
        CHECK(get_texel_byte_size(MetalPixelFormat::Invalid) == 4);

        // B. Verificação de calculate_bytes_per_row
        CHECK(calculate_bytes_per_row(MetalPixelFormat::RGBA16Float, 1920) == 1920 * 8);
        CHECK(calculate_bytes_per_row(MetalPixelFormat::BGRA8Unorm, 1920) == 1920 * 4);
        CHECK(calculate_bytes_per_row(MetalPixelFormat::Depth16Unorm, 256) == 256 * 2);

        // C. Validação de limites de upload para dimensões válidas
        std::size_t exact_rgba8_size = 1920 * 1080 * 4;
        CHECK(validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 1920, 1080, 1920, exact_rgba8_size));
        // Buffer maior que o mínimo é aceito
        CHECK(validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 1920, 1080, 1920, exact_rgba8_size + 4096));
        // Buffer menor que o necessário é rejeitado
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 1920, 1080, 1920, exact_rgba8_size - 1));

        // Pitch com padding (ex: pitch 2048 para largura 1920 em RGBA16Float)
        std::size_t padded_rgba16_size = 2048 * 1080 * 8;
        CHECK(validate_texture_upload_bounds(MetalPixelFormat::RGBA16Float, 1920, 1080, 2048, padded_rgba16_size));
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA16Float, 1920, 1080, 2048, padded_rgba16_size - 1));

        // D. Rejeição de entradas degeneradas
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 0, 1080, 1920, exact_rgba8_size));
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 1920, 0, 1920, exact_rgba8_size));
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 1920, 1080, 1000, exact_rgba8_size)); // pitch < width
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 20000, 1080, 20000, exact_rgba8_size)); // width > 16384
        CHECK(!validate_texture_upload_bounds(MetalPixelFormat::RGBA8Unorm, 1920, 20000, 1920, exact_rgba8_size)); // height > 16384

        // =====================================================================
        // 33. Teste T158: Validação de Leituras Guest (Buffer, Texture, Index)
        // =====================================================================
        std::uint8_t dummy_buf[16] = {1, 2, 3, 4};
        CHECK(!validate_guest_buffer_read(nullptr, 16));
        CHECK(!validate_guest_buffer_read(dummy_buf, 0));
        CHECK(validate_guest_buffer_read(dummy_buf, 16));

        CHECK(!validate_guest_texture_read(nullptr, 64, 64, 4));
        CHECK(!validate_guest_texture_read(dummy_buf, 0, 64, 4));
        CHECK(!validate_guest_texture_read(dummy_buf, 64, 0, 4));
        CHECK(!validate_guest_texture_read(dummy_buf, 64, 64, 0));
        CHECK(validate_guest_texture_read(dummy_buf, 64, 64, 4));

        CHECK(!validate_guest_index_read(nullptr, 10, GnmIndexType::Index16));
        CHECK(!validate_guest_index_read(dummy_buf, 0, GnmIndexType::Index16));
        CHECK(!validate_guest_index_read(dummy_buf, 10, static_cast<GnmIndexType>(99)));
        CHECK(validate_guest_index_read(dummy_buf, 10, GnmIndexType::Index16));
        CHECK(validate_guest_index_read(dummy_buf, 10, GnmIndexType::Index32));

        // =====================================================================
        // 34. Teste T159: Chaves de Cache de Recursos e Invalidação de Coerência
        // =====================================================================
        MetalBufferCacheKey bk1{0x2000, 1024};
        MetalBufferCacheKey bk2{0x2000, 1024};
        MetalBufferCacheKey bk3{0x3000, 1024};
        MetalBufferCacheKey bk4{0x2000, 2048};
        CHECK(bk1 == bk2);
        CHECK(MetalBufferCacheKeyHash()(bk1) == MetalBufferCacheKeyHash()(bk2));
        CHECK(!(bk1 == bk3));
        CHECK(!(bk1 == bk4));

        MetalTextureCacheKey tk1{0x4000, 256, 256, 256, MetalPixelFormat::BGRA8Unorm, 1};
        MetalTextureCacheKey tk2{0x4000, 256, 256, 256, MetalPixelFormat::BGRA8Unorm, 1};
        CHECK(tk1 == tk2);
        CHECK(MetalTextureCacheKeyHash()(tk1) == MetalTextureCacheKeyHash()(tk2));

        MetalTextureCacheKey tk_diff = tk1;
        tk_diff.guest_address = 0x5000;
        CHECK(!(tk1 == tk_diff));
        tk_diff = tk1; tk_diff.width = 512;
        CHECK(!(tk1 == tk_diff));
        tk_diff = tk1; tk_diff.height = 512;
        CHECK(!(tk1 == tk_diff));
        tk_diff = tk1; tk_diff.pitch = 512;
        CHECK(!(tk1 == tk_diff));
        tk_diff = tk1; tk_diff.format = MetalPixelFormat::RGBA8Unorm;
        CHECK(!(tk1 == tk_diff));
        tk_diff = tk1; tk_diff.num_mips = 4;
        CHECK(!(tk1 == tk_diff));

        gnm_bridge_invalidate_buffer(0x2000);
        gnm_bridge_invalidate_texture(0x4000);

        // =====================================================================
        // 35. Teste T160: Attachments Depth/Stencil Offscreen e Compatibilidade
        // =====================================================================
        CHECK(!is_depth_stencil_attachment_compatible(MetalDepthStencilConfig{}, 0, 1080));
        CHECK(!is_depth_stencil_attachment_compatible(MetalDepthStencilConfig{}, 1920, 0));

        MetalDepthStencilConfig ds_empty{};
        CHECK(is_depth_stencil_attachment_compatible(ds_empty, 1920, 1080));

        MetalDepthStencilConfig ds_valid_depth{};
        ds_valid_depth.depth_test_enabled = true;
        ds_valid_depth.depth_pixel_format = MetalPixelFormat::Depth32Float;
        CHECK(is_depth_stencil_attachment_compatible(ds_valid_depth, 1920, 1080));

        ds_valid_depth.depth_pixel_format = MetalPixelFormat::Depth16Unorm;
        CHECK(is_depth_stencil_attachment_compatible(ds_valid_depth, 1920, 1080));

        ds_valid_depth.depth_pixel_format = MetalPixelFormat::Depth32Float_Stencil8;
        CHECK(is_depth_stencil_attachment_compatible(ds_valid_depth, 1920, 1080));

        ds_valid_depth.depth_pixel_format = MetalPixelFormat::RGBA8Unorm;
        CHECK(!is_depth_stencil_attachment_compatible(ds_valid_depth, 1920, 1080));

        ds_valid_depth.depth_pixel_format = MetalPixelFormat::Invalid;
        CHECK(!is_depth_stencil_attachment_compatible(ds_valid_depth, 1920, 1080));

        MetalDepthStencilConfig ds_valid_stencil{};
        ds_valid_stencil.stencil_test_enabled = true;
        ds_valid_stencil.stencil_pixel_format = MetalPixelFormat::Stencil8;
        CHECK(is_depth_stencil_attachment_compatible(ds_valid_stencil, 1920, 1080));

        ds_valid_stencil.stencil_pixel_format = MetalPixelFormat::Depth32Float_Stencil8;
        CHECK(is_depth_stencil_attachment_compatible(ds_valid_stencil, 1920, 1080));

        ds_valid_stencil.stencil_pixel_format = MetalPixelFormat::Depth32Float;
        CHECK(!is_depth_stencil_attachment_compatible(ds_valid_stencil, 1920, 1080));

        MetalDrawCommand ds_cmds[2]{};
        ds_cmds[0].valid = true;
        ds_cmds[0].pipeline_config.depth_stencil.depth_test_enabled = true;
        ds_cmds[0].pipeline_config.depth_stencil.depth_pixel_format = MetalPixelFormat::Depth32Float;
        ds_cmds[0].pipeline_config.depth_stencil.depth_compare_function = MetalCompareFunction::LessEqual;

        ds_cmds[1].valid = true;
        ds_cmds[1].pipeline_config.depth_stencil.stencil_test_enabled = true;
        ds_cmds[1].pipeline_config.depth_stencil.stencil_pixel_format = MetalPixelFormat::Stencil8;
        ds_cmds[1].pipeline_config.depth_stencil.stencil_compare_function = MetalCompareFunction::Always;

        auto aggregated_ds = get_active_depth_stencil_config(ds_cmds, 2);
        CHECK(aggregated_ds.depth_test_enabled);
        CHECK(aggregated_ds.depth_pixel_format == MetalPixelFormat::Depth32Float);
        CHECK(aggregated_ds.depth_compare_function == MetalCompareFunction::LessEqual);
        CHECK(aggregated_ds.stencil_test_enabled);
        CHECK(aggregated_ds.stencil_pixel_format == MetalPixelFormat::Stencil8);
        CHECK(aggregated_ds.stencil_compare_function == MetalCompareFunction::Always);

        auto null_ds = get_active_depth_stencil_config(nullptr, 0);
        CHECK(!null_ds.depth_test_enabled);
        CHECK(!null_ds.stencil_test_enabled);

        // =====================================================================
        // 36. Teste T169: Cálculo de Mipmaps e Limites de LOD de Sampler
        // =====================================================================
        auto mips = calculate_texture_mipmap_levels(MetalPixelFormat::RGBA8Unorm, 256, 128, 256, 3);
        CHECK(mips.size() == 3);
        CHECK(mips[0].level == 0 && mips[0].width == 256 && mips[0].height == 128 && mips[0].pitch == 256);
        CHECK(mips[0].byte_size == 256 * 128 * 4);
        CHECK(mips[1].level == 1 && mips[1].width == 128 && mips[1].height == 64 && mips[1].pitch == 128);
        CHECK(mips[1].byte_size == 128 * 64 * 4);
        CHECK(mips[2].level == 2 && mips[2].width == 64 && mips[2].height == 32 && mips[2].pitch == 64);
        CHECK(mips[2].byte_size == 64 * 32 * 4);

        auto mips_small = calculate_texture_mipmap_levels(MetalPixelFormat::BGRA8Unorm, 4, 2, 4, 4);
        CHECK(mips_small.size() == 4);
        CHECK(mips_small[0].width == 4 && mips_small[0].height == 2);
        CHECK(mips_small[1].width == 2 && mips_small[1].height == 1);
        CHECK(mips_small[2].width == 1 && mips_small[2].height == 1);
        CHECK(mips_small[3].width == 1 && mips_small[3].height == 1);

        CHECK(calculate_texture_mipmap_levels(MetalPixelFormat::RGBA8Unorm, 0, 100, 0, 2).empty());
        CHECK(calculate_texture_mipmap_levels(MetalPixelFormat::RGBA8Unorm, 100, 0, 100, 2).empty());
        CHECK(calculate_texture_mipmap_levels(MetalPixelFormat::RGBA8Unorm, 100, 100, 100, 0).empty());

        CHECK(should_clamp_sampler_lod(4, 1) == true);
        CHECK(should_clamp_sampler_lod(4, 4) == false);
        CHECK(should_clamp_sampler_lod(1, 1) == false);
        CHECK(should_clamp_sampler_lod(0, 0) == false);

        // =====================================================================
        // 37. Teste T171: Validação de Recursos de Compute Dispatch
        // =====================================================================
        MetalDispatchCommand comp_cmd{};
        comp_cmd.valid = true;
        comp_cmd.pipeline_config.cs_hash = 0x12345;
        comp_cmd.pipeline_config.cs_msl = "kernel void cs_test() {}";
        comp_cmd.threadgroups_x = 1;
        comp_cmd.threadgroups_y = 1;
        comp_cmd.threadgroups_z = 1;
        comp_cmd.threads_per_threadgroup_x = 32;
        comp_cmd.threads_per_threadgroup_y = 1;
        comp_cmd.threads_per_threadgroup_z = 1;

        auto cr_res1 = validate_compute_resources(comp_cmd, false, false);
        CHECK(cr_res1.ok());

        comp_cmd.compute_bindings.active_buffers = 1;
        auto cr_res2 = validate_compute_resources(comp_cmd, false, false);
        CHECK(!cr_res2.ok());
        CHECK(cr_res2.error == BridgeError::PipelineConfigurationFailed);

        auto cr_res3 = validate_compute_resources(comp_cmd, false, true);
        CHECK(cr_res3.ok());

        comp_cmd.compute_bindings.active_textures = 1;
        auto cr_res4 = validate_compute_resources(comp_cmd, false, true);
        CHECK(!cr_res4.ok());

        auto cr_res5 = validate_compute_resources(comp_cmd, true, true);
        CHECK(cr_res5.ok());

        // =====================================================================
        // 38. Teste T158: Validações Estritas de Ponteiros e Precondições Guest
        // =====================================================================
        {
            CHECK(!validate_guest_buffer_read(nullptr, 1024));
            CHECK(!validate_guest_buffer_read(dummy_buf, 0));
            CHECK(validate_guest_buffer_read(dummy_buf, 512));

            CHECK(!validate_guest_texture_read(nullptr, 64, 64, 4));
            CHECK(!validate_guest_texture_read(dummy_buf, 0, 64, 4));
            CHECK(!validate_guest_texture_read(dummy_buf, 64, 0, 4));
            CHECK(!validate_guest_texture_read(dummy_buf, 64, 64, 0));
            CHECK(validate_guest_texture_read(dummy_buf, 64, 64, 4));

            CHECK(!validate_guest_index_read(nullptr, 100, GnmIndexType::Index16));
            CHECK(!validate_guest_index_read(dummy_buf, 0, GnmIndexType::Index16));
            CHECK(!validate_guest_index_read(dummy_buf, 100, static_cast<GnmIndexType>(77)));
            CHECK(validate_guest_index_read(dummy_buf, 100, GnmIndexType::Index16));
            CHECK(validate_guest_index_read(dummy_buf, 100, GnmIndexType::Index32));

            CHECK(!validate_draw_execution_preconditions(nullptr, 0));
            CHECK(!validate_draw_execution_preconditions(nullptr, 5));
            MetalDrawCommand prec_cmds[2]{};
            prec_cmds[0].valid = false;
            prec_cmds[1].valid = false;
            CHECK(!validate_draw_execution_preconditions(prec_cmds, 2));
            prec_cmds[1].valid = true;
            CHECK(validate_draw_execution_preconditions(prec_cmds, 2));
        }

        // =====================================================================
        // 39. Teste T159: Coerência de Cache, Tabelas Hash e Evicção de Recursos
        // =====================================================================
        {
            std::unordered_map<MetalBufferCacheKey, int, MetalBufferCacheKeyHash> buf_cache;
            MetalBufferCacheKey bk_a{0x1000, 512};
            MetalBufferCacheKey bk_b{0x1000, 1024};
            MetalBufferCacheKey bk_c{0x2000, 512};
            buf_cache[bk_a] = 10;
            buf_cache[bk_b] = 20;
            buf_cache[bk_c] = 30;
            CHECK(buf_cache.size() == 3);
            CHECK(buf_cache[bk_a] == 10);
            CHECK(buf_cache[bk_b] == 20);
            CHECK(buf_cache[bk_c] == 30);

            std::unordered_map<MetalTextureCacheKey, int, MetalTextureCacheKeyHash> tex_cache;
            MetalTextureCacheKey tk_base{0x5000, 128, 128, 128, MetalPixelFormat::RGBA8Unorm, 1};
            MetalTextureCacheKey tk_diff_fmt{0x5000, 128, 128, 128, MetalPixelFormat::BGRA8Unorm, 1};
            MetalTextureCacheKey tk_diff_size{0x5000, 256, 256, 256, MetalPixelFormat::RGBA8Unorm, 1};
            MetalTextureCacheKey tk_diff_mips{0x5000, 128, 128, 128, MetalPixelFormat::RGBA8Unorm, 4};
            tex_cache[tk_base] = 1;
            tex_cache[tk_diff_fmt] = 2;
            tex_cache[tk_diff_size] = 3;
            tex_cache[tk_diff_mips] = 4;
            CHECK(tex_cache.size() == 4);
            CHECK(tex_cache[tk_base] == 1);
            CHECK(tex_cache[tk_diff_fmt] == 2);
            CHECK(tex_cache[tk_diff_size] == 3);
            CHECK(tex_cache[tk_diff_mips] == 4);

            gnm_bridge_invalidate_buffer(0x0);
            gnm_bridge_invalidate_buffer(0x1000);
            gnm_bridge_invalidate_buffer(UINT64_MAX);
            gnm_bridge_invalidate_texture(0x0);
            gnm_bridge_invalidate_texture(0x5000);
            gnm_bridge_invalidate_texture(UINT64_MAX);
        }

        // =====================================================================
        // 40. Teste T160: Compatibilidade de Depth/Stencil e Agregação de Passes
        // =====================================================================
        {
            MetalDepthStencilConfig ds_combo{};
            ds_combo.depth_test_enabled = true;
            ds_combo.depth_pixel_format = MetalPixelFormat::Depth32Float_Stencil8;
            ds_combo.stencil_test_enabled = true;
            ds_combo.stencil_pixel_format = MetalPixelFormat::Depth32Float_Stencil8;
            CHECK(is_depth_stencil_attachment_compatible(ds_combo, 1920, 1080));

            MetalDepthStencilConfig ds_bad_depth = ds_combo;
            ds_bad_depth.depth_pixel_format = MetalPixelFormat::RGBA8Unorm;
            CHECK(!is_depth_stencil_attachment_compatible(ds_bad_depth, 1920, 1080));

            MetalDepthStencilConfig ds_bad_stencil = ds_combo;
            ds_bad_stencil.stencil_pixel_format = MetalPixelFormat::Depth16Unorm;
            CHECK(!is_depth_stencil_attachment_compatible(ds_bad_stencil, 1920, 1080));

            // Agregação de 4 comandos com estados mistos
            MetalDrawCommand agg_cmds[4]{};
            // 0: Depth pass (Depth32Float)
            agg_cmds[0].valid = true;
            agg_cmds[0].pipeline_config.depth_stencil.depth_test_enabled = true;
            agg_cmds[0].pipeline_config.depth_stencil.depth_write_enabled = true;
            agg_cmds[0].pipeline_config.depth_stencil.depth_pixel_format = MetalPixelFormat::Depth32Float;
            agg_cmds[0].pipeline_config.depth_stencil.depth_compare_function = MetalCompareFunction::Less;

            // 1: Stencil pass (Stencil8)
            agg_cmds[1].valid = true;
            agg_cmds[1].pipeline_config.depth_stencil.stencil_test_enabled = true;
            agg_cmds[1].pipeline_config.depth_stencil.stencil_write_enabled = true;
            agg_cmds[1].pipeline_config.depth_stencil.stencil_pixel_format = MetalPixelFormat::Stencil8;
            agg_cmds[1].pipeline_config.depth_stencil.stencil_compare_function = MetalCompareFunction::Equal;
            agg_cmds[1].pipeline_config.depth_stencil.stencil_read_mask = 0xAA;
            agg_cmds[1].pipeline_config.depth_stencil.stencil_write_mask = 0x55;

            // 2: Comando inválido -> deve ser ignorado
            agg_cmds[2].valid = false;
            agg_cmds[2].pipeline_config.depth_stencil.depth_test_enabled = true;
            agg_cmds[2].pipeline_config.depth_stencil.depth_pixel_format = MetalPixelFormat::Depth16Unorm;

            // 3: Pass comum apenas de cor
            agg_cmds[3].valid = true;

            auto agg_res = get_active_depth_stencil_config(agg_cmds, 4);
            CHECK(agg_res.depth_test_enabled);
            CHECK(agg_res.depth_write_enabled);
            CHECK(agg_res.depth_pixel_format == MetalPixelFormat::Depth32Float);
            CHECK(agg_res.depth_compare_function == MetalCompareFunction::Less);
            CHECK(agg_res.stencil_test_enabled);
            CHECK(agg_res.stencil_write_enabled);
            CHECK(agg_res.stencil_pixel_format == MetalPixelFormat::Stencil8);
            CHECK(agg_res.stencil_compare_function == MetalCompareFunction::Equal);
            CHECK(agg_res.stencil_read_mask == 0xAA);
            CHECK(agg_res.stencil_write_mask == 0x55);
        }

        // =====================================================================
        // 41. Teste T169: Mipmaps Não-Quadrados, Texturas Assimétricas e Clamping
        // =====================================================================
        {
            // Textura retangular larga 512x64 com 10 níveis de mipmap
            auto rect_mips = calculate_texture_mipmap_levels(MetalPixelFormat::RGBA8Unorm, 512, 64, 512, 10);
            CHECK(rect_mips.size() == 10);
            CHECK(rect_mips[0].width == 512 && rect_mips[0].height == 64);
            CHECK(rect_mips[1].width == 256 && rect_mips[1].height == 32);
            CHECK(rect_mips[2].width == 128 && rect_mips[2].height == 16);
            CHECK(rect_mips[3].width == 64 && rect_mips[3].height == 8);
            CHECK(rect_mips[4].width == 32 && rect_mips[4].height == 4);
            CHECK(rect_mips[5].width == 16 && rect_mips[5].height == 2);
            CHECK(rect_mips[6].width == 8 && rect_mips[6].height == 1);
            CHECK(rect_mips[7].width == 4 && rect_mips[7].height == 1);
            CHECK(rect_mips[8].width == 2 && rect_mips[8].height == 1);
            CHECK(rect_mips[9].width == 1 && rect_mips[9].height == 1);
            for (const auto& m : rect_mips) {
                CHECK(m.pitch >= m.width);
                CHECK(m.byte_size == static_cast<std::size_t>(m.pitch) * m.height * 4);
            }

            // Textura alta e fina 1x128
            auto tall_mips = calculate_texture_mipmap_levels(MetalPixelFormat::RGBA16Float, 1, 128, 1, 4);
            CHECK(tall_mips.size() == 4);
            CHECK(tall_mips[0].width == 1 && tall_mips[0].height == 128 && tall_mips[0].byte_size == 1 * 128 * 8);
            CHECK(tall_mips[1].width == 1 && tall_mips[1].height == 64 && tall_mips[1].byte_size == 1 * 64 * 8);
            CHECK(tall_mips[2].width == 1 && tall_mips[2].height == 32 && tall_mips[2].byte_size == 1 * 32 * 8);
            CHECK(tall_mips[3].width == 1 && tall_mips[3].height == 16 && tall_mips[3].byte_size == 1 * 16 * 8);

            // Textura com dimensões ímpares 3x5
            auto odd_mips = calculate_texture_mipmap_levels(MetalPixelFormat::Depth16Unorm, 3, 5, 3, 3);
            CHECK(odd_mips.size() == 3);
            CHECK(odd_mips[0].width == 3 && odd_mips[0].height == 5);
            CHECK(odd_mips[1].width == 1 && odd_mips[1].height == 2);
            CHECK(odd_mips[2].width == 1 && odd_mips[2].height == 1);

            // Testes estritos de clamp de sampler LOD
            CHECK(should_clamp_sampler_lod(8, 1) == true);
            CHECK(should_clamp_sampler_lod(8, 8) == false);
            CHECK(should_clamp_sampler_lod(2, 0) == true);
            CHECK(should_clamp_sampler_lod(1, 1) == false);
            CHECK(should_clamp_sampler_lod(0, 0) == false);
        }

        // =====================================================================
        // 42. Teste T171: Validação de Compute Dispatch, Limites e Recursos
        // =====================================================================
        {
            MetalDispatchCommand cd{};
            cd.valid = false;
            CHECK(!validate_compute_dispatch(cd).ok());
            CHECK(validate_compute_dispatch(cd).error == BridgeError::PipelineConfigurationFailed);

            cd.valid = true;
            cd.pipeline_config.cs_hash = 0;
            CHECK(!validate_compute_dispatch(cd).ok());
            CHECK(validate_compute_dispatch(cd).error == BridgeError::InvalidShaderAddress);

            cd.pipeline_config.cs_hash = 0xABCDE;
            cd.pipeline_config.cs_msl = "kernel void cs() {}";
            cd.threadgroups_x = 0; cd.threadgroups_y = 1; cd.threadgroups_z = 1;
            cd.threads_per_threadgroup_x = 8; cd.threads_per_threadgroup_y = 8; cd.threads_per_threadgroup_z = 1;
            CHECK(!validate_compute_dispatch(cd).ok());

            cd.threadgroups_x = 1;
            cd.threads_per_threadgroup_z = 0;
            CHECK(!validate_compute_dispatch(cd).ok());

            cd.threads_per_threadgroup_z = 1;
            CHECK(validate_compute_dispatch(cd).ok());

            // Validação de recursos de storage
            cd.compute_bindings.active_buffers = 2;
            CHECK(!validate_compute_resources(cd, false, false).ok());
            CHECK(!validate_compute_resources(cd, true, false).ok());
            CHECK(validate_compute_resources(cd, false, true).ok());

            cd.compute_bindings.active_textures = 1;
            CHECK(!validate_compute_resources(cd, false, true).ok());
            CHECK(validate_compute_resources(cd, true, true).ok());

            // Teste de process_dispatch via GnmMetalBridge
            GuestMemory comp_mem(1024 * 1024);
            GnmMetalBridge comp_bridge(comp_mem);
            DispatchCall dc_bad{};
            dc_bad.pipeline_state.cs_program_address = 0;
            auto pres1 = comp_bridge.process_dispatch(dc_bad);
            CHECK(!pres1.ok());
            CHECK(pres1.error == BridgeError::InvalidShaderAddress);
        }

        // =====================================================================
        // Todos os testes passaram
        // =====================================================================
    });
}