#include "gnm_context.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace bb {

namespace {
inline float uint_as_float(std::uint32_t val) {
    float f = 0.0f;
    std::memcpy(&f, &val, sizeof(float));
    return f;
}

inline std::uint32_t float_as_uint(float val) {
    std::uint32_t u = 0;
    std::memcpy(&u, &val, sizeof(std::uint32_t));
    return u;
}
} // namespace

GnmContext::GnmContext(const GuestMemory& memory)
    : memory_(memory) {
    reset();
}

void GnmContext::reset() {
    context_regs_.fill(0);
    sh_regs_graphics_.fill(0);
    sh_regs_compute_.fill(0);
    uconfig_regs_.fill(0);

    pipeline_state_ = PipelineState{};
    compute_pipeline_state_ = ComputePipelineState{};
    draw_calls_.clear();
    dispatch_calls_.clear();

    // Configuração inicial padrão segura
    pipeline_state_.primitive_type = GnmPrimitiveType::TriList;
    pipeline_state_.rasterizer.cull_mode = GnmCullMode::Back;
    pipeline_state_.rasterizer.front_face = GnmFrontFace::Clockwise;
    pipeline_state_.rasterizer.line_width = 1.0f;
    pipeline_state_.rasterizer.point_size = 1.0f;

    for (auto& vp : pipeline_state_.viewports.viewports) {
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;
    }

    // Default target mask: todos os 8 canais habilitados
    pipeline_state_.render_targets.target_mask = 0xFF;
    pipeline_state_.blend.target_mask = 0xFF;
}

void GnmContext::on_pm4_event(const Pm4Event& event) {
    switch (event.kind) {
    case Pm4Kind::draw_indexed: {
        DrawCall dc{};
        dc.indexed = true;
        dc.index_count = event.args[0];
        dc.vertex_count = event.args[0];
        // Address 64-bit do index buffer construído a partir de args[1] e args[2]
        dc.index_buffer_address = (static_cast<std::uint64_t>(event.args[2] & 0xFFFF) << 32) | event.args[1];
        if (dc.index_buffer_address == 0 && event.address != 0) {
            dc.index_buffer_address = event.address;
        }
        dc.start_index = event.args[3];
        const bool is_32bit = (event.args.size() > 4 && event.args[4] > 0xFFFF) || ((event.args[1] & 2) != 0);
        dc.index_type = is_32bit ? GnmIndexType::Index32 : GnmIndexType::Index16;
        dc.instance_count = 1;
        dc.start_instance = 0;
        dc.pipeline_state = pipeline_state_; // Snapshot do estado do pipeline
        draw_calls_.push_back(dc);
        break;
    }
    case Pm4Kind::draw_auto: {
        DrawCall dc{};
        dc.indexed = false;
        dc.vertex_count = event.args[0];
        dc.index_count = 0;
        dc.instance_count = 1;
        dc.start_instance = 0;
        if (event.args[1] != 0) {
            const auto prim = event.args[1] & 0x3F;
            if (prim >= 1 && prim <= 14) {
                pipeline_state_.primitive_type = static_cast<GnmPrimitiveType>(prim);
            }
        }
        dc.pipeline_state = pipeline_state_; // Snapshot do estado do pipeline
        draw_calls_.push_back(dc);
        break;
    }
    case Pm4Kind::dispatch: {
        DispatchCall dc{};
        dc.threadgroups_x = event.args[0];
        dc.threadgroups_y = event.args[1];
        dc.threadgroups_z = event.args[2];
        dc.pipeline_state = compute_pipeline_state_; // Snapshot do compute pipeline
        dispatch_calls_.push_back(dc);
        break;
    }
    default:
        break;
    }
}

void GnmContext::on_context_reg(std::uint32_t offset, std::uint32_t value) {
    if (offset < context_regs_.size()) {
        context_regs_[offset] = value;
        update_context_register(offset, value);
    }
}

void GnmContext::on_sh_reg(std::uint32_t offset, std::uint32_t value, bool compute) {
    if (compute) {
        if (offset < sh_regs_compute_.size()) {
            sh_regs_compute_[offset] = value;
            update_sh_register(offset, value, true);
        }
    } else {
        if (offset < sh_regs_graphics_.size()) {
            sh_regs_graphics_[offset] = value;
            update_sh_register(offset, value, false);
        }
    }
}

void GnmContext::on_uconfig_reg(std::uint32_t offset, std::uint32_t value) {
    if (offset < uconfig_regs_.size()) {
        uconfig_regs_[offset] = value;
    }
}

Pm4Status GnmContext::submit(std::uint64_t address, std::uint32_t words, const Pm4Limits& limits) {
    GnmPm4 parser(memory_, *this, limits);
    return parser.process(address, words);
}

void GnmContext::set_context_reg(std::uint32_t offset, std::uint32_t value) {
    on_context_reg(offset, value);
}

void GnmContext::set_sh_reg(std::uint32_t offset, std::uint32_t value, bool compute) {
    on_sh_reg(offset, value, compute);
}

void GnmContext::set_uconfig_reg(std::uint32_t offset, std::uint32_t value) {
    on_uconfig_reg(offset, value);
}

std::uint32_t GnmContext::context_reg(std::uint32_t offset) const {
    return offset < context_regs_.size() ? context_regs_[offset] : 0;
}

std::uint32_t GnmContext::sh_reg(std::uint32_t offset, bool compute) const {
    const auto& regs = compute ? sh_regs_compute_ : sh_regs_graphics_;
    return offset < regs.size() ? regs[offset] : 0;
}

std::uint32_t GnmContext::uconfig_reg(std::uint32_t offset) const {
    return offset < uconfig_regs_.size() ? uconfig_regs_[offset] : 0;
}

const Viewport& GnmContext::current_viewport(std::uint32_t slot) const {
    return slot < pipeline_state_.viewports.viewports.size()
               ? pipeline_state_.viewports.viewports[slot]
               : pipeline_state_.viewports.viewports[0];
}

const ScissorRect& GnmContext::current_scissor(std::uint32_t slot) const {
    return slot < pipeline_state_.scissors.scissors.size()
               ? pipeline_state_.scissors.scissors[slot]
               : pipeline_state_.scissors.scissors[0];
}

std::uint32_t GnmContext::user_data(ShaderStage stage, std::uint32_t slot) const {
    if (slot >= 16) return 0;
    switch (stage) {
    case ShaderStage::Vertex: return pipeline_state_.vs_user_data[slot];
    case ShaderStage::Pixel: return pipeline_state_.ps_user_data[slot];
    case ShaderStage::Compute: return compute_pipeline_state_.cs_user_data[slot];
    default: return 0;
    }
}

void GnmContext::set_vs_program(std::uint64_t address) {
    pipeline_state_.vs_program_address = address;
    sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_LO_VS] = static_cast<std::uint32_t>(address & 0xFFFFFFFF);
    sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_HI_VS] = static_cast<std::uint32_t>((address >> 32) & 0xFFFFFFFF);
}

void GnmContext::set_ps_program(std::uint64_t address) {
    pipeline_state_.ps_program_address = address;
    sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_LO_PS] = static_cast<std::uint32_t>(address & 0xFFFFFFFF);
    sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_HI_PS] = static_cast<std::uint32_t>((address >> 32) & 0xFFFFFFFF);
}

void GnmContext::set_cs_program(std::uint64_t address) {
    compute_pipeline_state_.cs_program_address = address;
    sh_regs_compute_[gcn_reg::COMPUTE_PGM_LO] = static_cast<std::uint32_t>(address & 0xFFFFFFFF);
    sh_regs_compute_[gcn_reg::COMPUTE_PGM_HI] = static_cast<std::uint32_t>((address >> 32) & 0xFFFFFFFF);
}

void GnmContext::set_user_data(ShaderStage stage, std::uint32_t slot, std::uint32_t value) {
    if (slot >= 16) return;
    switch (stage) {
    case ShaderStage::Vertex:
        pipeline_state_.vs_user_data[slot] = value;
        sh_regs_graphics_[gcn_reg::SPI_SHADER_USER_DATA_VS_0 + slot] = value;
        break;
    case ShaderStage::Pixel:
        pipeline_state_.ps_user_data[slot] = value;
        sh_regs_graphics_[gcn_reg::SPI_SHADER_USER_DATA_PS_0 + slot] = value;
        break;
    case ShaderStage::Compute:
        compute_pipeline_state_.cs_user_data[slot] = value;
        sh_regs_compute_[gcn_reg::COMPUTE_USER_DATA_0 + slot] = value;
        break;
    }
}

const DrawCall& GnmContext::current_draw_call() const {
    return draw_calls_.empty() ? empty_draw_ : draw_calls_.back();
}

const DispatchCall& GnmContext::current_dispatch_call() const {
    return dispatch_calls_.empty() ? empty_dispatch_ : dispatch_calls_.back();
}

const DrawCall* GnmContext::last_draw_call() const {
    return draw_calls_.empty() ? nullptr : &draw_calls_.back();
}

const DispatchCall* GnmContext::last_dispatch_call() const {
    return dispatch_calls_.empty() ? nullptr : &dispatch_calls_.back();
}

void GnmContext::clear_calls() {
    draw_calls_.clear();
    dispatch_calls_.clear();
}

// =============================================================================
// Atualização de Registradores de Contexto e SH
// =============================================================================

void GnmContext::update_context_register(std::uint32_t offset, std::uint32_t value) {
    // Render Targets 0..7 (base 0x318, stride 0x14)
    if (offset >= gcn_reg::CB_COLOR0_BASE &&
        offset < gcn_reg::CB_COLOR0_BASE + 8 * gcn_reg::CB_COLOR_STRIDE) {
        const auto rt_idx = (offset - gcn_reg::CB_COLOR0_BASE) / gcn_reg::CB_COLOR_STRIDE;
        if (rt_idx < 8) {
            update_render_target(rt_idx);
        }
        return;
    }

    if (offset == gcn_reg::CB_TARGET_MASK) {
        pipeline_state_.render_targets.target_mask = static_cast<std::uint8_t>(value & 0xFF);
        pipeline_state_.blend.target_mask = static_cast<std::uint8_t>(value & 0xFF);
        return;
    }

    // Depth / Stencil
    if (offset == gcn_reg::DB_DEPTH_CONTROL ||
        (offset >= gcn_reg::DB_Z_INFO && offset <= gcn_reg::DB_HTILE_DATA_BASE)) {
        update_depth_stencil();
        return;
    }

    // Viewport 0..15 (base 0x10F, stride 6)
    if (offset >= gcn_reg::PA_CL_VPORT_XSCALE &&
        offset < gcn_reg::PA_CL_VPORT_XSCALE + 16 * 6) {
        const auto vp_idx = (offset - gcn_reg::PA_CL_VPORT_XSCALE) / 6;
        if (vp_idx < 16) {
            update_viewport(vp_idx);
        }
        return;
    }

    // Window Scissor
    if (offset == gcn_reg::PA_SC_WINDOW_SCISSOR_TL || offset == gcn_reg::PA_SC_WINDOW_SCISSOR_BR) {
        const auto tl = context_regs_[gcn_reg::PA_SC_WINDOW_SCISSOR_TL];
        const auto br = context_regs_[gcn_reg::PA_SC_WINDOW_SCISSOR_BR];
        pipeline_state_.scissors.window_scissor.left = tl & 0x7FFF;
        pipeline_state_.scissors.window_scissor.top = (tl >> 16) & 0x7FFF;
        pipeline_state_.scissors.window_scissor.right = br & 0x7FFF;
        pipeline_state_.scissors.window_scissor.bottom = (br >> 16) & 0x7FFF;
        return;
    }

    // Screen Scissor
    if (offset == gcn_reg::PA_SC_SCREEN_SCISSOR_TL || offset == gcn_reg::PA_SC_SCREEN_SCISSOR_BR) {
        const auto tl = context_regs_[gcn_reg::PA_SC_SCREEN_SCISSOR_TL];
        const auto br = context_regs_[gcn_reg::PA_SC_SCREEN_SCISSOR_BR];
        pipeline_state_.scissors.screen_scissor.left = tl & 0x7FFF;
        pipeline_state_.scissors.screen_scissor.top = (tl >> 16) & 0x7FFF;
        pipeline_state_.scissors.screen_scissor.right = br & 0x7FFF;
        pipeline_state_.scissors.screen_scissor.bottom = (br >> 16) & 0x7FFF;
        return;
    }

    // Viewport Scissor 0..15 (base 0x07A, stride 2)
    if (offset >= gcn_reg::PA_SC_VPORT_SCISSOR_0_TL &&
        offset < gcn_reg::PA_SC_VPORT_SCISSOR_0_TL + 16 * 2) {
        const auto sc_idx = (offset - gcn_reg::PA_SC_VPORT_SCISSOR_0_TL) / 2;
        if (sc_idx < 16) {
            update_scissor(sc_idx);
        }
        return;
    }

    // Blend Control 0..7
    if (offset >= gcn_reg::CB_BLEND0_CONTROL && offset < gcn_reg::CB_BLEND0_CONTROL + 8) {
        const auto b_idx = offset - gcn_reg::CB_BLEND0_CONTROL;
        update_blend_target(b_idx);
        return;
    }

    // Blend Color RGBA (float)
    if (offset >= gcn_reg::CB_BLEND_RED && offset <= gcn_reg::CB_BLEND_ALPHA) {
        pipeline_state_.blend.blend_color[offset - gcn_reg::CB_BLEND_RED] = uint_as_float(value);
        return;
    }

    // Rasterizer / Primitive Mode
    if (offset == gcn_reg::PA_SU_SC_MODE_CNTL ||
        offset == gcn_reg::PA_SU_LINE_CNTL ||
        offset == gcn_reg::PA_SU_POINT_SIZE ||
        offset == gcn_reg::PA_SU_POLY_OFFSET_CLAMP ||
        offset == gcn_reg::PA_SU_POLY_OFFSET_FRONT_SCALE ||
        offset == gcn_reg::PA_SU_POLY_OFFSET_FRONT_OFFSET) {
        update_rasterizer();
        return;
    }

    // Primitive Type
    if (offset == gcn_reg::VGT_PRIMITIVE_TYPE) {
        const auto prim = value & 0x3F;
        if (prim >= 1 && prim <= 14) {
            pipeline_state_.primitive_type = static_cast<GnmPrimitiveType>(prim);
        }
        return;
    }
}

void GnmContext::update_sh_register(std::uint32_t offset, std::uint32_t value, bool compute) {
    if (compute) {
        if (offset == gcn_reg::COMPUTE_PGM_LO || offset == gcn_reg::COMPUTE_PGM_HI) {
            const auto lo = sh_regs_compute_[gcn_reg::COMPUTE_PGM_LO];
            const auto hi = sh_regs_compute_[gcn_reg::COMPUTE_PGM_HI];
            compute_pipeline_state_.cs_program_address = (static_cast<std::uint64_t>(hi) << 32) | lo;
            return;
        }
        if (offset >= gcn_reg::COMPUTE_USER_DATA_0 && offset < gcn_reg::COMPUTE_USER_DATA_0 + 16) {
            const auto slot = offset - gcn_reg::COMPUTE_USER_DATA_0;
            compute_pipeline_state_.cs_user_data[slot] = value;
            return;
        }
        if (offset >= gcn_reg::COMPUTE_USER_DATA_ALT_0 && offset < gcn_reg::COMPUTE_USER_DATA_ALT_0 + 16) {
            const auto slot = offset - gcn_reg::COMPUTE_USER_DATA_ALT_0;
            compute_pipeline_state_.cs_user_data[slot] = value;
            return;
        }
    } else {
        if (offset == gcn_reg::SPI_SHADER_PGM_LO_VS || offset == gcn_reg::SPI_SHADER_PGM_HI_VS) {
            const auto lo = sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_LO_VS];
            const auto hi = sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_HI_VS];
            pipeline_state_.vs_program_address = (static_cast<std::uint64_t>(hi) << 32) | lo;
            return;
        }
        if (offset == gcn_reg::SPI_SHADER_PGM_LO_PS || offset == gcn_reg::SPI_SHADER_PGM_HI_PS) {
            const auto lo = sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_LO_PS];
            const auto hi = sh_regs_graphics_[gcn_reg::SPI_SHADER_PGM_HI_PS];
            pipeline_state_.ps_program_address = (static_cast<std::uint64_t>(hi) << 32) | lo;
            return;
        }
        if (offset >= gcn_reg::SPI_SHADER_USER_DATA_VS_0 && offset < gcn_reg::SPI_SHADER_USER_DATA_VS_0 + 16) {
            const auto slot = offset - gcn_reg::SPI_SHADER_USER_DATA_VS_0;
            pipeline_state_.vs_user_data[slot] = value;
            return;
        }
        if (offset >= gcn_reg::SPI_SHADER_USER_DATA_PS_0 && offset < gcn_reg::SPI_SHADER_USER_DATA_PS_0 + 16) {
            const auto slot = offset - gcn_reg::SPI_SHADER_USER_DATA_PS_0;
            pipeline_state_.ps_user_data[slot] = value;
            return;
        }
    }
}

void GnmContext::update_render_target(std::uint32_t index) {
    if (index >= 8) return;
    const auto base_offset = gcn_reg::CB_COLOR0_BASE + index * gcn_reg::CB_COLOR_STRIDE;
    const auto reg_base   = context_regs_[base_offset + 0];
    const auto reg_pitch  = context_regs_[base_offset + 1];
    const auto reg_slice  = context_regs_[base_offset + 2];
    const auto reg_info   = context_regs_[base_offset + 4];
    const auto reg_dim    = context_regs_[base_offset + 6];
    const auto reg_cmask  = context_regs_[base_offset + 7];
    const auto reg_fmask  = context_regs_[base_offset + 9];

    auto& rt = pipeline_state_.render_targets.targets[index];
    // GCN 256-byte aligned base address: BASE_256B (base >> 8)
    rt.base_address = static_cast<std::uint64_t>(reg_base) << 8;
    rt.pitch = reg_pitch & 0x3FFF;
    rt.slice_tile_max = reg_slice & 0x3FFFFF;
    rt.width = (reg_dim & 0x3FFF) + 1;
    rt.height = ((reg_dim >> 14) & 0x3FFF) + 1;
    rt.cmask_address = static_cast<std::uint64_t>(reg_cmask) << 8;
    rt.fmask_address = static_cast<std::uint64_t>(reg_fmask) << 8;

    const auto fmt_id = (reg_info >> 2) & 0xF;
    rt.enabled = (fmt_id != 0);

    switch (fmt_id) {
    case 1: rt.format = GnmSurfaceFormat::R8_Unorm; break;
    case 2: rt.format = GnmSurfaceFormat::R16_Float; break;
    case 3: rt.format = GnmSurfaceFormat::R8G8_Unorm; break;
    case 4: rt.format = GnmSurfaceFormat::R32_Float; break;
    case 5: rt.format = GnmSurfaceFormat::R16G16_Float; break;
    case 6: rt.format = GnmSurfaceFormat::R11G11B10_Float; break;
    case 7: {
        const auto swap = (reg_info >> 6) & 0x3;
        rt.format = (swap == 1 || swap == 2) ? GnmSurfaceFormat::B8G8R8A8_Unorm : GnmSurfaceFormat::R8G8B8A8_Unorm;
        break;
    }
    case 8: rt.format = GnmSurfaceFormat::R16G16B16A16_Float; break;
    case 9: rt.format = GnmSurfaceFormat::R32G32B32A32_Float; break;
    case 10: rt.format = GnmSurfaceFormat::R10G10B10A2_Unorm; break;
    default: rt.format = (fmt_id != 0) ? GnmSurfaceFormat::R8G8B8A8_Unorm : GnmSurfaceFormat::Invalid; break;
    }
}

void GnmContext::update_depth_stencil() {
    auto& ds = pipeline_state_.depth_stencil;
    const auto reg_ctrl  = context_regs_[gcn_reg::DB_DEPTH_CONTROL];
    const auto reg_zinfo = context_regs_[gcn_reg::DB_Z_INFO];
    const auto reg_sinfo = context_regs_[gcn_reg::DB_STENCIL_INFO];
    const auto reg_size  = context_regs_[gcn_reg::DB_DEPTH_SIZE];
    const auto reg_dbase = context_regs_[gcn_reg::DB_DEPTH_BASE];
    const auto reg_sbase = context_regs_[gcn_reg::DB_STENCIL_BASE];
    const auto reg_htile = context_regs_[gcn_reg::DB_HTILE_DATA_BASE];

    ds.depth_address = static_cast<std::uint64_t>(reg_dbase) << 8;
    ds.stencil_address = static_cast<std::uint64_t>(reg_sbase) << 8;
    ds.htile_address = static_cast<std::uint64_t>(reg_htile) << 8;

    ds.pitch = ((reg_size & 0x3FFF) + 1) * 8;
    ds.height = (((reg_size >> 14) & 0x3FFF) + 1) * 8;
    ds.width = ds.pitch;

    ds.depth_test_enabled = (reg_ctrl & 0x1) != 0;
    ds.depth_write_enabled = (reg_ctrl & 0x2) != 0;
    ds.depth_func = static_cast<GnmCompareFunc>((reg_ctrl >> 4) & 0x7);

    ds.stencil_test_enabled = (reg_ctrl & 0x80) != 0;
    ds.stencil_write_enabled = (reg_ctrl & 0x100) != 0;
    ds.stencil_func = static_cast<GnmCompareFunc>((reg_ctrl >> 9) & 0x7);

    const auto zfmt = reg_zinfo & 0xF;
    if (zfmt == 1) ds.z_format = GnmSurfaceFormat::D16_Unorm;
    else if (zfmt == 2) ds.z_format = GnmSurfaceFormat::D32_Float;
    else if (zfmt == 3) ds.z_format = GnmSurfaceFormat::D32_Float_S8_Uint;
    else ds.z_format = (zfmt != 0) ? GnmSurfaceFormat::D32_Float : GnmSurfaceFormat::Invalid;

    const auto sfmt = reg_sinfo & 0xF;
    ds.stencil_format = (sfmt != 0) ? GnmSurfaceFormat::S8_Uint : GnmSurfaceFormat::Invalid;
}

void GnmContext::update_viewport(std::uint32_t index) {
    if (index >= 16) return;
    const auto base = gcn_reg::PA_CL_VPORT_XSCALE + index * 6;
    const float sx = uint_as_float(context_regs_[base + 0]);
    const float ox = uint_as_float(context_regs_[base + 1]);
    const float sy = uint_as_float(context_regs_[base + 2]);
    const float oy = uint_as_float(context_regs_[base + 3]);
    const float sz = uint_as_float(context_regs_[base + 4]);
    const float oz = uint_as_float(context_regs_[base + 5]);

    auto& vp = pipeline_state_.viewports.viewports[index];
    vp.scale_x = sx; vp.offset_x = ox;
    vp.scale_y = sy; vp.offset_y = oy;
    vp.scale_z = sz; vp.offset_z = oz;

    vp.width = 2.0f * std::abs(sx);
    vp.height = 2.0f * std::abs(sy);
    vp.x = ox - std::abs(sx);
    vp.y = oy - std::abs(sy);
    vp.min_depth = oz - sz;
    vp.max_depth = oz + sz;
}

void GnmContext::update_scissor(std::uint32_t index) {
    if (index >= 16) return;
    const auto base = gcn_reg::PA_SC_VPORT_SCISSOR_0_TL + index * 2;
    const auto tl = context_regs_[base + 0];
    const auto br = context_regs_[base + 1];

    auto& sc = pipeline_state_.scissors.scissors[index];
    sc.left = tl & 0x7FFF;
    sc.top = (tl >> 16) & 0x7FFF;
    sc.right = br & 0x7FFF;
    sc.bottom = (br >> 16) & 0x7FFF;
}

void GnmContext::update_blend_target(std::uint32_t index) {
    if (index >= 8) return;
    const auto ctrl = context_regs_[gcn_reg::CB_BLEND0_CONTROL + index];
    auto& bt = pipeline_state_.blend.targets[index];

    bt.blend_enable = (ctrl & 1) != 0;
    bt.color_src_factor = static_cast<GnmBlendFactor>((ctrl >> 1) & 0xF);
    bt.color_dst_factor = static_cast<GnmBlendFactor>((ctrl >> 5) & 0xF);
    bt.color_op = static_cast<GnmBlendOp>((ctrl >> 9) & 0x7);
    bt.alpha_src_factor = static_cast<GnmBlendFactor>((ctrl >> 12) & 0xF);
    bt.alpha_dst_factor = static_cast<GnmBlendFactor>((ctrl >> 16) & 0xF);
    bt.alpha_op = static_cast<GnmBlendOp>((ctrl >> 20) & 0x7);
    bt.separate_alpha = (ctrl & (1 << 25)) != 0;
}

void GnmContext::update_rasterizer() {
    auto& rz = pipeline_state_.rasterizer;
    const auto mode = context_regs_[gcn_reg::PA_SU_SC_MODE_CNTL];

    const bool cull_front = (mode & 1) != 0;
    const bool cull_back = (mode & 2) != 0;
    if (cull_front && cull_back) {
        rz.cull_mode = GnmCullMode::Back;
    } else if (cull_back) {
        rz.cull_mode = GnmCullMode::Back;
    } else if (cull_front) {
        rz.cull_mode = GnmCullMode::Front;
    } else {
        rz.cull_mode = GnmCullMode::None;
    }

    rz.front_face = ((mode & 4) != 0) ? GnmFrontFace::Clockwise : GnmFrontFace::CounterClockwise;

    const auto line_val = context_regs_[gcn_reg::PA_SU_LINE_CNTL] & 0xFFFF;
    rz.line_width = (line_val > 0) ? (static_cast<float>(line_val) / 8.0f) : 1.0f;

    const auto pt_val = context_regs_[gcn_reg::PA_SU_POINT_SIZE] & 0xFFFF;
    rz.point_size = (pt_val > 0) ? (static_cast<float>(pt_val) / 8.0f) : 1.0f;

    rz.depth_bias_clamp = uint_as_float(context_regs_[gcn_reg::PA_SU_POLY_OFFSET_CLAMP]);
    rz.slope_scaled_depth_bias = uint_as_float(context_regs_[gcn_reg::PA_SU_POLY_OFFSET_FRONT_SCALE]);
    rz.depth_bias = uint_as_float(context_regs_[gcn_reg::PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    rz.depth_bias_enable = (rz.slope_scaled_depth_bias != 0.0f || rz.depth_bias != 0.0f);
}

// =============================================================================
// Extração Segura de Descritores de Memória Guest
// =============================================================================

bool GnmContext::read_buffer_descriptor(std::uint64_t address, BufferDescriptor& out) const {
    out = BufferDescriptor{};
    if (address == 0) return false;

    std::uint8_t bytes[16];
    if (memory_.load(address, bytes, sizeof(bytes)) != MemoryError::ok) {
        return false;
    }

    std::uint32_t dw[4];
    for (std::size_t i = 0; i < 4; ++i) {
        dw[i] = static_cast<std::uint32_t>(bytes[i * 4 + 0]) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
        out.raw[i] = dw[i];
    }

    // 48-bit base address: dw0 (low 32 bits), dw1[15:0] (high 16 bits)
    out.base_address = (static_cast<std::uint64_t>(dw[1] & 0xFFFF) << 32) | dw[0];
    out.stride = (dw[1] >> 16) & 0x3FFF; // 14 bits de stride
    out.num_records = dw[2];
    out.format = dw[3];

    // Validação estrita de limites: evitar integer overflow no cálculo de total_size
    if (out.stride > 0 && out.num_records > (UINT64_MAX / out.stride)) {
        out = BufferDescriptor{};
        return false;
    }

    return out.is_valid();
}

bool GnmContext::read_texture_descriptor(std::uint64_t address, TextureDescriptor& out) const {
    out = TextureDescriptor{};
    if (address == 0) return false;

    std::uint8_t bytes[32];
    if (memory_.load(address, bytes, sizeof(bytes)) != MemoryError::ok) {
        return false;
    }

    std::uint32_t dw[8];
    for (std::size_t i = 0; i < 8; ++i) {
        dw[i] = static_cast<std::uint32_t>(bytes[i * 4 + 0]) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
        out.raw[i] = dw[i];
    }

    // GCN Texture base address: dw0 é base[39:8], dw1[7:0] é base[47:40]
    std::uint64_t addr = (static_cast<std::uint64_t>(dw[1] & 0xFF) << 40) |
                         (static_cast<std::uint64_t>(dw[0]) << 8);
    out.base_address = addr;
    out.format = (dw[1] >> 20) & 0x3F;
    out.width = (dw[2] & 0x3FFF) + 1;
    out.height = ((dw[2] >> 14) & 0x3FFF) + 1;
    out.pitch = ((dw[3] >> 20) & 0xFFF) + 1;

    const auto base_level = (dw[3] >> 13) & 0x7;
    const auto last_level = (dw[3] >> 16) & 0xF;
    out.num_mips = (last_level >= base_level) ? (last_level - base_level + 1) : 1;
    out.depth = (dw[4] & 0x1FFF) + 1;
    out.tile_mode = (dw[3] >> 3) & 0x1F;

    // Bounds checking
    if (out.width > 16384 || out.height > 16384 || out.num_mips > 16) {
        out = TextureDescriptor{};
        return false;
    }

    return out.is_valid();
}

bool GnmContext::read_sampler_descriptor(std::uint64_t address, SamplerDescriptor& out) const {
    out = SamplerDescriptor{};
    if (address == 0) return false;

    std::uint8_t bytes[16];
    if (memory_.load(address, bytes, sizeof(bytes)) != MemoryError::ok) {
        return false;
    }

    std::uint32_t dw[4];
    for (std::size_t i = 0; i < 4; ++i) {
        dw[i] = static_cast<std::uint32_t>(bytes[i * 4 + 0]) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
        out.raw[i] = dw[i];
    }

    out.clamp_x = dw[0] & 0x7;
    out.clamp_y = (dw[0] >> 3) & 0x7;
    out.clamp_z = (dw[0] >> 6) & 0x7;
    out.min_filter = (dw[0] >> 9) & 0x7;
    out.mag_filter = (dw[0] >> 12) & 0x7;
    out.mip_filter = (dw[0] >> 15) & 0x3;
    out.min_lod = static_cast<float>((dw[1] >> 8) & 0xFFF) / 256.0f;
    out.max_lod = static_cast<float>((dw[1] >> 20) & 0xFFF) / 256.0f;
    out.lod_bias = static_cast<float>(static_cast<std::int16_t>(dw[2] & 0x3FFF)) / 256.0f;
    return true;
}

bool GnmContext::read_buffer_from_table(std::uint64_t table_address, std::uint32_t slot, BufferDescriptor& out) const {
    out = BufferDescriptor{};
    if (table_address == 0) return false;
    constexpr std::uint64_t desc_size = 16;
    if (slot > (UINT64_MAX - desc_size) / desc_size) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(slot) * desc_size;
    if (table_address > UINT64_MAX - offset) return false;
    return read_buffer_descriptor(table_address + offset, out);
}

bool GnmContext::read_texture_from_table(std::uint64_t table_address, std::uint32_t slot, TextureDescriptor& out) const {
    out = TextureDescriptor{};
    if (table_address == 0) return false;
    constexpr std::uint64_t desc_size = 32;
    if (slot > (UINT64_MAX - desc_size) / desc_size) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(slot) * desc_size;
    if (table_address > UINT64_MAX - offset) return false;
    return read_texture_descriptor(table_address + offset, out);
}

bool GnmContext::read_sampler_from_table(std::uint64_t table_address, std::uint32_t slot, SamplerDescriptor& out) const {
    out = SamplerDescriptor{};
    if (table_address == 0) return false;
    constexpr std::uint64_t desc_size = 16;
    if (slot > (UINT64_MAX - desc_size) / desc_size) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(slot) * desc_size;
    if (table_address > UINT64_MAX - offset) return false;
    return read_sampler_descriptor(table_address + offset, out);
}

bool GnmContext::extract_buffer_from_user_data(ShaderStage stage, std::uint32_t start_slot, BufferDescriptor& out) const {
    out = BufferDescriptor{};
    if (start_slot + 4 > 16) return false;

    const auto& user_data = (stage == ShaderStage::Vertex) ? pipeline_state_.vs_user_data :
                            (stage == ShaderStage::Pixel)  ? pipeline_state_.ps_user_data :
                                                             compute_pipeline_state_.cs_user_data;

    std::uint32_t dw[4];
    for (std::size_t i = 0; i < 4; ++i) {
        dw[i] = user_data[start_slot + i];
        out.raw[i] = dw[i];
    }

    out.base_address = (static_cast<std::uint64_t>(dw[1] & 0xFFFF) << 32) | dw[0];
    out.stride = (dw[1] >> 16) & 0x3FFF;
    out.num_records = dw[2];
    out.format = dw[3];

    if (out.stride > 0 && out.num_records > (UINT64_MAX / out.stride)) {
        out = BufferDescriptor{};
        return false;
    }

    return out.is_valid();
}

// =============================================================================
// Utilitários Estáticos Determinísticos de Codificação
// =============================================================================

std::array<std::uint8_t, 16> GnmContext::encode_buffer_descriptor(const BufferDescriptor& desc) {
    std::array<std::uint8_t, 16> bytes{};
    const std::uint32_t dw0 = static_cast<std::uint32_t>(desc.base_address & 0xFFFFFFFF);
    const std::uint32_t dw1 = static_cast<std::uint32_t>((desc.base_address >> 32) & 0xFFFF) |
                             ((desc.stride & 0x3FFF) << 16);
    const std::uint32_t dw2 = desc.num_records;
    const std::uint32_t dw3 = desc.format;

    const std::uint32_t words[4] = {dw0, dw1, dw2, dw3};
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i * 4 + 0] = static_cast<std::uint8_t>(words[i] & 0xFF);
        bytes[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        bytes[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        bytes[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
    }
    return bytes;
}

std::array<std::uint8_t, 32> GnmContext::encode_texture_descriptor(const TextureDescriptor& desc) {
    std::array<std::uint8_t, 32> bytes{};
    // dw0: base[39:8]
    const std::uint32_t dw0 = static_cast<std::uint32_t>((desc.base_address >> 8) & 0xFFFFFFFF);
    // dw1: base[47:40] nos bits 7..0, format nos bits 25..20
    const std::uint32_t dw1 = static_cast<std::uint32_t>((desc.base_address >> 40) & 0xFF) |
                             ((desc.format & 0x3F) << 20);
    // dw2: width-1 (13:0), height-1 (27:14)
    const std::uint32_t w_val = desc.width > 0 ? desc.width - 1 : 0;
    const std::uint32_t h_val = desc.height > 0 ? desc.height - 1 : 0;
    const std::uint32_t dw2 = (w_val & 0x3FFF) | ((h_val & 0x3FFF) << 14);
    // dw3: tile_mode (7:3), base_level (15:13 = 0), last_level (19:16), pitch-1 (31:20)
    const std::uint32_t p_val = desc.pitch > 0 ? desc.pitch - 1 : 0;
    const std::uint32_t last_level = desc.num_mips > 0 ? desc.num_mips - 1 : 0;
    const std::uint32_t dw3 = ((desc.tile_mode & 0x1F) << 3) |
                             ((last_level & 0xF) << 16) |
                             ((p_val & 0xFFF) << 20);
    // dw4: depth-1 (12:0)
    const std::uint32_t d_val = desc.depth > 0 ? desc.depth - 1 : 0;
    const std::uint32_t dw4 = d_val & 0x1FFF;
    const std::uint32_t dw5 = 0;
    const std::uint32_t dw6 = 0;
    const std::uint32_t dw7 = 0;

    const std::uint32_t words[8] = {dw0, dw1, dw2, dw3, dw4, dw5, dw6, dw7};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i * 4 + 0] = static_cast<std::uint8_t>(words[i] & 0xFF);
        bytes[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        bytes[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        bytes[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
    }
    return bytes;
}

std::array<std::uint8_t, 16> GnmContext::encode_sampler_descriptor(const SamplerDescriptor& desc) {
    std::array<std::uint8_t, 16> bytes{};
    const std::uint32_t dw0 = (desc.clamp_x & 0x7) |
                             ((desc.clamp_y & 0x7) << 3) |
                             ((desc.clamp_z & 0x7) << 6) |
                             ((desc.min_filter & 0x7) << 9) |
                             ((desc.mag_filter & 0x7) << 12) |
                             ((desc.mip_filter & 0x3) << 15);
    const std::uint32_t min_l = static_cast<std::uint32_t>(desc.min_lod * 256.0f) & 0xFFF;
    const std::uint32_t max_l = static_cast<std::uint32_t>(desc.max_lod * 256.0f) & 0xFFF;
    const std::uint32_t dw1 = (min_l << 8) | (max_l << 20);
    const std::uint32_t bias = static_cast<std::uint32_t>(static_cast<std::int32_t>(desc.lod_bias * 256.0f)) & 0x3FFF;
    const std::uint32_t dw2 = bias;
    const std::uint32_t dw3 = 0;

    const std::uint32_t words[4] = {dw0, dw1, dw2, dw3};
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i * 4 + 0] = static_cast<std::uint8_t>(words[i] & 0xFF);
        bytes[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        bytes[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        bytes[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
    }
    return bytes;
}

} // namespace bb
