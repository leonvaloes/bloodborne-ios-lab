#include "check.h"
#include "flver_parser.h"
#include "vfs.h"
#include "binder.h"
#include <vector>
#include <string>
#include <cstring>

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
    });
}
