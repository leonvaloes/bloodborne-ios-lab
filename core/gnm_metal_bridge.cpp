#include "gnm_metal_bridge.h"
#include "guest_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace bb {

// =============================================================================
// BridgeError -> string
// =============================================================================
const char* bridge_error_to_string(BridgeError err) {
    switch (err) {
        case BridgeError::Ok: return "ok";
        case BridgeError::InvalidGuestMemory: return "invalid_guest_memory";
        case BridgeError::InvalidBufferAddress: return "invalid_buffer_address";
        case BridgeError::InvalidTextureAddress: return "invalid_texture_address";
        case BridgeError::InvalidBindingSlot: return "invalid_binding_slot";
        case BridgeError::InvalidShaderAddress: return "invalid_shader_address";
        case BridgeError::ShaderReadFailed: return "shader_read_failed";
        case BridgeError::ShaderExceedsLimit: return "shader_exceeds_limit";
        case BridgeError::ShaderDecodingFailed: return "shader_decoding_failed";
        case BridgeError::ShaderLoweringFailed: return "shader_lowering_failed";
        case BridgeError::ShaderTranslationFailed: return "shader_translation_failed";
        case BridgeError::InvalidIndexBuffer: return "invalid_index_buffer";
        case BridgeError::InvalidTopology: return "invalid_topology";
        case BridgeError::PipelineConfigurationFailed: return "pipeline_configuration_failed";
        case BridgeError::EmptyDrawCall: return "empty_draw_call";
        default: return "unknown";
    }
}

// =============================================================================
// MetalResourceBindings Implementação
// =============================================================================
bool MetalResourceBindings::bind_buffer(std::uint32_t slot, const BufferDescriptor& desc) {
    if (slot >= MaxBuffers) return false;
    if (!desc.is_valid()) return false;
    buffers[slot].slot = slot;
    buffers[slot].guest_address = desc.base_address;
    buffers[slot].size = desc.total_size_bytes();
    buffers[slot].stride = desc.stride;
    buffers[slot].valid = true;
    active_buffers = std::max(active_buffers, slot + 1);
    return true;
}

bool MetalResourceBindings::bind_texture(std::uint32_t slot, const TextureDescriptor& desc) {
    if (slot >= MaxTextures) return false;
    if (!desc.is_valid()) return false;
    textures[slot].slot = slot;
    textures[slot].guest_address = desc.base_address;
    textures[slot].width = desc.width;
    textures[slot].height = desc.height;
    textures[slot].pitch = desc.pitch;
    textures[slot].num_mips = desc.num_mips;
    textures[slot].valid = true;
    active_textures = std::max(active_textures, slot + 1);
    return true;
}

bool MetalResourceBindings::bind_sampler(std::uint32_t slot, const SamplerDescriptor& desc) {
    if (slot >= MaxSamplers) return false;
    samplers[slot].slot = slot;
    samplers[slot].min_filter = desc.min_filter;
    samplers[slot].mag_filter = desc.mag_filter;
    samplers[slot].mip_filter = desc.mip_filter;
    samplers[slot].min_lod = desc.min_lod;
    samplers[slot].max_lod = desc.max_lod;
    samplers[slot].valid = true;
    active_samplers = std::max(active_samplers, slot + 1);
    return true;
}

void MetalResourceBindings::clear() {
    for (auto& b : buffers) b.valid = false;
    for (auto& t : textures) t.valid = false;
    for (auto& s : samplers) s.valid = false;
    active_buffers = 0;
    active_textures = 0;
    active_samplers = 0;
}

// =============================================================================
// GcnDecoder e GcnMslTranslator Wrappers
// =============================================================================
bb::gcn::Program GcnDecoder::decode(const std::uint32_t* words, std::size_t word_count) {
    return bb::gcn::decode(words, word_count);
}

bb::gcn::MslResult GcnMslTranslator::translate(const bb::gcn::ProgramIR& ir, ShaderStage stage) {
    return bb::gcn::translate_msl(ir, stage);
}

// =============================================================================
// ShaderCache Implementação
// =============================================================================
ShaderCache::ShaderCache(std::size_t max_entries) : max_entries_(max_entries) {}

std::uint64_t ShaderCache::calculate_hash(const void* data, std::size_t size) {
    // FNV-1a 64-bit hash
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

BridgeResult<const CachedShader*> ShaderCache::get_or_compile(
    const GuestMemory& memory,
    std::uint64_t guest_address,
    ShaderStage stage
) {
    // 1. Extrair bytes do shader da memória guest
    constexpr std::size_t MAX_SHADER_DWORDS = MAX_SHADER_BYTES / 4;
    std::vector<std::uint32_t> words;
    words.reserve(MAX_SHADER_DWORDS);

    if (guest_address == 0) {
        return {BridgeError::InvalidShaderAddress, "guest shader address is zero"};
    }

    // Ler palavra por palavra até s_endpgm (opcode 1 em SOPP = 0xBF810000) ou limite
    std::uint32_t word = 0;
    for (std::size_t i = 0; i < MAX_SHADER_DWORDS; ++i) {
        MemoryError mem_err = memory.load(guest_address + i * 4, &word, sizeof(word));
        if (mem_err != MemoryError::ok) {
            if (words.empty()) {
                return {BridgeError::InvalidShaderAddress, "shader address unmapped"};
            }
            return {BridgeError::ShaderReadFailed, "memory read failed at offset " + std::to_string(i * 4)};
        }
        words.push_back(word);

        // Verificar se é s_endpgm (SOPP com opcode 1): 0xBF810000
        if ((word & 0xFFFF0000) == 0xBF810000) {
            break;
        }
    }

    if (words.size() >= MAX_SHADER_DWORDS) {
        return {BridgeError::ShaderExceedsLimit, "shader exceeds 64KB limit"};
    }

    if (words.empty()) {
        return {BridgeError::ShaderReadFailed, "no shader words read"};
    }

    // 2. Calcular hash
    std::uint64_t hash = calculate_hash(words.data(), words.size() * sizeof(std::uint32_t));

    // 3. Verificar cache
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(hash);
        if (it != cache_.end()) {
            hit_count_++;
            return {BridgeError::Ok, "", &it->second};
        }
        miss_count_++;
    }

    // 4. Decodificar
    bb::gcn::Program prog = GcnDecoder::decode(words.data(), words.size());
    if (prog.instructions.empty()) {
        return {BridgeError::ShaderDecodingFailed, "decoded program has no instructions"};
    }

    // 5. Construir IR
    bb::gcn::ProgramIR ir = bb::gcn::lower(prog);
    if (!ir.complete) {
        return {BridgeError::ShaderLoweringFailed, "IR incomplete: " +
            (ir.diagnostics.empty() ? "unknown" : ir.diagnostics.front())};
    }

    // 6. Traduzir para MSL
    bb::gcn::MslResult msl_result = GcnMslTranslator::translate(ir, stage);
    if (!msl_result.error.empty()) {
        return {BridgeError::ShaderTranslationFailed, msl_result.error};
    }

    // 7. Inserir no cache
    CachedShader cached;
    cached.hash = hash;
    cached.stage = stage;
    cached.msl_source = std::move(msl_result.text);
    cached.instruction_count = static_cast<std::uint32_t>(prog.instructions.size());
    cached.byte_size = words.size() * sizeof(std::uint32_t);

    // Coleta bind metadata (simplificado - extrair de user data seria feito fora)
    // Por ora, vazio

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.size() >= max_entries_) {
            // Eviction simples: limpa tudo quando cheio (política FIFO/LRU simplificada)
            cache_.clear();
        }
        auto [it, inserted] = cache_.emplace(hash, std::move(cached));
        return {BridgeError::Ok, "", &it->second};
    }
}

void ShaderCache::insert(std::uint64_t hash, CachedShader shader) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() >= max_entries_) {
        cache_.clear();
    }
    cache_.emplace(hash, std::move(shader));
}

const CachedShader* ShaderCache::find(std::uint64_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(hash);
    return (it != cache_.end()) ? &it->second : nullptr;
}

std::size_t ShaderCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void ShaderCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    hit_count_ = 0;
    miss_count_ = 0;
}

// =============================================================================
// Mapeamentos Estáticos Gnm -> Metal
// =============================================================================
MetalPixelFormat GnmMetalBridge::map_surface_format(GnmSurfaceFormat format) {
    switch (format) {
        case GnmSurfaceFormat::R8_Unorm: return MetalPixelFormat::R8Unorm;
        case GnmSurfaceFormat::R8G8_Unorm: return MetalPixelFormat::RG8Unorm;
        case GnmSurfaceFormat::R8G8B8A8_Unorm: return MetalPixelFormat::RGBA8Unorm;
        case GnmSurfaceFormat::B8G8R8A8_Unorm: return MetalPixelFormat::BGRA8Unorm;
        case GnmSurfaceFormat::R16_Float: return MetalPixelFormat::R16Float;
        case GnmSurfaceFormat::R16G16_Float: return MetalPixelFormat::RG16Float;
        case GnmSurfaceFormat::R16G16B16A16_Float: return MetalPixelFormat::RGBA16Float;
        case GnmSurfaceFormat::R32_Float: return MetalPixelFormat::R32Float;
        case GnmSurfaceFormat::R32G32_Float: return MetalPixelFormat::RG32Float;
        case GnmSurfaceFormat::R32G32B32A32_Float: return MetalPixelFormat::RGBA32Float;
        case GnmSurfaceFormat::R11G11B10_Float: return MetalPixelFormat::RG11B10Float;
        case GnmSurfaceFormat::R10G10B10A2_Unorm: return MetalPixelFormat::RGB10A2Unorm;
        case GnmSurfaceFormat::D16_Unorm: return MetalPixelFormat::Depth16Unorm;
        case GnmSurfaceFormat::D32_Float: return MetalPixelFormat::Depth32Float;
        case GnmSurfaceFormat::D32_Float_S8_Uint: return MetalPixelFormat::Depth32Float_Stencil8;
        case GnmSurfaceFormat::S8_Uint: return MetalPixelFormat::Stencil8;
        default: return MetalPixelFormat::Invalid;
    }
}

MetalBlendFactor GnmMetalBridge::map_blend_factor(GnmBlendFactor factor) {
    switch (factor) {
        case GnmBlendFactor::Zero: return MetalBlendFactor::Zero;
        case GnmBlendFactor::One: return MetalBlendFactor::One;
        case GnmBlendFactor::SrcColor: return MetalBlendFactor::SourceColor;
        case GnmBlendFactor::OneMinusSrcColor: return MetalBlendFactor::OneMinusSourceColor;
        case GnmBlendFactor::SrcAlpha: return MetalBlendFactor::SourceAlpha;
        case GnmBlendFactor::OneMinusSrcAlpha: return MetalBlendFactor::OneMinusSourceAlpha;
        case GnmBlendFactor::DstAlpha: return MetalBlendFactor::DestinationAlpha;
        case GnmBlendFactor::OneMinusDstAlpha: return MetalBlendFactor::OneMinusDestinationAlpha;
        case GnmBlendFactor::DstColor: return MetalBlendFactor::DestinationColor;
        case GnmBlendFactor::OneMinusDstColor: return MetalBlendFactor::OneMinusDestinationColor;
        default: return MetalBlendFactor::One;
    }
}

MetalBlendOperation GnmMetalBridge::map_blend_op(GnmBlendOp op) {
    switch (op) {
        case GnmBlendOp::Add: return MetalBlendOperation::Add;
        case GnmBlendOp::Subtract: return MetalBlendOperation::Subtract;
        case GnmBlendOp::ReverseSubtract: return MetalBlendOperation::ReverseSubtract;
        case GnmBlendOp::Min: return MetalBlendOperation::Min;
        case GnmBlendOp::Max: return MetalBlendOperation::Max;
        default: return MetalBlendOperation::Add;
    }
}

MetalCompareFunction GnmMetalBridge::map_compare_func(GnmCompareFunc func) {
    switch (func) {
        case GnmCompareFunc::Never: return MetalCompareFunction::Never;
        case GnmCompareFunc::Less: return MetalCompareFunction::Less;
        case GnmCompareFunc::Equal: return MetalCompareFunction::Equal;
        case GnmCompareFunc::LessEqual: return MetalCompareFunction::LessEqual;
        case GnmCompareFunc::Greater: return MetalCompareFunction::Greater;
        case GnmCompareFunc::NotEqual: return MetalCompareFunction::NotEqual;
        case GnmCompareFunc::GreaterEqual: return MetalCompareFunction::GreaterEqual;
        case GnmCompareFunc::Always: return MetalCompareFunction::Always;
        default: return MetalCompareFunction::Always;
    }
}

MetalPrimitiveType GnmMetalBridge::map_primitive_type(GnmPrimitiveType prim, bool& out_supported) {
    switch (prim) {
        case GnmPrimitiveType::PointList: out_supported = true; return MetalPrimitiveType::Point;
        case GnmPrimitiveType::LineList: out_supported = true; return MetalPrimitiveType::Line;
        case GnmPrimitiveType::LineStrip: out_supported = true; return MetalPrimitiveType::LineStrip;
        case GnmPrimitiveType::TriList: out_supported = true; return MetalPrimitiveType::Triangle;
        case GnmPrimitiveType::TriFan: out_supported = false; return MetalPrimitiveType::Triangle;
        case GnmPrimitiveType::TriStrip: out_supported = true; return MetalPrimitiveType::TriangleStrip;
        case GnmPrimitiveType::RectList: out_supported = false; return MetalPrimitiveType::Triangle;
        case GnmPrimitiveType::QuadList: out_supported = false; return MetalPrimitiveType::Triangle;
        case GnmPrimitiveType::Patch: out_supported = false; return MetalPrimitiveType::Triangle;
        default: out_supported = false; return MetalPrimitiveType::Triangle;
    }
}

MetalCullMode GnmMetalBridge::map_cull_mode(GnmCullMode mode) {
    switch (mode) {
        case GnmCullMode::None: return MetalCullMode::None;
        case GnmCullMode::Front: return MetalCullMode::Front;
        case GnmCullMode::Back: return MetalCullMode::Back;
        default: return MetalCullMode::Back;
    }
}

MetalWinding GnmMetalBridge::map_front_face(GnmFrontFace face) {
    return (face == GnmFrontFace::Clockwise) ? MetalWinding::Clockwise : MetalWinding::CounterClockwise;
}

// =============================================================================
// GnmMetalBridge Implementação
// =============================================================================
GnmMetalBridge::GnmMetalBridge(const GuestMemory& memory) : memory_(memory), shader_cache_(1024) {}

bool GnmMetalBridge::bind_buffer(ShaderStage stage, std::uint32_t slot, const BufferDescriptor& desc) {
    if (slot >= MetalResourceBindings::MaxBuffers) return false;
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    return bindings ? bindings->bind_buffer(slot, desc) : false;
}

bool GnmMetalBridge::bind_texture(ShaderStage stage, std::uint32_t slot, const TextureDescriptor& desc) {
    if (slot >= MetalResourceBindings::MaxTextures) return false;
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    return bindings ? bindings->bind_texture(slot, desc) : false;
}

bool GnmMetalBridge::bind_sampler(ShaderStage stage, std::uint32_t slot, const SamplerDescriptor& desc) {
    if (slot >= MetalResourceBindings::MaxSamplers) return false;
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    return bindings ? bindings->bind_sampler(slot, desc) : false;
}

bool GnmMetalBridge::bind_buffer_table(ShaderStage stage, std::uint32_t base_slot, std::uint64_t table_addr, std::uint32_t count) {
    if (base_slot + count > MetalResourceBindings::MaxBuffers) return false;
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    if (!bindings) return false;

    GnmContext ctx(memory_);
    for (std::uint32_t i = 0; i < count; ++i) {
        BufferDescriptor desc;
        if (!ctx.read_buffer_descriptor(table_addr + i * 16, desc)) {
            return false;
        }
        if (!desc.is_valid()) continue;
        if (!bindings->bind_buffer(base_slot + i, desc)) return false;
    }
    return true;
}

bool GnmMetalBridge::bind_texture_table(ShaderStage stage, std::uint32_t base_slot, std::uint64_t table_addr, std::uint32_t count) {
    if (base_slot + count > MetalResourceBindings::MaxTextures) return false;
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    if (!bindings) return false;

    GnmContext ctx(memory_);
    for (std::uint32_t i = 0; i < count; ++i) {
        TextureDescriptor desc;
        if (!ctx.read_texture_descriptor(table_addr + i * 32, desc)) {
            return false;
        }
        if (!desc.is_valid()) continue;
        if (!bindings->bind_texture(base_slot + i, desc)) return false;
    }
    return true;
}

bool GnmMetalBridge::bind_sampler_table(ShaderStage stage, std::uint32_t base_slot, std::uint64_t table_addr, std::uint32_t count) {
    if (base_slot + count > MetalResourceBindings::MaxSamplers) return false;
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    if (!bindings) return false;

    GnmContext ctx(memory_);
    for (std::uint32_t i = 0; i < count; ++i) {
        SamplerDescriptor desc;
        if (!ctx.read_sampler_descriptor(table_addr + i * 16, desc)) {
            return false;
        }
        if (!bindings->bind_sampler(base_slot + i, desc)) return false;
    }
    return true;
}

void GnmMetalBridge::import_user_data_buffers(const GnmContext& context, ShaderStage stage) {
    MetalResourceBindings* bindings = nullptr;
    switch (stage) {
        case ShaderStage::Vertex: bindings = &vs_bindings_; break;
        case ShaderStage::Pixel: bindings = &ps_bindings_; break;
        case ShaderStage::Compute: bindings = &cs_bindings_; break;
    }
    if (!bindings) return;

    // Extrair até 4 descritores de buffer inline (slots 0,4,8,12)
    for (std::uint32_t base_slot = 0; base_slot < 16; base_slot += 4) {
        BufferDescriptor desc;
        if (context.extract_buffer_from_user_data(stage, base_slot, desc)) {
            bindings->bind_buffer(base_slot / 4, desc);
        }
    }
}

void GnmMetalBridge::clear_bindings(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: vs_bindings_.clear(); break;
        case ShaderStage::Pixel: ps_bindings_.clear(); break;
        case ShaderStage::Compute: cs_bindings_.clear(); break;
    }
}

const MetalResourceBindings& GnmMetalBridge::bindings(ShaderStage stage) const {
    switch (stage) {
        case ShaderStage::Vertex: return vs_bindings_;
        case ShaderStage::Pixel: return ps_bindings_;
        case ShaderStage::Compute: return cs_bindings_;
    }
    return vs_bindings_;
}

bool GnmMetalBridge::is_buffer_address_valid(std::uint64_t address, std::size_t size) const {
    if (address == 0 || size == 0) return false;
    // Verificar overflow
    if (address + size < address) return false;
    // Verificar leitura no início
    std::uint8_t byte;
    MemoryError err = memory_.load(address, &byte, 1);
    if (err != MemoryError::ok) return false;
    // Verificar leitura no fim (último byte)
    err = memory_.load(address + size - 1, &byte, 1);
    return err == MemoryError::ok;
}

BridgeResult<MetalDrawCommand> GnmMetalBridge::process_draw_call(const DrawCall& draw_call) {
    MetalDrawCommand cmd{};

    // Validar VS
    if (draw_call.pipeline_state.vs_program_address == 0) {
        return {BridgeError::InvalidShaderAddress, "vertex shader address is zero"};
    }

    auto vs_result = shader_cache_.get_or_compile(memory_, draw_call.pipeline_state.vs_program_address, ShaderStage::Vertex);
    if (!vs_result) {
        return {vs_result.error, vs_result.message};
    }
    cmd.pipeline_config.vs_hash = vs_result.value->hash;
    cmd.pipeline_config.vs_msl = vs_result.value->msl_source;

    // Validar PS (pode ser null para passagens apenas de depth)
    if (draw_call.pipeline_state.ps_program_address != 0) {
        auto ps_result = shader_cache_.get_or_compile(memory_, draw_call.pipeline_state.ps_program_address, ShaderStage::Pixel);
        if (!ps_result) {
            return {ps_result.error, ps_result.message};
        }
        cmd.pipeline_config.ps_hash = ps_result.value->hash;
        cmd.pipeline_config.ps_msl = ps_result.value->msl_source;
    }

    // Validar index buffer (se indexado)
    if (draw_call.indexed) {
        if (draw_call.index_buffer_address == 0 || draw_call.index_count == 0) {
            return {BridgeError::InvalidIndexBuffer, "index buffer address is zero or count is zero"};
        }
        std::size_t index_stride = (draw_call.index_type == GnmIndexType::Index16) ? 2 : 4;
        std::size_t index_size = draw_call.index_count * index_stride;
        if (!is_buffer_address_valid(draw_call.index_buffer_address, index_size)) {
            return {BridgeError::InvalidIndexBuffer, "index buffer out of bounds or invalid"};
        }
        cmd.indexed = true;
        cmd.index_count = draw_call.index_count;
        cmd.index_buffer_address = draw_call.index_buffer_address;
        cmd.index_type = draw_call.index_type;
    } else {
        cmd.indexed = false;
        cmd.vertex_count = draw_call.vertex_count;
    }

    // Bindings de vértices (VS) e fragment (PS)
    cmd.vertex_bindings = vs_bindings_;
    cmd.fragment_bindings = ps_bindings_;

    // Configurar Render Targets (Color Attachments)
    const auto& rt_state = draw_call.pipeline_state.render_targets;
    std::uint32_t active_rt = 0;
    for (std::size_t i = 0; i < rt_state.targets.size(); ++i) {
        const auto& rt = rt_state.targets[i];
        if (!rt.is_valid()) continue;

        MetalColorAttachmentConfig& ca = cmd.pipeline_config.color_attachments[active_rt];
        ca.pixel_format = map_surface_format(rt.format);
        if (ca.pixel_format == MetalPixelFormat::Invalid) {
            return {BridgeError::PipelineConfigurationFailed, "unsupported render target format"};
        }

        const auto& blend_target = draw_call.pipeline_state.blend.targets[i];
        ca.blending_enabled = blend_target.blend_enable;
        ca.source_rgb_blend_factor = map_blend_factor(blend_target.color_src_factor);
        ca.destination_rgb_blend_factor = map_blend_factor(blend_target.color_dst_factor);
        ca.rgb_blend_operation = map_blend_op(blend_target.color_op);
        ca.source_alpha_blend_factor = map_blend_factor(blend_target.alpha_src_factor);
        ca.destination_alpha_blend_factor = map_blend_factor(blend_target.alpha_dst_factor);
        ca.alpha_blend_operation = map_blend_op(blend_target.alpha_op);
        ca.write_mask = static_cast<std::uint8_t>((rt_state.target_mask >> (i * 4)) & 0xF);

        active_rt++;
    }
    cmd.pipeline_config.active_color_attachments = active_rt;

    // Depth/Stencil
    const auto& ds_state = draw_call.pipeline_state.depth_stencil;
    cmd.pipeline_config.depth_stencil.depth_pixel_format = map_surface_format(ds_state.z_format);
    cmd.pipeline_config.depth_stencil.stencil_pixel_format = map_surface_format(ds_state.stencil_format);
    cmd.pipeline_config.depth_stencil.depth_test_enabled = ds_state.depth_test_enabled;
    cmd.pipeline_config.depth_stencil.depth_write_enabled = ds_state.depth_write_enabled;
    cmd.pipeline_config.depth_stencil.depth_compare_function = map_compare_func(ds_state.depth_func);
    cmd.pipeline_config.depth_stencil.stencil_test_enabled = ds_state.stencil_test_enabled;
    cmd.pipeline_config.depth_stencil.stencil_write_enabled = ds_state.stencil_write_enabled;
    cmd.pipeline_config.depth_stencil.stencil_compare_function = map_compare_func(ds_state.stencil_func);

    // Primitive Topology
    bool topology_supported = false;
    cmd.pipeline_config.primitive_type = map_primitive_type(draw_call.pipeline_state.primitive_type, topology_supported);
    if (!topology_supported) {
        return {BridgeError::InvalidTopology, "unsupported primitive topology"};
    }

    // Rasterizer
    cmd.pipeline_config.cull_mode = map_cull_mode(draw_call.pipeline_state.rasterizer.cull_mode);
    cmd.pipeline_config.front_face = map_front_face(draw_call.pipeline_state.rasterizer.front_face);

    // Viewport e Scissor
    cmd.viewport = draw_call.pipeline_state.viewports.viewports[0];
    cmd.scissor = draw_call.pipeline_state.scissors.scissors[0];

    // Instance info
    cmd.instance_count = draw_call.instance_count;
    cmd.start_instance = draw_call.start_instance;
    cmd.start_index = draw_call.start_index;

    cmd.valid = true;
    return {BridgeError::Ok, "", std::move(cmd)};
}

BridgeResult<MetalDispatchCommand> GnmMetalBridge::process_dispatch(const DispatchCall& dispatch_call) {
    MetalDispatchCommand cmd{};

    // Validar CS
    if (dispatch_call.pipeline_state.cs_program_address == 0) {
        return {BridgeError::InvalidShaderAddress, "compute shader address is zero"};
    }

    auto cs_result = shader_cache_.get_or_compile(memory_, dispatch_call.pipeline_state.cs_program_address, ShaderStage::Compute);
    if (!cs_result) {
        return {cs_result.error, cs_result.message};
    }
    cmd.pipeline_config.cs_hash = cs_result.value->hash;
    cmd.pipeline_config.cs_msl = cs_result.value->msl_source;

    // Bindings de compute
    cmd.compute_bindings = cs_bindings_;

    // Validar threadgroups
    if (dispatch_call.threadgroups_x == 0 || dispatch_call.threadgroups_y == 0 || dispatch_call.threadgroups_z == 0) {
        return {BridgeError::PipelineConfigurationFailed, "threadgroup dimensions cannot be zero"};
    }
    cmd.threadgroups_x = dispatch_call.threadgroups_x;
    cmd.threadgroups_y = dispatch_call.threadgroups_y;
    cmd.threadgroups_z = dispatch_call.threadgroups_z;

    // Threadgroup size padrão (64 threads = 1 wavefront)
    cmd.threads_per_threadgroup_x = 64;
    cmd.threads_per_threadgroup_y = 1;
    cmd.threads_per_threadgroup_z = 1;
    cmd.pipeline_config.threads_per_threadgroup_x = 64;
    cmd.pipeline_config.threads_per_threadgroup_y = 1;
    cmd.pipeline_config.threads_per_threadgroup_z = 1;

    cmd.valid = true;
    return {BridgeError::Ok, "", std::move(cmd)};
}

} // namespace bb