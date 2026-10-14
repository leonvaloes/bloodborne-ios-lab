#include "check.h"
#include "shader_translator.h"
#include <iostream>

int main() {
    // 1. SPIR-V Header Validation
    std::string err;
    CHECK(!bb::ShaderTranslator::validate_spirv_header(nullptr, 0, err));
    CHECK(!err.empty());

    uint32_t bad_magic[5] = {0x12345678, 0x00010000, 0, 10, 0};
    CHECK(!bb::ShaderTranslator::validate_spirv_header(bad_magic, 5, err));

    uint32_t valid_hdr[5] = {bb::spirv::MagicNumber, 0x00010300, 0x1234, 20, 0};
    CHECK(bb::ShaderTranslator::validate_spirv_header(valid_hdr, 5, err));

    // 2. Vertex Shader Bytecode Generation & Translation
    auto vert_spv = bb::ShaderTranslator::build_basic_vertex_spirv();
    CHECK(vert_spv.size() > 10);
    CHECK(bb::ShaderTranslator::validate_spirv_header(vert_spv.data(), vert_spv.size(), err));

    auto vert_refl = bb::ShaderTranslator::reflect(vert_spv.data(), vert_spv.size());
    CHECK(vert_refl.stage == bb::ShaderStage::Vertex);
    CHECK(vert_refl.entry_point_name == "vertex_main");
    CHECK(vert_refl.vertex_inputs.size() >= 3);
    CHECK(vert_refl.vertex_inputs[0].location == 0);
    CHECK(vert_refl.vertex_inputs[1].location == 1);
    CHECK(vert_refl.vertex_inputs[2].location == 2);

    bb::TranslationOptions vert_opts{};
    vert_opts.msl_version = 30000;
    vert_opts.flip_vertex_y = true;
    vert_opts.enable_argument_buffers = true;

    auto vert_res = bb::ShaderTranslator::translate_spirv_to_msl(vert_spv, vert_opts);
    CHECK(vert_res.success);
    CHECK(!vert_res.msl_source.empty());
    CHECK(vert_res.msl_source.find("#include <metal_stdlib>") != std::string::npos);
    CHECK(vert_res.msl_source.find("vertex VertexOutput vertex_main") != std::string::npos);
    CHECK(vert_res.msl_source.find("[[stage_in]]") != std::string::npos);
    CHECK(vert_res.msl_source.find("[[position]]") != std::string::npos);
    CHECK(vert_res.msl_source.find("struct VertexArgumentTable") != std::string::npos);
    CHECK(vert_res.msl_source.find("out.position.y = -out.position.y") != std::string::npos);

    // 3. Fragment Shader Bytecode Generation & Translation
    auto frag_spv = bb::ShaderTranslator::build_basic_fragment_spirv();
    CHECK(frag_spv.size() > 10);
    auto frag_refl = bb::ShaderTranslator::reflect(frag_spv.data(), frag_spv.size());
    CHECK(frag_refl.stage == bb::ShaderStage::Fragment);
    CHECK(frag_refl.entry_point_name == "fragment_main");

    bb::TranslationOptions frag_opts{};
    frag_opts.enable_argument_buffers = true;
    frag_opts.argument_buffer_index = 0;
    frag_opts.default_uniform_buffer_index = 1;

    auto frag_res = bb::ShaderTranslator::translate_spirv_to_msl(frag_spv, frag_opts);
    CHECK(frag_res.success);
    CHECK(frag_res.msl_source.find("fragment FragmentOutput fragment_main") != std::string::npos);
    CHECK(frag_res.msl_source.find("struct FragmentArgumentTable") != std::string::npos);
    CHECK(frag_res.msl_source.find("texture2d<float> textures[16]") != std::string::npos);
    CHECK(frag_res.msl_source.find("sampler          samplers[4]") != std::string::npos);
    CHECK(frag_res.msl_source.find("NdotL") != std::string::npos);
    CHECK(frag_res.msl_source.find("diffuseTex.sample") != std::string::npos);

    std::cout << "All shader translator tests passed successfully!" << std::endl;
    return 0;
}
