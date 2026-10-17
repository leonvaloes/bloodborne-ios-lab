#include "gnm_metal_bridge.h"
#include "guest_memory.h"
#include "shader_translator.h"
#include <algorithm>
#include <array>
#include <cmath>
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
    samplers[slot].clamp_x = desc.clamp_x;
    samplers[slot].clamp_y = desc.clamp_y;
    samplers[slot].clamp_z = desc.clamp_z;
    samplers[slot].max_anisotropy = std::min(16u, 1u << (desc.aniso_ratio & 0x7));
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

BridgeResult<std::shared_ptr<const CachedShader>> ShaderCache::get_or_compile(
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
    // O mesmo bytecode traduzido para VS/PS/CS produz assinaturas MSL diferentes.
    // Incorpora o estágio à chave sem perder o hash de conteúdo usado no pipeline.
    std::uint64_t hash = calculate_hash(words.data(), words.size() * sizeof(std::uint32_t));
    const std::uint64_t stage_key = hash ^
        (0x9e3779b97f4a7c15ULL * (static_cast<std::uint64_t>(stage) + 1));

    // 3. Verificar cache
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(stage_key);
        if (it != cache_.end()) {
            hit_count_++;
            return {BridgeError::Ok, "", it->second};
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
    cached.hash = stage_key;
    cached.stage = stage;
    cached.msl_source = std::move(msl_result.text);
    cached.instruction_count = static_cast<std::uint32_t>(prog.instructions.size());
    cached.byte_size = words.size() * sizeof(std::uint32_t);

    auto shader_ptr = std::make_shared<const CachedShader>(std::move(cached));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.size() >= max_entries_) {
            // Eviction simples: limpa tudo quando cheio (política FIFO/LRU simplificada)
            cache_.clear();
        }
        auto [it, inserted] = cache_.emplace(stage_key, std::move(shader_ptr));
        return {BridgeError::Ok, "", it->second};
    }
}

void ShaderCache::insert(std::uint64_t hash, CachedShader shader) {
    insert(hash, std::make_shared<const CachedShader>(std::move(shader)));
}

void ShaderCache::insert(std::uint64_t hash, std::shared_ptr<const CachedShader> shader) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() >= max_entries_) {
        cache_.clear();
    }
    cache_[hash] = std::move(shader);
}

std::shared_ptr<const CachedShader> ShaderCache::find(std::uint64_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(hash);
    return (it != cache_.end()) ? it->second : nullptr;
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

CachedShader GnmMetalBridge::get_fallback_shader(ShaderStage stage) {
    CachedShader cs;
    cs.stage = stage;
    switch (stage) {
        case ShaderStage::Vertex:
            cs.hash = 0xFB000001ULL;
            cs.msl_source = ShaderTranslator::fallback_geometry_vertex_msl();
            cs.instruction_count = 16;
            cs.byte_size = cs.msl_source.size();
            cs.bound_buffers = {0, 1};
            break;
        case ShaderStage::Pixel:
            cs.hash = 0xFB000002ULL;
            cs.msl_source = ShaderTranslator::fallback_geometry_fragment_msl();
            cs.instruction_count = 16;
            cs.byte_size = cs.msl_source.size();
            cs.bound_textures = {0};
            cs.bound_samplers = {0};
            break;
        case ShaderStage::Compute:
            cs.hash = 0xFB000003ULL;
            cs.msl_source = "#include <metal_stdlib>\nusing namespace metal;\nkernel void cs_fallback(uint tid [[thread_position_in_grid]]) {}\n";
            cs.instruction_count = 4;
            cs.byte_size = cs.msl_source.size();
            break;
    }
    return cs;
}

static bool is_valid_color_render_target_format(MetalPixelFormat format) {
    switch (format) {
        case MetalPixelFormat::BGRA8Unorm:
        case MetalPixelFormat::RGBA8Unorm:
        case MetalPixelFormat::RGBA16Float:
        case MetalPixelFormat::R16Float:
        case MetalPixelFormat::RG16Float:
        case MetalPixelFormat::R32Float:
        case MetalPixelFormat::RG32Float:
        case MetalPixelFormat::RGBA32Float:
        case MetalPixelFormat::RG11B10Float:
        case MetalPixelFormat::RGB10A2Unorm:
        case MetalPixelFormat::R8Unorm:
        case MetalPixelFormat::RG8Unorm:
            return true;
        default:
            return false;
    }
}

BridgeResult<MetalDrawCommand> GnmMetalBridge::process_draw_call(const DrawCall& draw_call) {
    MetalDrawCommand cmd{};

    // Validar VS
    if (draw_call.pipeline_state.vs_program_address == 0) {
        if (allow_fallback_) {
            CachedShader fb = get_fallback_shader(ShaderStage::Vertex);
            cmd.pipeline_config.vs_hash = fb.hash;
            cmd.pipeline_config.vs_msl = fb.msl_source;
        } else {
            return {BridgeError::InvalidShaderAddress, "vertex shader address is zero"};
        }
    } else {
        auto vs_result = shader_cache_.get_or_compile(memory_, draw_call.pipeline_state.vs_program_address, ShaderStage::Vertex);
        if (!vs_result) {
            if (allow_fallback_) {
                CachedShader fb = get_fallback_shader(ShaderStage::Vertex);
                cmd.pipeline_config.vs_hash = fb.hash;
                cmd.pipeline_config.vs_msl = fb.msl_source;
            } else {
                return {vs_result.error, vs_result.message};
            }
        } else {
            cmd.pipeline_config.vs_hash = vs_result.value->hash;
            cmd.pipeline_config.vs_msl = vs_result.value->msl_source;
        }
    }

    // Validar PS (pode ser null para passagens apenas de depth)
    if (draw_call.pipeline_state.ps_program_address != 0) {
        auto ps_result = shader_cache_.get_or_compile(memory_, draw_call.pipeline_state.ps_program_address, ShaderStage::Pixel);
        if (!ps_result) {
            if (allow_fallback_) {
                CachedShader fb = get_fallback_shader(ShaderStage::Pixel);
                cmd.pipeline_config.ps_hash = fb.hash;
                cmd.pipeline_config.ps_msl = fb.msl_source;
            } else {
                return {ps_result.error, ps_result.message};
            }
        } else {
            cmd.pipeline_config.ps_hash = ps_result.value->hash;
            cmd.pipeline_config.ps_msl = ps_result.value->msl_source;
        }
    } else if (allow_fallback_) {
        CachedShader fb = get_fallback_shader(ShaderStage::Pixel);
        cmd.pipeline_config.ps_hash = fb.hash;
        cmd.pipeline_config.ps_msl = fb.msl_source;
    }

    // Validar index buffer (se indexado)
    if (draw_call.indexed) {
        if (draw_call.index_buffer_address == 0 || draw_call.index_count == 0 ||
            draw_call.start_index > UINT32_MAX - draw_call.index_count) {
            return {BridgeError::InvalidIndexBuffer, "index buffer address is zero or count is zero"};
        }
        std::size_t index_stride = (draw_call.index_type == GnmIndexType::Index16) ? 2 : 4;
        std::size_t index_size = static_cast<std::size_t>(draw_call.index_count + draw_call.start_index) * index_stride;
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

    // Recusar bindings não mapeados antes que um backend tente lê-los.
    for (const auto* bindings : {&vs_bindings_, &ps_bindings_}) {
        for (const auto& buffer : bindings->buffers) {
            if (buffer.valid && !is_buffer_address_valid(buffer.guest_address, buffer.size)) {
                return {BridgeError::InvalidBufferAddress, "vertex/fragment buffer unmapped or invalid"};
            }
        }
    }
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

    // Validação de Múltiplos Render Targets (MRT) e formatos (T168)
    if (active_rt == 0) {
        // Passagem somente profundidade (ex: shadow map / Z-prepass)
        if (cmd.pipeline_config.depth_stencil.depth_pixel_format == MetalPixelFormat::Invalid) {
            return {BridgeError::PipelineConfigurationFailed, "draw call has no active color targets and no depth attachment"};
        }
    } else {
        if (active_rt > 8) {
            return {BridgeError::PipelineConfigurationFailed, "exceeded maximum supported render targets (8)"};
        }
        for (std::uint32_t i = 0; i < active_rt; ++i) {
            if (!is_valid_color_render_target_format(cmd.pipeline_config.color_attachments[i].pixel_format)) {
                return {BridgeError::PipelineConfigurationFailed,
                        "unsupported color render target format at attachment " + std::to_string(i)};
            }
        }
        if (cmd.pipeline_config.ps_msl.empty()) {
            return {BridgeError::PipelineConfigurationFailed, "pixel shader MSL source is empty for color pass"};
        }
    }

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

    // Cor constante de blend e depth bias (T173)
    cmd.blend_color = draw_call.pipeline_state.blend.blend_color;
    cmd.depth_bias_enable = draw_call.pipeline_state.rasterizer.depth_bias_enable;
    cmd.depth_bias = draw_call.pipeline_state.rasterizer.depth_bias;
    cmd.slope_scaled_depth_bias = draw_call.pipeline_state.rasterizer.slope_scaled_depth_bias;
    cmd.depth_bias_clamp = draw_call.pipeline_state.rasterizer.depth_bias_clamp;

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

// =============================================================================
// Chaves de Cache Metal e Validações (T108, T131, T132, T133, T134, T135)
// =============================================================================

static inline void hash_combine(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

bool MetalPipelineCacheKey::operator==(const MetalPipelineCacheKey& o) const {
    if (vs_hash != o.vs_hash || ps_hash != o.ps_hash || active_color_attachments != o.active_color_attachments ||
        depth_format != o.depth_format || stencil_format != o.stencil_format || primitive_type != o.primitive_type) {
        return false;
    }
    for (std::uint32_t i = 0; i < active_color_attachments && i < 8; ++i) {
        if (color_formats[i] != o.color_formats[i] ||
            blend_enabled[i] != o.blend_enabled[i] ||
            write_mask[i] != o.write_mask[i]) {
            return false;
        }
        if (blend_enabled[i]) {
            if (src_rgb_blend[i] != o.src_rgb_blend[i] || dst_rgb_blend[i] != o.dst_rgb_blend[i] ||
                rgb_blend_op[i] != o.rgb_blend_op[i] || src_alpha_blend[i] != o.src_alpha_blend[i] ||
                dst_alpha_blend[i] != o.dst_alpha_blend[i] || alpha_blend_op[i] != o.alpha_blend_op[i]) {
                return false;
            }
        }
    }
    return true;
}

MetalPipelineCacheKey MetalPipelineCacheKey::from_pipeline_config(const MetalRenderPipelineConfig& cfg) {
    MetalPipelineCacheKey key{};
    key.vs_hash = cfg.vs_hash;
    key.ps_hash = cfg.ps_hash;
    key.active_color_attachments = cfg.active_color_attachments;
    for (std::uint32_t i = 0; i < cfg.active_color_attachments && i < 8; ++i) {
        const auto& ca = cfg.color_attachments[i];
        key.color_formats[i] = static_cast<std::uint32_t>(ca.pixel_format);
        key.blend_enabled[i] = ca.blending_enabled;
        key.src_rgb_blend[i] = static_cast<std::uint32_t>(ca.source_rgb_blend_factor);
        key.dst_rgb_blend[i] = static_cast<std::uint32_t>(ca.destination_rgb_blend_factor);
        key.rgb_blend_op[i] = static_cast<std::uint32_t>(ca.rgb_blend_operation);
        key.src_alpha_blend[i] = static_cast<std::uint32_t>(ca.source_alpha_blend_factor);
        key.dst_alpha_blend[i] = static_cast<std::uint32_t>(ca.destination_alpha_blend_factor);
        key.alpha_blend_op[i] = static_cast<std::uint32_t>(ca.alpha_blend_operation);
        key.write_mask[i] = ca.write_mask;
    }
    key.depth_format = static_cast<std::uint32_t>(cfg.depth_stencil.depth_pixel_format);
    key.stencil_format = static_cast<std::uint32_t>(cfg.depth_stencil.stencil_pixel_format);
    key.primitive_type = static_cast<std::uint32_t>(cfg.primitive_type);
    return key;
}

std::size_t MetalPipelineCacheKeyHash::operator()(const MetalPipelineCacheKey& k) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, std::hash<std::uint64_t>()(k.vs_hash));
    hash_combine(seed, std::hash<std::uint64_t>()(k.ps_hash));
    hash_combine(seed, std::hash<std::uint32_t>()(k.active_color_attachments));
    hash_combine(seed, std::hash<std::uint32_t>()(k.depth_format));
    hash_combine(seed, std::hash<std::uint32_t>()(k.stencil_format));
    hash_combine(seed, std::hash<std::uint32_t>()(k.primitive_type));
    for (std::uint32_t i = 0; i < k.active_color_attachments && i < 8; ++i) {
        hash_combine(seed, std::hash<std::uint32_t>()(k.color_formats[i]));
        hash_combine(seed, std::hash<bool>()(k.blend_enabled[i]));
        hash_combine(seed, std::hash<std::uint8_t>()(k.write_mask[i]));
        if (k.blend_enabled[i]) {
            hash_combine(seed, std::hash<std::uint32_t>()(k.src_rgb_blend[i]));
            hash_combine(seed, std::hash<std::uint32_t>()(k.dst_rgb_blend[i]));
            hash_combine(seed, std::hash<std::uint32_t>()(k.rgb_blend_op[i]));
        }
    }
    return seed;
}

bool MetalDepthStencilCacheKey::operator==(const MetalDepthStencilCacheKey& o) const {
    return depth_pixel_format == o.depth_pixel_format &&
           stencil_pixel_format == o.stencil_pixel_format &&
           depth_test_enabled == o.depth_test_enabled &&
           depth_write_enabled == o.depth_write_enabled &&
           depth_compare_function == o.depth_compare_function &&
           stencil_test_enabled == o.stencil_test_enabled &&
           stencil_write_enabled == o.stencil_write_enabled &&
           stencil_compare_function == o.stencil_compare_function &&
           stencil_read_mask == o.stencil_read_mask &&
           stencil_write_mask == o.stencil_write_mask;
}

MetalDepthStencilCacheKey MetalDepthStencilCacheKey::from_config(const MetalDepthStencilConfig& cfg) {
    MetalDepthStencilCacheKey key{};
    key.depth_pixel_format = static_cast<std::uint32_t>(cfg.depth_pixel_format);
    key.stencil_pixel_format = static_cast<std::uint32_t>(cfg.stencil_pixel_format);
    key.depth_test_enabled = cfg.depth_test_enabled;
    key.depth_write_enabled = cfg.depth_write_enabled;
    key.depth_compare_function = static_cast<std::uint32_t>(cfg.depth_compare_function);
    key.stencil_test_enabled = cfg.stencil_test_enabled;
    key.stencil_write_enabled = cfg.stencil_write_enabled;
    key.stencil_compare_function = static_cast<std::uint32_t>(cfg.stencil_compare_function);
    key.stencil_read_mask = cfg.stencil_read_mask;
    key.stencil_write_mask = cfg.stencil_write_mask;
    return key;
}

std::size_t MetalDepthStencilCacheKeyHash::operator()(const MetalDepthStencilCacheKey& k) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, std::hash<std::uint32_t>()(k.depth_pixel_format));
    hash_combine(seed, std::hash<std::uint32_t>()(k.stencil_pixel_format));
    hash_combine(seed, std::hash<bool>()(k.depth_test_enabled));
    hash_combine(seed, std::hash<bool>()(k.depth_write_enabled));
    hash_combine(seed, std::hash<std::uint32_t>()(k.depth_compare_function));
    hash_combine(seed, std::hash<bool>()(k.stencil_test_enabled));
    hash_combine(seed, std::hash<bool>()(k.stencil_write_enabled));
    hash_combine(seed, std::hash<std::uint32_t>()(k.stencil_compare_function));
    hash_combine(seed, std::hash<std::uint8_t>()(k.stencil_read_mask));
    hash_combine(seed, std::hash<std::uint8_t>()(k.stencil_write_mask));
    return seed;
}

bool MetalSamplerCacheKey::operator==(const MetalSamplerCacheKey& o) const {
    constexpr float kEps = 1e-4f;
    return min_filter == o.min_filter &&
           mag_filter == o.mag_filter &&
           mip_filter == o.mip_filter &&
           clamp_x == o.clamp_x &&
           clamp_y == o.clamp_y &&
           clamp_z == o.clamp_z &&
           max_anisotropy == o.max_anisotropy &&
           std::fabs(min_lod - o.min_lod) < kEps &&
           std::fabs(max_lod - o.max_lod) < kEps;
}

MetalSamplerCacheKey MetalSamplerCacheKey::from_binding(const MetalSamplerBinding& b) {
    MetalSamplerCacheKey key{};
    key.min_filter = b.min_filter;
    key.mag_filter = b.mag_filter;
    key.mip_filter = b.mip_filter;
    key.min_lod = b.min_lod;
    key.max_lod = b.max_lod;
    key.clamp_x = b.clamp_x;
    key.clamp_y = b.clamp_y;
    key.clamp_z = b.clamp_z;
    key.max_anisotropy = b.max_anisotropy;
    return key;
}

std::size_t MetalSamplerCacheKeyHash::operator()(const MetalSamplerCacheKey& k) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, std::hash<std::uint32_t>()(k.min_filter));
    hash_combine(seed, std::hash<std::uint32_t>()(k.mag_filter));
    hash_combine(seed, std::hash<std::uint32_t>()(k.mip_filter));
    hash_combine(seed, std::hash<std::uint32_t>()(k.clamp_x));
    hash_combine(seed, std::hash<std::uint32_t>()(k.clamp_y));
    hash_combine(seed, std::hash<std::uint32_t>()(k.clamp_z));
    hash_combine(seed, std::hash<std::uint32_t>()(k.max_anisotropy));
    hash_combine(seed, std::hash<int>()(static_cast<int>(k.min_lod * 100.0f)));
    hash_combine(seed, std::hash<int>()(static_cast<int>(k.max_lod * 100.0f)));
    return seed;
}

std::uint32_t get_texel_byte_size(MetalPixelFormat format) {
    switch (format) {
        case MetalPixelFormat::RGBA16Float:
            return 8;
        case MetalPixelFormat::RGBA8Unorm:
        case MetalPixelFormat::BGRA8Unorm:
        case MetalPixelFormat::RG11B10Float:
        case MetalPixelFormat::Depth32Float:
        case MetalPixelFormat::Depth32Float_Stencil8:
            return 4;
        case MetalPixelFormat::Depth16Unorm:
            return 2;
        case MetalPixelFormat::Stencil8:
            return 1;
        default:
            return 4;
    }
}

std::uint32_t calculate_bytes_per_row(MetalPixelFormat format, std::uint32_t pitch) {
    const std::uint32_t bpp = get_texel_byte_size(format);
    return pitch * bpp;
}

bool validate_texture_upload_bounds(MetalPixelFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, std::size_t buffer_size) {
    if (width == 0 || height == 0 || pitch < width || width > 16384 || height > 16384 || pitch > 16384) {
        return false;
    }
    const std::uint32_t bpp = get_texel_byte_size(format);
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(pitch) * bpp;
    const std::uint64_t total_bytes = row_bytes * height;
    if (total_bytes > buffer_size) {
        return false;
    }
    return true;
}

std::string build_combined_msl_source(const std::string& vs_msl, const std::string& ps_msl) {
    if (vs_msl.empty()) {
        return "";
    }
    if (ps_msl.empty()) {
        return vs_msl;
    }
    std::string combined;
    combined.reserve(vs_msl.size() + ps_msl.size() + 2);
    combined.append(vs_msl);
    combined.append("\n\n");
    combined.append(ps_msl);
    return combined;
}

BridgeResult<bool> validate_compute_dispatch(const MetalDispatchCommand& cmd) {
    if (!cmd.valid) {
        return {BridgeError::PipelineConfigurationFailed, "compute dispatch command is marked invalid"};
    }
    if (cmd.pipeline_config.cs_hash == 0 || cmd.pipeline_config.cs_msl.empty()) {
        return {BridgeError::InvalidShaderAddress, "compute shader MSL or hash is missing"};
    }
    if (cmd.threadgroups_x == 0 || cmd.threadgroups_y == 0 || cmd.threadgroups_z == 0) {
        return {BridgeError::PipelineConfigurationFailed, "threadgroup dimensions cannot be zero"};
    }
    if (cmd.threads_per_threadgroup_x == 0 || cmd.threads_per_threadgroup_y == 0 || cmd.threads_per_threadgroup_z == 0) {
        return {BridgeError::PipelineConfigurationFailed, "threads per threadgroup dimensions cannot be zero"};
    }
    return {BridgeError::Ok, "", true};
}

bool validate_draw_execution_preconditions(const MetalDrawCommand* commands, std::uint32_t count) {
    if (!commands || count == 0) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (commands[i].valid) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Implementações de Coerência de Cache e Validação (T158, T159, T160, T169, T171)
// =============================================================================

bool MetalBufferCacheKey::operator==(const MetalBufferCacheKey& o) const noexcept {
    return guest_address == o.guest_address && size == o.size;
}

std::size_t MetalBufferCacheKeyHash::operator()(const MetalBufferCacheKey& k) const noexcept {
    std::size_t h = std::hash<std::uint64_t>{}(k.guest_address);
    h ^= std::hash<std::size_t>{}(k.size) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

bool MetalTextureCacheKey::operator==(const MetalTextureCacheKey& o) const noexcept {
    return guest_address == o.guest_address &&
           width == o.width &&
           height == o.height &&
           pitch == o.pitch &&
           format == o.format &&
           num_mips == o.num_mips;
}

std::size_t MetalTextureCacheKeyHash::operator()(const MetalTextureCacheKey& k) const noexcept {
    std::size_t h = std::hash<std::uint64_t>{}(k.guest_address);
    h ^= std::hash<std::uint32_t>{}(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint32_t>{}(k.pitch) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(k.format)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint32_t>{}(k.num_mips) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

bool validate_guest_buffer_read(const void* data, std::size_t size) {
    return data != nullptr && size > 0;
}

bool validate_guest_texture_read(const void* data, std::uint32_t width, std::uint32_t height, std::uint32_t bpp) {
    return data != nullptr && width > 0 && height > 0 && bpp > 0;
}

bool validate_guest_index_read(const void* data, std::size_t index_count, GnmIndexType type) {
    if (data == nullptr || index_count == 0) return false;
    if (type != GnmIndexType::Index16 && type != GnmIndexType::Index32) return false;
    return true;
}

MetalDepthStencilConfig get_active_depth_stencil_config(const MetalDrawCommand* commands, std::uint32_t count) {
    MetalDepthStencilConfig cfg{};
    if (!commands || count == 0) return cfg;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!commands[i].valid) continue;
        const auto& ds = commands[i].pipeline_config.depth_stencil;
        if (ds.depth_test_enabled || ds.depth_write_enabled || ds.depth_pixel_format != MetalPixelFormat::Invalid) {
            cfg.depth_test_enabled = cfg.depth_test_enabled || ds.depth_test_enabled;
            cfg.depth_write_enabled = cfg.depth_write_enabled || ds.depth_write_enabled;
            if (cfg.depth_pixel_format == MetalPixelFormat::Invalid) {
                cfg.depth_pixel_format = ds.depth_pixel_format;
            }
            cfg.depth_compare_function = ds.depth_compare_function;
        }
        if (ds.stencil_test_enabled || ds.stencil_write_enabled || ds.stencil_pixel_format != MetalPixelFormat::Invalid) {
            cfg.stencil_test_enabled = cfg.stencil_test_enabled || ds.stencil_test_enabled;
            cfg.stencil_write_enabled = cfg.stencil_write_enabled || ds.stencil_write_enabled;
            if (cfg.stencil_pixel_format == MetalPixelFormat::Invalid) {
                cfg.stencil_pixel_format = ds.stencil_pixel_format;
            }
            cfg.stencil_compare_function = ds.stencil_compare_function;
            cfg.stencil_read_mask = ds.stencil_read_mask;
            cfg.stencil_write_mask = ds.stencil_write_mask;
        }
    }
    return cfg;
}

bool is_depth_stencil_attachment_compatible(const MetalDepthStencilConfig& ds, std::uint32_t dest_width, std::uint32_t dest_height) {
    if (dest_width == 0 || dest_height == 0) return false;
    if (ds.depth_test_enabled || ds.depth_write_enabled) {
        if (ds.depth_pixel_format != MetalPixelFormat::Depth16Unorm &&
            ds.depth_pixel_format != MetalPixelFormat::Depth32Float &&
            ds.depth_pixel_format != MetalPixelFormat::Depth32Float_Stencil8) {
            return false;
        }
    }
    if (ds.stencil_test_enabled || ds.stencil_write_enabled) {
        if (ds.stencil_pixel_format != MetalPixelFormat::Stencil8 &&
            ds.stencil_pixel_format != MetalPixelFormat::Depth32Float_Stencil8) {
            return false;
        }
    }
    return true;
}

std::vector<MipLevelBounds> calculate_texture_mipmap_levels(
    MetalPixelFormat format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    std::uint32_t num_mips
) {
    std::vector<MipLevelBounds> levels;
    if (width == 0 || height == 0 || num_mips == 0) return levels;
    const std::uint32_t bpp = get_texel_byte_size(format);
    levels.reserve(num_mips);
    std::uint32_t cur_w = width;
    std::uint32_t cur_h = height;
    std::uint32_t cur_pitch = (pitch > 0) ? pitch : width;
    for (std::uint32_t lvl = 0; lvl < num_mips; ++lvl) {
        MipLevelBounds b{};
        b.level = lvl;
        b.width = std::max(1u, cur_w);
        b.height = std::max(1u, cur_h);
        b.pitch = std::max(1u, cur_pitch);
        b.byte_size = static_cast<std::size_t>(b.pitch) * b.height * bpp;
        levels.push_back(b);
        cur_w = (cur_w > 1) ? (cur_w >> 1) : 1;
        cur_h = (cur_h > 1) ? (cur_h >> 1) : 1;
        cur_pitch = (cur_pitch > 1) ? (cur_pitch >> 1) : 1;
        if (cur_pitch < cur_w) cur_pitch = cur_w;
    }
    return levels;
}

bool should_clamp_sampler_lod(std::uint32_t texture_mips, std::uint32_t initialized_mips) {
    return (texture_mips > 1 && initialized_mips <= 1);
}

BridgeResult<bool> validate_compute_resources(
    const MetalDispatchCommand& cmd,
    bool has_textures,
    bool has_buffers
) {
    auto base_val = validate_compute_dispatch(cmd);
    if (!base_val.ok()) return base_val;
    if (cmd.compute_bindings.active_buffers > 0 && !has_buffers) {
        return {BridgeError::PipelineConfigurationFailed, "active compute buffers present without bound storage"};
    }
    if (cmd.compute_bindings.active_textures > 0 && !has_textures) {
        return {BridgeError::PipelineConfigurationFailed, "active compute textures present without bound storage"};
    }
    return {BridgeError::Ok, "", true};
}

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__APPLE__) || !TARGET_OS_IPHONE
void gnm_bridge_clear_caches() {
    // Stub em plataformas host sem runtime nativo Metal (Windows/Linux/macOS-host)
}
void gnm_bridge_invalidate_buffer(std::uint64_t /*guest_address*/) {
    // Stub em plataformas host sem runtime nativo Metal (Windows/Linux/macOS-host)
}
void gnm_bridge_invalidate_texture(std::uint64_t /*guest_address*/) {
    // Stub em plataformas host sem runtime nativo Metal (Windows/Linux/macOS-host)
}
#endif

} // namespace bb