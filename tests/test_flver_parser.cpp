#include "check.h"
#include "flver_parser.h"
#include <iostream>

int main() {
    // 1. Magic and container validation
    CHECK(!bb::FlverParser::is_flver_data(nullptr, 0));
    CHECK(!bb::FlverParser::is_dcx_data(nullptr, 0));

    uint8_t bad_data[16] = {'N', 'O', 'T', 'F', 'L', 'V'};
    CHECK(!bb::FlverParser::is_flver_data(bad_data, sizeof(bad_data)));

    // 2. Synthetic Cube Model Parsing
    auto cube_bytes = bb::FlverParser::create_synthetic_flver_cube();
    CHECK(cube_bytes.size() >= 512);
    CHECK(bb::FlverParser::is_flver_data(cube_bytes.data(), cube_bytes.size()));

    bb::FlverModel cube_model{};
    std::string err;
    CHECK(bb::FlverParser::parse(cube_bytes.data(), cube_bytes.size(), cube_model, err));
    CHECK(cube_model.version == 0x20014);
    CHECK(cube_model.is_little_endian);
    CHECK(cube_model.submeshes.size() == 1);
    CHECK(cube_model.submeshes[0].vertices.size() == 8);
    CHECK(cube_model.submeshes[0].indices.size() == 36); // 12 triangles * 3
    CHECK(cube_model.total_vertex_count() == 8);
    CHECK(cube_model.total_triangle_count() == 12);

    // 3. Conversion to Metal Unified Memory Buffers
    auto metal_buf = bb::FlverParser::convert_to_metal_buffers(cube_model);
    CHECK(metal_buf.vertex_count == 8);
    CHECK(metal_buf.index_count == 36);
    CHECK(metal_buf.vertex_data.size() == 8 * 12); // 12 floats per vertex
    CHECK(metal_buf.index_data.size() == 36);
    CHECK(metal_buf.bounding_sphere[3] > 0.5f); // Non-zero radius

    // 4. Real Bloodborne Model Parsing (if present in local directory)
    const std::string real_flver_path = "C:/Users/Usuario/Documents/bloodborne_iphone/bloodborne_portPC/blodboorn 3171/Bloodborne™/Image0/dvdroot_ps4/map/m24_00_00_00/m24_00_00_00_900091.flver.dcx";
    bb::FlverModel real_model{};
    std::string real_err;
    if (bb::FlverParser::parse_file(real_flver_path, real_model, real_err)) {
        CHECK(real_model.version == 0x20014);
        CHECK(real_model.materials.size() == 6);
        CHECK(real_model.total_vertex_count() > 3000);
        CHECK(real_model.total_triangle_count() > 1000);

        auto real_metal_buf = bb::FlverParser::convert_to_metal_buffers(real_model);
        CHECK(real_metal_buf.vertex_count > 3000);
        CHECK(real_metal_buf.index_count > 5000);
        CHECK(real_metal_buf.bounding_sphere[3] > 10.0f);
        std::cout << "Real Bloodborne map model parsed successfully: "
                  << real_metal_buf.vertex_count << " vertices, "
                  << real_metal_buf.index_count / 3 << " triangles!" << std::endl;
    } else {
        std::cout << "Real model file not found or skipped; synthetic validation passed." << std::endl;
    }

    std::cout << "All FLVER parser tests passed successfully!" << std::endl;
    return 0;
}
