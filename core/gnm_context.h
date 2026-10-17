#pragma once

#include "guest_memory.h"
#include "gnm_pm4.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bb {

// =============================================================================
// Constantes de Registradores GCN Sea Islands / Bonaire / Liverpool (PS4 Gnm)
// =============================================================================
namespace gcn_reg {
// Registradores de Contexto (offsets DWORD relativos à base 0xA000)
constexpr std::uint32_t PA_SC_WINDOW_SCISSOR_TL      = 0x034;
constexpr std::uint32_t PA_SC_WINDOW_SCISSOR_BR      = 0x035;
constexpr std::uint32_t PA_SC_SCREEN_SCISSOR_TL      = 0x03C;
constexpr std::uint32_t PA_SC_SCREEN_SCISSOR_BR      = 0x03D;
constexpr std::uint32_t PA_SC_VPORT_SCISSOR_0_TL     = 0x07A;
constexpr std::uint32_t PA_SC_VPORT_SCISSOR_0_BR     = 0x07B;

constexpr std::uint32_t PA_CL_VPORT_XSCALE           = 0x10F;
constexpr std::uint32_t PA_CL_VPORT_XOFFSET          = 0x110;
constexpr std::uint32_t PA_CL_VPORT_YSCALE           = 0x111;
constexpr std::uint32_t PA_CL_VPORT_YOFFSET          = 0x112;
constexpr std::uint32_t PA_CL_VPORT_ZSCALE           = 0x113;
constexpr std::uint32_t PA_CL_VPORT_ZOFFSET          = 0x114;

constexpr std::uint32_t CB_BLEND0_CONTROL            = 0x1E0;
constexpr std::uint32_t DB_DEPTH_CONTROL             = 0x200;
constexpr std::uint32_t CB_COLOR_CONTROL             = 0x202;
constexpr std::uint32_t CB_TARGET_MASK               = 0x203;
constexpr std::uint32_t CB_BLEND_RED                 = 0x204;
constexpr std::uint32_t CB_BLEND_GREEN               = 0x205;
constexpr std::uint32_t CB_BLEND_BLUE                = 0x206;
constexpr std::uint32_t CB_BLEND_ALPHA               = 0x207;
constexpr std::uint32_t PA_CL_CLIP_CNTL              = 0x208;
constexpr std::uint32_t PA_SU_SC_MODE_CNTL           = 0x209;

constexpr std::uint32_t PA_SU_LINE_CNTL              = 0x282;
constexpr std::uint32_t PA_SU_POINT_SIZE             = 0x283;
constexpr std::uint32_t VGT_PRIMITIVE_TYPE           = 0x28B;

constexpr std::uint32_t DB_Z_INFO                    = 0x2CD;
constexpr std::uint32_t DB_STENCIL_INFO              = 0x2CE;
constexpr std::uint32_t DB_DEPTH_VIEW                = 0x2D4;
constexpr std::uint32_t DB_DEPTH_SIZE                = 0x2D5;
constexpr std::uint32_t DB_DEPTH_SLICE               = 0x2D6;
constexpr std::uint32_t DB_DEPTH_BASE                = 0x2DC;
constexpr std::uint32_t DB_STENCIL_BASE              = 0x2E0;
constexpr std::uint32_t DB_HTILE_DATA_BASE           = 0x2E4;
constexpr std::uint32_t PA_SU_POLY_OFFSET_CLAMP      = 0x2E8;
constexpr std::uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2E9;
constexpr std::uint32_t PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2EA;

constexpr std::uint32_t CB_COLOR0_BASE               = 0x318;
constexpr std::uint32_t CB_COLOR0_PITCH              = 0x319;
constexpr std::uint32_t CB_COLOR0_SLICE              = 0x31A;
constexpr std::uint32_t CB_COLOR0_VIEW               = 0x31B;
constexpr std::uint32_t CB_COLOR0_INFO               = 0x31C;
constexpr std::uint32_t CB_COLOR0_ATTRIB             = 0x31D;
constexpr std::uint32_t CB_COLOR0_DIM                = 0x31E;
constexpr std::uint32_t CB_COLOR0_CMASK              = 0x31F;
constexpr std::uint32_t CB_COLOR0_FMASK              = 0x321;
constexpr std::uint32_t CB_COLOR_STRIDE              = 0x014; // 20 DWORDs entre slots de RT

// Registradores de Shader (offsets DWORD relativos à base 0x2C00)
constexpr std::uint32_t SPI_SHADER_PGM_LO_VS         = 0x048;
constexpr std::uint32_t SPI_SHADER_PGM_HI_VS         = 0x049;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC1_VS      = 0x04A;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC2_VS      = 0x04B;
constexpr std::uint32_t SPI_SHADER_USER_DATA_VS_0    = 0x04C; // 16 DWORDs: 0x4C..0x5B

constexpr std::uint32_t SPI_SHADER_PGM_LO_PS         = 0x0C8;
constexpr std::uint32_t SPI_SHADER_PGM_HI_PS         = 0x0C9;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC1_PS      = 0x0CA;
constexpr std::uint32_t SPI_SHADER_PGM_RSRC2_PS      = 0x0CB;
constexpr std::uint32_t SPI_SHADER_USER_DATA_PS_0    = 0x0CC; // 16 DWORDs: 0xCC..0xDB

constexpr std::uint32_t COMPUTE_PGM_LO               = 0x20C;
constexpr std::uint32_t COMPUTE_PGM_HI               = 0x20D;
constexpr std::uint32_t COMPUTE_PGM_RSRC1            = 0x212;
constexpr std::uint32_t COMPUTE_PGM_RSRC2            = 0x213;
constexpr std::uint32_t COMPUTE_USER_DATA_0          = 0x204; // 16 DWORDs: 0x204..0x213
constexpr std::uint32_t COMPUTE_USER_DATA_ALT_0      = 0x140; // Alternativa: 0x140..0x14F
} // namespace gcn_reg

// =============================================================================
// Tipos e Enums de Configuração Gráfica e Computação Gnm
// =============================================================================

enum class GnmPrimitiveType : std::uint32_t {
    PointList = 1,
    LineList = 2,
    LineStrip = 3,
    TriList = 4,
    TriFan = 5,
    TriStrip = 6,
    RectList = 8,
    QuadList = 13,
    Patch = 14
};

enum class GnmIndexType : std::uint32_t {
    Index16 = 0,
    Index32 = 1
};

enum class GnmSurfaceFormat : std::uint32_t {
    Invalid = 0,
    R8_Unorm,
    R8G8_Unorm,
    R8G8B8A8_Unorm,
    B8G8R8A8_Unorm,
    R16_Float,
    R16G16_Float,
    R16G16B16A16_Float,
    R32_Float,
    R32G32_Float,
    R32G32B32A32_Float,
    R11G11B10_Float,
    R10G10B10A2_Unorm,
    D16_Unorm,
    D32_Float,
    D32_Float_S8_Uint,
    S8_Uint
};

enum class GnmCullMode : std::uint32_t {
    None = 0,
    Front = 1,
    Back = 2
};

enum class GnmFrontFace : std::uint32_t {
    CounterClockwise = 0,
    Clockwise = 1
};

enum class GnmCompareFunc : std::uint32_t {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7
};

enum class GnmBlendFactor : std::uint32_t {
    Zero = 0,
    One = 1,
    SrcColor = 2,
    OneMinusSrcColor = 3,
    SrcAlpha = 4,
    OneMinusSrcAlpha = 5,
    DstAlpha = 6,
    OneMinusDstAlpha = 7,
    DstColor = 8,
    OneMinusDstColor = 9
};

enum class GnmBlendOp : std::uint32_t {
    Add = 0,
    Subtract = 1,
    ReverseSubtract = 2,
    Min = 3,
    Max = 4
};

enum class ShaderStage : std::uint32_t {
    Vertex = 0,
    Pixel = 1,
    Compute = 2
};

// =============================================================================
// Estruturas de Estado de Renderização e Superfícies
// =============================================================================

// Render Target individual
struct RenderTarget {
    std::uint64_t base_address = 0;
    GnmSurfaceFormat format = GnmSurfaceFormat::Invalid;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t slice_tile_max = 0;
    std::uint64_t cmask_address = 0;
    std::uint64_t fmask_address = 0;
    bool enabled = false;

    bool is_valid() const {
        return enabled && base_address != 0 && width > 0 && height > 0;
    }
};

// Conjunto dos 8 Render Targets
struct RenderTargetsState {
    std::array<RenderTarget, 8> targets{};
    std::uint8_t target_mask = 0xFF; // Write mask por RT (4 bits por canal)

    std::uint32_t active_count() const {
        std::uint32_t count = 0;
        for (const auto& t : targets) {
            if (t.is_valid()) ++count;
        }
        return count;
    }
};

// Depth/Stencil Surface
struct DepthStencilState {
    std::uint64_t depth_address = 0;
    std::uint64_t stencil_address = 0;
    std::uint64_t htile_address = 0;
    GnmSurfaceFormat z_format = GnmSurfaceFormat::Invalid;
    GnmSurfaceFormat stencil_format = GnmSurfaceFormat::Invalid;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    bool depth_test_enabled = false;
    bool depth_write_enabled = false;
    bool stencil_test_enabled = false;
    bool stencil_write_enabled = false;
    GnmCompareFunc depth_func = GnmCompareFunc::LessEqual;
    GnmCompareFunc stencil_func = GnmCompareFunc::Always;

    bool is_valid() const {
        return (depth_address != 0 || stencil_address != 0) && width > 0 && height > 0;
    }
};

// Viewport
struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
    float scale_x = 0.0f;
    float scale_y = 0.0f;
    float scale_z = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
};

struct ViewportState {
    std::array<Viewport, 16> viewports{};
    std::uint32_t active_mask = 1;
};

// Scissor
struct ScissorRect {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;

    std::uint32_t width() const { return right > left ? right - left : 0; }
    std::uint32_t height() const { return bottom > top ? bottom - top : 0; }
};

struct ScissorState {
    std::array<ScissorRect, 16> scissors{};
    ScissorRect window_scissor{};
    ScissorRect screen_scissor{};
};

// Blend Target
struct BlendTarget {
    bool blend_enable = false;
    GnmBlendFactor color_src_factor = GnmBlendFactor::One;
    GnmBlendFactor color_dst_factor = GnmBlendFactor::Zero;
    GnmBlendOp color_op = GnmBlendOp::Add;
    GnmBlendFactor alpha_src_factor = GnmBlendFactor::One;
    GnmBlendFactor alpha_dst_factor = GnmBlendFactor::Zero;
    GnmBlendOp alpha_op = GnmBlendOp::Add;
    bool separate_alpha = false;
};

struct BlendState {
    std::array<BlendTarget, 8> targets{};
    std::array<float, 4> blend_color = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint8_t target_mask = 0xFF;
};

// Rasterizer State
struct RasterizerState {
    GnmCullMode cull_mode = GnmCullMode::Back;
    GnmFrontFace front_face = GnmFrontFace::Clockwise;
    bool depth_bias_enable = false;
    float depth_bias = 0.0f;
    float slope_scaled_depth_bias = 0.0f;
    float depth_bias_clamp = 0.0f;
    float line_width = 1.0f;
    float point_size = 1.0f;
};

// =============================================================================
// Descritores GCN: Buffer (V#), Textura (T#), Sampler (S#)
// =============================================================================

// Descritor de Buffer (16 bytes / 4 DWORDs)
struct BufferDescriptor {
    std::uint64_t base_address = 0;   // 48-bit base address
    std::uint32_t stride = 0;         // 14-bit stride em bytes (0..16383)
    std::uint32_t num_records = 0;    // Quantidade de registros
    std::uint32_t format = 0;         // Data/num format
    std::array<std::uint32_t, 4> raw{};

    std::size_t total_size_bytes() const {
        if (stride > 0) {
            if (num_records > (UINT64_MAX / stride)) return 0;
            return static_cast<std::size_t>(stride) * num_records;
        }
        return num_records;
    }

    bool is_valid() const {
        return base_address != 0 && (num_records > 0 || stride > 0);
    }
};

// Descritor de Textura (32 bytes / 8 DWORDs)
struct TextureDescriptor {
    std::uint64_t base_address = 0;   // 48-bit base address
    std::uint32_t format = 0;
    std::uint32_t width = 0;          // 1..16384
    std::uint32_t height = 0;         // 1..16384
    std::uint32_t pitch = 0;
    std::uint32_t depth = 1;
    std::uint32_t num_mips = 1;       // 1..16
    std::uint32_t tile_mode = 0;
    std::array<std::uint32_t, 8> raw{};

    bool is_valid() const {
        return base_address != 0 && width > 0 && height > 0 &&
               width <= 16384 && height <= 16384 && num_mips <= 16;
    }
};

// Descritor de Sampler (16 bytes / 4 DWORDs)
struct SamplerDescriptor {
    std::uint32_t clamp_x = 0;
    std::uint32_t clamp_y = 0;
    std::uint32_t clamp_z = 0;
    std::uint32_t min_filter = 0;
    std::uint32_t mag_filter = 0;
    std::uint32_t mip_filter = 0;
    std::uint32_t aniso_ratio = 0;
    float min_lod = 0.0f;
    float max_lod = 15.0f;
    float lod_bias = 0.0f;
    std::array<std::uint32_t, 4> raw{};
};

// =============================================================================
// Snapshots do Estado do Pipeline (Renderização e Computação)
// =============================================================================

struct PipelineState {
    RenderTargetsState render_targets{};
    DepthStencilState depth_stencil{};
    ViewportState viewports{};
    ScissorState scissors{};
    BlendState blend{};
    RasterizerState rasterizer{};

    std::uint64_t vs_program_address = 0;
    std::uint64_t ps_program_address = 0;
    std::array<std::uint32_t, 16> vs_user_data{};
    std::array<std::uint32_t, 16> ps_user_data{};

    GnmPrimitiveType primitive_type = GnmPrimitiveType::TriList;
};

struct ComputePipelineState {
    std::uint64_t cs_program_address = 0;
    std::array<std::uint32_t, 16> cs_user_data{};
};

struct DrawCall {
    bool indexed = false;
    std::uint32_t index_count = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t start_index = 0;
    std::uint64_t index_buffer_address = 0;
    GnmIndexType index_type = GnmIndexType::Index16;
    std::uint32_t instance_count = 1;
    std::uint32_t start_instance = 0;
    PipelineState pipeline_state{};
};

struct DispatchCall {
    std::uint32_t threadgroups_x = 0;
    std::uint32_t threadgroups_y = 0;
    std::uint32_t threadgroups_z = 0;
    ComputePipelineState pipeline_state{};
};

// =============================================================================
// GnmContext: Rastreamento de Estado de GPU Gnm/GCN e interface para Metal
// =============================================================================

class GnmContext : public Pm4Sink {
public:
    explicit GnmContext(GuestMemory& memory);
    explicit GnmContext(const GuestMemory& memory);

    // Pm4Sink overrides
    void on_pm4_event(const Pm4Event& event) override;
    void on_context_reg(std::uint32_t offset, std::uint32_t value) override;
    void on_sh_reg(std::uint32_t offset, std::uint32_t value, bool compute) override;
    void on_uconfig_reg(std::uint32_t offset, std::uint32_t value) override;

    // Submissão de buffers de comandos PM4
    Pm4Status submit(std::uint64_t address, std::uint32_t words, const Pm4Limits& limits = {});

    // Acesso e modificação direta de registradores (para testes e HLE)
    void set_context_reg(std::uint32_t offset, std::uint32_t value);
    void set_sh_reg(std::uint32_t offset, std::uint32_t value, bool compute = false);
    void set_uconfig_reg(std::uint32_t offset, std::uint32_t value);

    std::uint32_t context_reg(std::uint32_t offset) const;
    std::uint32_t sh_reg(std::uint32_t offset, bool compute = false) const;
    std::uint32_t uconfig_reg(std::uint32_t offset) const;

    // Consultas tipadas de estado para o backend
    const PipelineState& current_pipeline_state() const { return pipeline_state_; }
    const ComputePipelineState& current_compute_pipeline_state() const { return compute_pipeline_state_; }
    const RenderTargetsState& current_render_targets() const { return pipeline_state_.render_targets; }
    const DepthStencilState& current_depth_stencil() const { return pipeline_state_.depth_stencil; }
    const ViewportState& current_viewports() const { return pipeline_state_.viewports; }
    const Viewport& current_viewport(std::uint32_t slot = 0) const;
    const ScissorState& current_scissors() const { return pipeline_state_.scissors; }
    const ScissorRect& current_scissor(std::uint32_t slot = 0) const;
    const BlendState& current_blend() const { return pipeline_state_.blend; }
    const RasterizerState& current_rasterizer() const { return pipeline_state_.rasterizer; }

    std::uint64_t vs_program_address() const { return pipeline_state_.vs_program_address; }
    std::uint64_t ps_program_address() const { return pipeline_state_.ps_program_address; }
    std::uint64_t cs_program_address() const { return compute_pipeline_state_.cs_program_address; }
    std::uint32_t user_data(ShaderStage stage, std::uint32_t slot) const;

    // Métodos manuais para definir estado de shader e user data
    void set_vs_program(std::uint64_t address);
    void set_ps_program(std::uint64_t address);
    void set_cs_program(std::uint64_t address);
    void set_user_data(ShaderStage stage, std::uint32_t slot, std::uint32_t value);

    // Extração segura de descritores de GuestMemory com validação de limites
    bool read_buffer_descriptor(std::uint64_t address, BufferDescriptor& out) const;
    bool read_texture_descriptor(std::uint64_t address, TextureDescriptor& out) const;
    bool read_sampler_descriptor(std::uint64_t address, SamplerDescriptor& out) const;

    // Leitura a partir de tabelas indexadas na memória guest
    bool read_buffer_from_table(std::uint64_t table_address, std::uint32_t slot, BufferDescriptor& out) const;
    bool read_texture_from_table(std::uint64_t table_address, std::uint32_t slot, TextureDescriptor& out) const;
    bool read_sampler_from_table(std::uint64_t table_address, std::uint32_t slot, SamplerDescriptor& out) const;

    // Extração inline de descritor de buffer a partir de registradores User Data
    bool extract_buffer_from_user_data(ShaderStage stage, std::uint32_t start_slot, BufferDescriptor& out) const;

    // Consultas de chamadas registradas
    const DrawCall& current_draw_call() const;
    const DispatchCall& current_dispatch_call() const;
    const DrawCall* last_draw_call() const;
    const DispatchCall* last_dispatch_call() const;
    const std::vector<DrawCall>& draw_calls() const { return draw_calls_; }
    const std::vector<DispatchCall>& dispatch_calls() const { return dispatch_calls_; }
    const std::vector<Pm4Event>& sync_events() const { return sync_events_; }
    void clear_calls();

    // Diagnostico e rastreamento de escrita, DMA e sincronizacao (T153)
    std::uint32_t write_data_count() const { return write_data_count_; }
    std::uint32_t dma_count() const { return dma_count_; }
    std::uint32_t sync_error_count() const { return sync_error_count_; }
    std::uint32_t cache_invalidate_count() const { return cache_invalidate_count_; }
    bool caches_invalidated() const { return caches_invalidated_; }
    void clear_cache_invalidation() { caches_invalidated_ = false; }
    const std::string& last_error() const { return last_error_; }

    // Reset completo de contexto
    void reset();

    // Utilitários estáticos determinísticos para codificação de descritores
    static std::array<std::uint8_t, 16> encode_buffer_descriptor(const BufferDescriptor& desc);
    static std::array<std::uint8_t, 32> encode_texture_descriptor(const TextureDescriptor& desc);
    static std::array<std::uint8_t, 16> encode_sampler_descriptor(const SamplerDescriptor& desc);

private:
    void update_context_register(std::uint32_t offset, std::uint32_t value);
    void update_sh_register(std::uint32_t offset, std::uint32_t value, bool compute);
    void update_render_target(std::uint32_t index);
    void update_depth_stencil();
    void update_viewport(std::uint32_t index);
    void update_scissor(std::uint32_t index);
    void update_blend_target(std::uint32_t index);
    void update_rasterizer();

    void handle_write_data(const Pm4Event& event);
    void handle_dma_data(const Pm4Event& event);
    void handle_release_mem(const Pm4Event& event);
    void handle_wait_reg_mem(const Pm4Event& event);
    void handle_acquire_mem(const Pm4Event& event);
    void handle_event_sync(const Pm4Event& event);

    const GuestMemory& memory_;
    GuestMemory* memory_writer_ = nullptr;
    std::uint32_t write_data_count_ = 0;
    std::uint32_t dma_count_ = 0;
    std::uint32_t sync_error_count_ = 0;
    std::uint32_t cache_invalidate_count_ = 0;
    bool caches_invalidated_ = false;
    std::uint64_t gpu_timestamp_ = 1000;
    std::string last_error_{};

    std::array<std::uint32_t, 0x4000> context_regs_{};
    std::array<std::uint32_t, 0x4000> sh_regs_graphics_{};
    std::array<std::uint32_t, 0x4000> sh_regs_compute_{};
    std::array<std::uint32_t, 0x4000> uconfig_regs_{};

    PipelineState pipeline_state_{};
    ComputePipelineState compute_pipeline_state_{};

    std::vector<DrawCall> draw_calls_;
    std::vector<DispatchCall> dispatch_calls_;
    std::vector<Pm4Event> sync_events_;
    DrawCall empty_draw_{};
    DispatchCall empty_dispatch_{};
};

} // namespace bb
