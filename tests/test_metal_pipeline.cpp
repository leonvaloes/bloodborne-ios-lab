#include "check.h"
#include "metal_pipeline.h"
#include <iostream>
#include <cmath>

int main() {
    // 1. Pixel Format and Depth/Stencil Capabilities
    CHECK(bb::MetalPipelineManager::is_hdr_format(bb::MetalPixelFormat::RGBA16Float));
    CHECK(bb::MetalPipelineManager::is_hdr_format(bb::MetalPixelFormat::RG11B10Float));
    CHECK(!bb::MetalPipelineManager::is_hdr_format(bb::MetalPixelFormat::BGRA8Unorm));

    CHECK(bb::MetalPipelineManager::is_depth_format(bb::MetalPixelFormat::Depth32Float));
    CHECK(bb::MetalPipelineManager::is_depth_format(bb::MetalPixelFormat::Depth32Float_Stencil8));
    CHECK(!bb::MetalPipelineManager::is_depth_format(bb::MetalPixelFormat::RGBA16Float));

    bb::DepthStencilStateDescriptor ds{};
    ds.depth_write_enabled = true;
    ds.depth_compare_function = bb::MetalCompareFunction::LessEqual;
    CHECK(ds.is_valid());

    bb::RenderPipelineConfig cfg{};
    cfg.color_format = bb::MetalPixelFormat::RGBA16Float;
    cfg.depth_format = bb::MetalPixelFormat::Depth32Float;
    cfg.sample_count = 1;
    std::string err;
    CHECK(cfg.validate(err));

    cfg.color_format = bb::MetalPixelFormat::Invalid;
    CHECK(!cfg.validate(err));

    // 2. Matrix 4x4 Mathematics
    auto id = bb::Matrix4x4::identity();
    CHECK(id.m[0] == 1.0f && id.m[5] == 1.0f && id.m[10] == 1.0f && id.m[15] == 1.0f);
    CHECK(id.m[1] == 0.0f && id.m[4] == 0.0f);

    auto proj = bb::Matrix4x4::perspective(1.047f, 16.0f / 9.0f, 0.1f, 100.0f);
    CHECK(proj.m[0] > 0.0f);
    CHECK(proj.m[5] > 0.0f);
    CHECK(proj.m[11] == 1.0f); // perspective W division term

    float eye[3] = {0.0f, 2.0f, 5.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    auto view = bb::Matrix4x4::look_at(eye, target, up);
    CHECK(view.m[15] == 1.0f);

    auto mvp = bb::Matrix4x4::multiply(proj, view);
    CHECK(mvp.m[15] != 0.0f || mvp.m[11] != 0.0f);

    auto roty = bb::Matrix4x4::rotation_y(1.5707963f);
    CHECK(std::abs(roty.m[0]) < 1e-4f); // cos(pi/2) ~ 0
    CHECK(std::abs(roty.m[2] + 1.0f) < 1e-4f); // -sin(pi/2) ~ -1

    auto norm_mat = bb::Matrix4x4::normal_matrix(view);
    CHECK(norm_mat.m[0] != 0.0f || norm_mat.m[5] != 0.0f);

    // 3. Metal 3 Tier 2 Argument Buffer Table (A17 Pro GPU)
    bb::Metal3ArgumentTable table{};
    table.bind_texture(0, 0x10008000ULL);
    table.bind_texture(15, 0x20008000ULL);
    CHECK(table.texture_gpu_addresses[0] == 0x10008000ULL);
    CHECK(table.texture_gpu_addresses[15] == 0x20008000ULL);

    table.bind_sampler(0, 0x5000ULL);
    CHECK(table.sampler_descriptors[0] == 0x5000ULL);

    table.bind_buffer(0, 0x80004000ULL); // 8-byte aligned
    table.bind_buffer(1, 0x80004008ULL); // 8-byte aligned
    CHECK(table.validate());

    table.bind_buffer(2, 0x80004003ULL); // Misaligned pointer!
    CHECK(!table.validate());

    std::cout << "All Metal pipeline tests passed successfully!" << std::endl;
    return 0;
}
