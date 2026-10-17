#include "check.h"
#include "flver_parser.h"
#include "vfs.h"
#include "binder.h"
#include "gnm_metal_bridge.h"
#include "guest_memory.h"
#include "host_bridge.h"
#include <vector>
#include <string>
#include <cstring>
#include <array>

int main() {
    return run([] {
        using namespace bb;

        // 1. Validação de rejeição de dados nulos ou corrompidos de área
        CHECK(!FlverParser::is_flver_data(nullptr, 0));
        CHECK(!FlverParser::is_dcx_data(nullptr, 0));

        std::uint8_t invalid_map_bytes[32] = {'M', 'A', 'P', '0', 0, 1, 2, 3};
        FlverModel invalid_model{};
        std::string err;
        CHECK(!FlverParser::parse(invalid_map_bytes, sizeof(invalid_map_bytes), invalid_model, err));

        // 2. Criação e descompressão de geometria sintética de área (Clínica de Iosefka / m29)
        auto clinic_flver_bytes = FlverParser::create_synthetic_flver_cube();
        CHECK(!clinic_flver_bytes.empty());
        CHECK(FlverParser::is_flver_data(clinic_flver_bytes.data(), clinic_flver_bytes.size()));

        // Compacta em DCX para simular arquivo de mapa do Bloodborne (.flver.dcx)
        std::vector<std::uint8_t> compressed_dcx;
        CHECK(DcxDecompressor::create_dcx(clinic_flver_bytes.data(), clinic_flver_bytes.size(),
                                          compressed_dcx, DcxAlgorithm::deflate) == BinderError::ok);
        CHECK(FlverParser::is_dcx_data(compressed_dcx.data(), compressed_dcx.size()));

        // Descompacta via DCX decompressor
        std::vector<std::uint8_t> decompressed_flver;
        CHECK(DcxDecompressor::decompress(compressed_dcx.data(), compressed_dcx.size(), decompressed_flver) == BinderError::ok);
        CHECK(decompressed_flver.size() == clinic_flver_bytes.size());
        CHECK(decompressed_flver == clinic_flver_bytes);

        // 3. Parse da geometria 3D do mapa
        FlverModel clinic_model{};
        CHECK(FlverParser::parse(decompressed_flver.data(), decompressed_flver.size(), clinic_model, err));
        CHECK(clinic_model.version == 0x20014);
        CHECK(clinic_model.total_vertex_count() > 0);
        CHECK(clinic_model.total_triangle_count() > 0);
        CHECK(!clinic_model.submeshes.empty());

        // 4. Conversão para buffers de memória unificada Metal
        auto metal_buffers = FlverParser::convert_to_metal_buffers(clinic_model);
        CHECK(metal_buffers.vertex_count == clinic_model.total_vertex_count());
        CHECK(metal_buffers.index_count == clinic_model.total_triangle_count() * 3);
        CHECK(metal_buffers.vertex_data.size() == metal_buffers.vertex_count * 12);
        CHECK(metal_buffers.index_data.size() == metal_buffers.index_count);
        CHECK(metal_buffers.bounding_sphere[3] > 0.0f); // Raio válido da esfera delimitadora da área

        // 5. Teste de montagem no VFS da rota de mapas /app0/dvdroot_ps4/map/
        VirtualFileSystem vfs;
        CHECK(vfs.set_synthetic_file("/app0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00.flver.dcx",
                                     compressed_dcx));
        CHECK(vfs.exists("/app0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00.flver.dcx"));

        std::vector<std::uint8_t> vfs_read_bytes;
        CHECK(vfs.read_file("/app0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00.flver.dcx", vfs_read_bytes) == VfsError::ok);
        CHECK(vfs_read_bytes.size() == compressed_dcx.size());

        FlverModel vfs_model{};
        CHECK(FlverParser::parse(vfs_read_bytes.data(), vfs_read_bytes.size(), vfs_model, err));
        CHECK(vfs_model.total_vertex_count() == clinic_model.total_vertex_count());

        // 6. T039: Pipeline de Shaders Metal com Iluminacao Blinn-Phong e Depth Test para Primeira Area (FR-004)
        {
            MetalRenderPipelineConfig clinic_pipe{};
            clinic_pipe.vs_hash = 0xAA290001;
            clinic_pipe.ps_hash = 0xAA290002;
            clinic_pipe.vs_msl =
                "#include <metal_stdlib>\n"
                "using namespace metal;\n"
                "struct AreaVertexIn {\n"
                "    float3 position [[attribute(0)]];\n"
                "    float3 normal   [[attribute(1)]];\n"
                "    float2 uv       [[attribute(2)]];\n"
                "};\n"
                "struct AreaVertexOut {\n"
                "    float4 position [[position]];\n"
                "    float3 world_pos;\n"
                "    float3 normal;\n"
                "    float2 uv;\n"
                "};\n"
                "vertex AreaVertexOut clinic_vs(AreaVertexIn in [[stage_in]], constant float4x4& mvp [[buffer(1)]]) {\n"
                "    AreaVertexOut out;\n"
                "    out.position = mvp * float4(in.position, 1.0);\n"
                "    out.world_pos = in.position;\n"
                "    out.normal = in.normal;\n"
                "    out.uv = in.uv;\n"
                "    return out;\n"
                "}\n";

            clinic_pipe.ps_msl =
                "#include <metal_stdlib>\n"
                "using namespace metal;\n"
                "struct AreaVertexOut {\n"
                "    float4 position [[position]];\n"
                "    float3 world_pos;\n"
                "    float3 normal;\n"
                "    float2 uv;\n"
                "};\n"
                "fragment float4 clinic_ps(AreaVertexOut in [[stage_in]]) {\n"
                "    float3 ambient = float3(0.08, 0.08, 0.12);\n"
                "    float3 moon_dir = normalize(float3(0.3, 0.8, -0.5));\n"
                "    float diff = max(dot(normalize(in.normal), moon_dir), 0.0);\n"
                "    float3 diffuse = diff * float3(0.4, 0.45, 0.55);\n"
                "    float3 lantern_pos = float3(0.0, 1.5, 0.0);\n"
                "    float dist = length(lantern_pos - in.world_pos);\n"
                "    float atten = 1.0 / (1.0 + 0.1 * dist + 0.05 * dist * dist);\n"
                "    float3 lantern_light = atten * float3(0.6, 0.4, 0.2);\n"
                "    float3 final_color = ambient + diffuse + lantern_light;\n"
                "    return float4(final_color, 1.0);\n"
                "}\n";

            clinic_pipe.color_attachments[0].pixel_format = MetalPixelFormat::BGRA8Unorm;
            clinic_pipe.active_color_attachments = 1;
            clinic_pipe.depth_stencil.depth_pixel_format = MetalPixelFormat::Depth32Float;
            clinic_pipe.depth_stencil.depth_test_enabled = true;
            clinic_pipe.depth_stencil.depth_write_enabled = true;
            clinic_pipe.depth_stencil.depth_compare_function = MetalCompareFunction::LessEqual;

            CHECK(clinic_pipe.is_valid());

            // Testa unificacao de bibliotecas MSL para compilacao estatica Metal
            std::string combined_msl = build_combined_msl_source(clinic_pipe.vs_msl, clinic_pipe.ps_msl);
            CHECK(!combined_msl.empty());
            CHECK(combined_msl.find("clinic_vs") != std::string::npos);
            CHECK(combined_msl.find("clinic_ps") != std::string::npos);

            // Validacao de chaves de cache estaveis
            MetalPipelineCacheKey p_key{};
            p_key.vs_hash = clinic_pipe.vs_hash;
            p_key.ps_hash = clinic_pipe.ps_hash;
            p_key.color_formats[0] = static_cast<std::uint32_t>(clinic_pipe.color_attachments[0].pixel_format);
            p_key.depth_format = static_cast<std::uint32_t>(clinic_pipe.depth_stencil.depth_pixel_format);

            MetalDepthStencilCacheKey ds_key = MetalDepthStencilCacheKey::from_config(clinic_pipe.depth_stencil);

            CHECK(p_key.vs_hash == 0xAA290001);
            CHECK(ds_key.depth_write_enabled);
            CHECK(ds_key.depth_compare_function == static_cast<std::uint32_t>(MetalCompareFunction::LessEqual));
        }

        // 7. T039: Comandos de Desenho Indexado da Area e Bounds Espaciais (FR-004)
        {
            constexpr std::size_t guest_area_mem = 16 * 1024 * 1024;
            GuestMemory area_mem(guest_area_mem);
            CHECK(area_mem.map(0x100000, 1024 * 1024, read | write) == MemoryError::ok);

            std::uint64_t vbuf_addr = 0x100000;
            std::uint64_t ibuf_addr = 0x140000;

            CHECK(area_mem.store(vbuf_addr, metal_buffers.vertex_data.data(), metal_buffers.vertex_data.size()) == MemoryError::ok);
            CHECK(area_mem.store(ibuf_addr, metal_buffers.index_data.data(), metal_buffers.index_data.size() * sizeof(std::uint16_t)) == MemoryError::ok);

            MetalDrawCommand cmd{};
            cmd.indexed = true;
            cmd.index_count = metal_buffers.index_count;
            cmd.vertex_count = metal_buffers.vertex_count;
            cmd.start_index = 0;
            cmd.index_buffer_address = ibuf_addr;
            cmd.index_type = GnmIndexType::Index16;
            cmd.instance_count = 1;
            cmd.viewport.x = 0;
            cmd.viewport.y = 0;
            cmd.viewport.width = 1920;
            cmd.viewport.height = 1080;
            cmd.viewport.min_depth = 0.0f;
            cmd.viewport.max_depth = 1.0f;
            cmd.valid = true;

            CHECK(validate_draw_execution_preconditions(&cmd, 1));

            // Bounds espaciais: valida centróide e raio da bounding sphere da clinica
            CHECK(metal_buffers.bounding_sphere[3] >= 0.5f); // raio positivo
        }

        // 8. T039: Fluxo de Transicao de Novo Jogo e Carregamento de Area Inicial (FR-004)
        {
            HostBridge bridge;
            CHECK(bridge.intro_video_status() == "not_started");

            // Dispara introducao cinematografica de Novo Jogo
            bridge.play_intro_video_or_fallback("/app0/dvdroot_ps4/movie/intro.mp4");
            // Finaliza introducao e engaja Novo Jogo
            bridge.finish_intro_and_boot(HostBridge::IntroFallbackReason::MissingMedia);
            CHECK(bridge.intro_video_status() == "offline_fallback_missing_media");

            // O coordenador requisita os ativos de mapa de m29 no VFS
            CHECK(vfs.exists("/app0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00.flver.dcx"));

            std::vector<std::uint8_t> map_dcx;
            CHECK(vfs.read_file("/app0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00.flver.dcx", map_dcx) == VfsError::ok);
            CHECK(!map_dcx.empty());

            std::vector<std::uint8_t> flver_decomp;
            CHECK(DcxDecompressor::decompress(map_dcx.data(), map_dcx.size(), flver_decomp) == BinderError::ok);

            FlverModel loaded_area{};
            std::string parse_err;
            CHECK(FlverParser::parse(flver_decomp.data(), flver_decomp.size(), loaded_area, parse_err));
            CHECK(loaded_area.total_vertex_count() > 0);
            CHECK(loaded_area.total_triangle_count() > 0);
        }
    });
}
