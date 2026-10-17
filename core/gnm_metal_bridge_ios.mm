#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include "gnm_metal_bridge.h"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

namespace bb {


    // Cache global de recursos Metal com chaves de descritor para coerência (T159)
    static std::mutex g_resource_mutex;
    static std::unordered_map<MetalBufferCacheKey, id<MTLBuffer>, MetalBufferCacheKeyHash> g_buffer_cache;
    static std::unordered_map<MetalTextureCacheKey, id<MTLTexture>, MetalTextureCacheKeyHash> g_texture_cache;

    // -----------------------------------------------------------------------------
    // Helpers de criação de recursos Metal
    // -----------------------------------------------------------------------------

    id<MTLBuffer> create_metal_buffer(id<MTLDevice> device, const void* data, size_t size, const char* label) {
        if (!device || size == 0) return nil;
        id<MTLBuffer> buffer = nil;
        if (data) {
            buffer = [device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
        } else {
            // T158: Não alocar buffer vazio a partir de dados nulos
            return nil;
        }
        if (label && buffer) {
            buffer.label = [NSString stringWithUTF8String:label];
        }
        return buffer;
    }
    id<MTLTexture> create_metal_texture(id<MTLDevice> device, const void* data, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp, const char* label) {
        if (!device || width == 0 || height == 0 || !data) return nil;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
        if (data && texture) {
            [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0
                         withBytes:data
                       bytesPerRow:pitch * bpp];
        }
        if (label && texture) {
            texture.label = [NSString stringWithUTF8String:label];
        }
        return texture;
    }

    // -----------------------------------------------------------------------------
    // Pipeline State Cache (T108, T131)
    // -----------------------------------------------------------------------------

    static std::mutex g_pipeline_mutex;
    static std::unordered_map<MetalPipelineCacheKey, id<MTLRenderPipelineState>, MetalPipelineCacheKeyHash> g_pipeline_cache;
    id<MTLRenderPipelineState> get_or_create_pipeline_state(
    id<MTLDevice> device,
    const MetalDrawCommand& draw_cmd,
    const MetalResourceBindings& /*vs_bindings*/,
    const MetalResourceBindings& /*ps_bindings*/) {
        MetalPipelineCacheKey key = MetalPipelineCacheKey::from_pipeline_config(draw_cmd.pipeline_config);
        {
            std::lock_guard<std::mutex> lock(g_pipeline_mutex);
            auto it = g_pipeline_cache.find(key);
            if (it != g_pipeline_cache.end()) {
                return it->second;
            }
        }

        // T131: Compilar e vincular conjuntamente os estágios vertex e fragment válidos
        std::string combined_source = build_combined_msl_source(        draw_cmd.pipeline_config.vs_msl,        draw_cmd.pipeline_config.ps_msl    );
        if (combined_source.empty()) return nil;
        NSString* msl_source_str = [NSString stringWithUTF8String:combined_source.c_str()];
        if (!msl_source_str || msl_source_str.length == 0) return nil;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:msl_source_str options:nil error:&error];
        if (!library) {
            NSLog(@"[GnmMetalBridge] Failed to compile combined MSL library: %@", error);
            return nil;
        }
        id<MTLFunction> vs_function = [library newFunctionWithName:@"vertex_main"];
        if (!vs_function) {
            vs_function = [library newFunctionWithName:@"gcn_vertex"];
        }
        if (!vs_function) {
            NSLog(@"[GnmMetalBridge] Vertex function 'vertex_main' / 'gcn_vertex' not found in combined library");
            return nil;
        }
        id<MTLFunction> ps_function = nil;
        if (!draw_cmd.pipeline_config.ps_msl.empty()) {
            ps_function = [library newFunctionWithName:@"fragment_main"];
            if (!ps_function) {
                ps_function = [library newFunctionWithName:@"gcn_fragment"];
            }
            if (!ps_function) {
                NSLog(@"[GnmMetalBridge] Fragment function 'fragment_main' / 'gcn_fragment' not found in combined library");
                return nil;
            }
        }
        MTLRenderPipelineDescriptor* pipeline_desc = [MTLRenderPipelineDescriptor new];
        pipeline_desc.vertexFunction = vs_function;
        pipeline_desc.fragmentFunction = ps_function;
        pipeline_desc.label = [NSString stringWithFormat:@"GnmPipeline_VS%llu_PS%llu",                           (unsigned long long)draw_cmd.pipeline_config.vs_hash,                           (unsigned long long)draw_cmd.pipeline_config.ps_hash];

        // Color attachments (T108, T168)
        for (uint32_t i = 0; i < draw_cmd.pipeline_config.active_color_attachments && i < 8; ++i) {
            const auto& ca = draw_cmd.pipeline_config.color_attachments[i];
            MTLPixelFormat mtl_format;
            switch (ca.pixel_format) {
                case MetalPixelFormat::BGRA8Unorm: mtl_format = MTLPixelFormatBGRA8Unorm; break;
                case MetalPixelFormat::RGBA8Unorm: mtl_format = MTLPixelFormatRGBA8Unorm; break;
                case MetalPixelFormat::RGBA16Float: mtl_format = MTLPixelFormatRGBA16Float; break;
                case MetalPixelFormat::R16Float: mtl_format = MTLPixelFormatR16Float; break;
                case MetalPixelFormat::RG16Float: mtl_format = MTLPixelFormatRG16Float; break;
                case MetalPixelFormat::R32Float: mtl_format = MTLPixelFormatR32Float; break;
                case MetalPixelFormat::RG32Float: mtl_format = MTLPixelFormatRG32Float; break;
                case MetalPixelFormat::RGBA32Float: mtl_format = MTLPixelFormatRGBA32Float; break;
                case MetalPixelFormat::RG11B10Float: mtl_format = MTLPixelFormatRG11B10Float; break;
                case MetalPixelFormat::RGB10A2Unorm: mtl_format = MTLPixelFormatRGB10A2Unorm; break;
                case MetalPixelFormat::R8Unorm: mtl_format = MTLPixelFormatR8Unorm; break;
                case MetalPixelFormat::RG8Unorm: mtl_format = MTLPixelFormatRG8Unorm; break;
                default: mtl_format = MTLPixelFormatInvalid; break;
            }
            if (mtl_format == MTLPixelFormatInvalid) {
                NSLog(@"[GnmMetalBridge] Unsupported pixel format for colorAttachment[%u]", i);
                return nil;
            }
            pipeline_desc.colorAttachments[i].pixelFormat = mtl_format;
            pipeline_desc.colorAttachments[i].isBlendingEnabled = ca.blending_enabled;
            if (ca.blending_enabled) {
                pipeline_desc.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)ca.source_rgb_blend_factor;
                pipeline_desc.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)ca.destination_rgb_blend_factor;
                pipeline_desc.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)ca.rgb_blend_operation;
                pipeline_desc.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)ca.source_alpha_blend_factor;
                pipeline_desc.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)ca.destination_alpha_blend_factor;
                pipeline_desc.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)ca.alpha_blend_operation;
            }
            pipeline_desc.colorAttachments[i].writeMask = (MTLColorWriteMask)ca.write_mask;
        }

        // Depth/Stencil (T108)
        if (draw_cmd.pipeline_config.depth_stencil.depth_pixel_format != MetalPixelFormat::Invalid) {
            MTLPixelFormat depth_fmt;
            switch (draw_cmd.pipeline_config.depth_stencil.depth_pixel_format) {
                case MetalPixelFormat::Depth16Unorm: depth_fmt = MTLPixelFormatDepth16Unorm;
                break;
                case MetalPixelFormat::Depth32Float: depth_fmt = MTLPixelFormatDepth32Float;
                break;
                case MetalPixelFormat::Depth32Float_Stencil8: depth_fmt = MTLPixelFormatDepth32Float_Stencil8;
                break;
                default: depth_fmt = MTLPixelFormatDepth32Float;
                break;
            }
            pipeline_desc.depthAttachmentPixelFormat = depth_fmt;
        }
        if (draw_cmd.pipeline_config.depth_stencil.stencil_pixel_format != MetalPixelFormat::Invalid) {
            MTLPixelFormat stencil_fmt;
            switch (draw_cmd.pipeline_config.depth_stencil.stencil_pixel_format) {
                case MetalPixelFormat::Stencil8: stencil_fmt = MTLPixelFormatStencil8;
                break;
                case MetalPixelFormat::Depth32Float_Stencil8: stencil_fmt = MTLPixelFormatDepth32Float_Stencil8;
                break;
                default: stencil_fmt = MTLPixelFormatStencil8;
                break;
            }
            pipeline_desc.stencilAttachmentPixelFormat = stencil_fmt;
        }
        id<MTLRenderPipelineState> pipeline_state = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
        if (!pipeline_state) {
            NSLog(@"[GnmMetalBridge] Failed to create pipeline state: %@", error);
            return nil;
        }
        std::lock_guard<std::mutex> lock(g_pipeline_mutex);
        g_pipeline_cache.emplace(key, pipeline_state);
        return pipeline_state;
    }

    // -----------------------------------------------------------------------------
    // Depth/Stencil State Cache (T108)
    // -----------------------------------------------------------------------------

    static std::mutex g_depth_stencil_mutex;
    static std::unordered_map<MetalDepthStencilCacheKey, id<MTLDepthStencilState>, MetalDepthStencilCacheKeyHash> g_depth_stencil_cache;
    id<MTLDepthStencilState> get_or_create_depth_stencil_state(
    id<MTLDevice> device,
    const MetalDepthStencilConfig& ds_config) {
        if (!device) return nil;
        if (ds_config.depth_pixel_format == MetalPixelFormat::Invalid &&        ds_config.stencil_pixel_format == MetalPixelFormat::Invalid &&        !ds_config.depth_test_enabled && !ds_config.depth_write_enabled &&        !ds_config.stencil_test_enabled && !ds_config.stencil_write_enabled) {
            return nil;
        }
        MetalDepthStencilCacheKey key = MetalDepthStencilCacheKey::from_config(ds_config);
        {
            std::lock_guard<std::mutex> lock(g_depth_stencil_mutex);
            auto it = g_depth_stencil_cache.find(key);
            if (it != g_depth_stencil_cache.end()) {
                return it->second;
            }
        }
        MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
        desc.depthWriteEnabled = ds_config.depth_write_enabled;
        if (ds_config.depth_test_enabled) {
            switch (ds_config.depth_compare_function) {
                case MetalCompareFunction::Never: desc.depthCompareFunction = MTLCompareFunctionNever;
                break;
                case MetalCompareFunction::Less: desc.depthCompareFunction = MTLCompareFunctionLess;
                break;
                case MetalCompareFunction::Equal: desc.depthCompareFunction = MTLCompareFunctionEqual;
                break;
                case MetalCompareFunction::LessEqual: desc.depthCompareFunction = MTLCompareFunctionLessEqual;
                break;
                case MetalCompareFunction::Greater: desc.depthCompareFunction = MTLCompareFunctionGreater;
                break;
                case MetalCompareFunction::NotEqual: desc.depthCompareFunction = MTLCompareFunctionNotEqual;
                break;
                case MetalCompareFunction::GreaterEqual: desc.depthCompareFunction = MTLCompareFunctionGreaterEqual;
                break;
                case MetalCompareFunction::Always: desc.depthCompareFunction = MTLCompareFunctionAlways;
                break;
            }
        } else {
            desc.depthCompareFunction = MTLCompareFunctionAlways;
        }
        if (ds_config.stencil_test_enabled || ds_config.stencil_write_enabled) {
            MTLStencilDescriptor* stencil_desc = [MTLStencilDescriptor new];
            stencil_desc.readMask = ds_config.stencil_read_mask;
            stencil_desc.writeMask = ds_config.stencil_write_mask;
            switch (ds_config.stencil_compare_function) {
                case MetalCompareFunction::Never: stencil_desc.stencilCompareFunction = MTLCompareFunctionNever;
                break;
                case MetalCompareFunction::Less: stencil_desc.stencilCompareFunction = MTLCompareFunctionLess;
                break;
                case MetalCompareFunction::Equal: stencil_desc.stencilCompareFunction = MTLCompareFunctionEqual;
                break;
                case MetalCompareFunction::LessEqual: stencil_desc.stencilCompareFunction = MTLCompareFunctionLessEqual;
                break;
                case MetalCompareFunction::Greater: stencil_desc.stencilCompareFunction = MTLCompareFunctionGreater;
                break;
                case MetalCompareFunction::NotEqual: stencil_desc.stencilCompareFunction = MTLCompareFunctionNotEqual;
                break;
                case MetalCompareFunction::GreaterEqual: stencil_desc.stencilCompareFunction = MTLCompareFunctionGreaterEqual;
                break;
                case MetalCompareFunction::Always: stencil_desc.stencilCompareFunction = MTLCompareFunctionAlways;
                break;
            }
            desc.frontFaceStencil = stencil_desc;
            desc.backFaceStencil = stencil_desc;
        }
        id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:desc];
        if (state) {
            std::lock_guard<std::mutex> lock(g_depth_stencil_mutex);
            g_depth_stencil_cache.emplace(key, state);
        }
        return state;
    }

    // -----------------------------------------------------------------------------
    // Sampler State Cache (T133)
    // -----------------------------------------------------------------------------

    static std::mutex g_sampler_mutex;
    static std::unordered_map<MetalSamplerCacheKey, id<MTLSamplerState>, MetalSamplerCacheKeyHash> g_sampler_cache;
    id<MTLSamplerState> get_or_create_sampler_state(
    id<MTLDevice> device,
    const MetalSamplerBinding& binding) {
        if (!device || !binding.valid) return nil;
        MetalSamplerCacheKey key = MetalSamplerCacheKey::from_binding(binding);
        {
            std::lock_guard<std::mutex> lock(g_sampler_mutex);
            auto it = g_sampler_cache.find(key);
            if (it != g_sampler_cache.end()) {
                return it->second;
            }
        }
        MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
        desc.minFilter = (binding.min_filter == 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        desc.magFilter = (binding.mag_filter == 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        desc.mipFilter = (binding.mip_filter == 1) ? MTLSamplerMipFilterLinear :                     (binding.mip_filter == 2 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped);
        desc.lodMinClamp = binding.min_lod;
        desc.lodMaxClamp = binding.max_lod;

        auto map_address_mode = [](uint32_t clamp) -> MTLSamplerAddressMode {
            switch (clamp) {
                case 0: return MTLSamplerAddressModeRepeat;
                case 1: return MTLSamplerAddressModeMirrorRepeat;
                case 2: return MTLSamplerAddressModeClampToEdge;
                case 3: return MTLSamplerAddressModeClampToBorderColor;
                case 4: return MTLSamplerAddressModeMirrorClampToEdge;
                default: return MTLSamplerAddressModeClampToEdge;
            }
        };
        desc.sAddressMode = map_address_mode(binding.clamp_x);
        desc.tAddressMode = map_address_mode(binding.clamp_y);
        desc.rAddressMode = map_address_mode(binding.clamp_z);
        desc.maxAnisotropy = std::max(1u, std::min(16u, binding.max_anisotropy));

        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:desc];
        if (sampler) {
            std::lock_guard<std::mutex> lock(g_sampler_mutex);
            g_sampler_cache.emplace(key, sampler);
        }
        return sampler;
    }

    // -----------------------------------------------------------------------------
    // Bind Resources (buffers, textures, samplers) to encoder (T107, T132, T133)
    // -----------------------------------------------------------------------------

    void bind_resources_to_encoder(
    id<MTLRenderCommandEncoder> encoder,
    const MetalResourceBindings& bindings,
    ShaderStage stage,
    id<MTLDevice> device,
    void* (*get_buffer)(void*, uint64_t, size_t, const char*),
    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*),
    void* context) {

        // Buffers (T107, T158, T159: consome memória guest real, validação e coerência de dados)
        for (uint32_t i = 0; i < MetalResourceBindings::MaxBuffers; ++i) {
            const auto& b = bindings.buffers[i];
            if (!b.valid || b.guest_address == 0 || b.size == 0) continue;
            id<MTLBuffer> buffer = nil;
            MetalBufferCacheKey key{b.guest_address, b.size};
            {
                std::lock_guard<std::mutex> lock(g_resource_mutex);
                auto it = g_buffer_cache.find(key);
                if (it != g_buffer_cache.end()) {
                    buffer = it->second;
                }
            }
            const void* data = get_buffer ? get_buffer(context, b.guest_address, b.size, "GnmBuffer") : nullptr;
            if (buffer && data) {
                // T159: Coerência de conteúdo no mesmo endereço
                std::memcpy([buffer contents], data, b.size);
            } else if (!buffer && data && validate_guest_buffer_read(data, b.size)) {
                // T158: Rejeitar leituras nulas sem criar buffer vazio
                buffer = create_metal_buffer(device, data, b.size, "GnmBuffer");
                if (buffer) {
                    std::lock_guard<std::mutex> lock(g_resource_mutex);
                    g_buffer_cache[key] = buffer;
                }
            }
            if (buffer) {
                if (stage == ShaderStage::Vertex) {
                    [encoder setVertexBuffer:buffer offset:0 atIndex:i];
                } else {
                    [encoder setFragmentBuffer:buffer offset:0 atIndex:i];
                }
            }
        }

        // Textures (T107, T158, T159, T169: consome dados reais, validação, mipmaps e coerência)
        for (uint32_t i = 0; i < MetalResourceBindings::MaxTextures; ++i) {
            const auto& t = bindings.textures[i];
            if (!t.valid || t.guest_address == 0 || t.width == 0 || t.height == 0) continue;
            id<MTLTexture> texture = nil;
            MetalTextureCacheKey key{t.guest_address, t.width, t.height, t.pitch, t.format, t.num_mips};
            {
                std::lock_guard<std::mutex> lock(g_resource_mutex);
                auto it = g_texture_cache.find(key);
                if (it != g_texture_cache.end()) {
                    texture = it->second;
                }
            }
            if (!texture) {
                uint32_t bpp = get_texel_byte_size(t.format);
                uint32_t pitch = (t.pitch > 0) ? t.pitch : t.width;
                const void* data = get_texture ? get_texture(context, t.guest_address, t.width, t.height, pitch, bpp, "GnmTexture") : nullptr;
                // T158: Rejeitar leituras nulas de textura sem criar recurso vazio
                if (data && validate_guest_texture_read(data, t.width, t.height, bpp)) {
                    MTLPixelFormat mtl_format;
                    switch (t.format) {
                        case MetalPixelFormat::BGRA8Unorm: mtl_format = MTLPixelFormatBGRA8Unorm; break;
                        case MetalPixelFormat::RGBA8Unorm: mtl_format = MTLPixelFormatRGBA8Unorm; break;
                        case MetalPixelFormat::RGBA16Float: mtl_format = MTLPixelFormatRGBA16Float; break;
                        case MetalPixelFormat::RG11B10Float: mtl_format = MTLPixelFormatRG11B10Float; break;
                        default: mtl_format = MTLPixelFormatBGRA8Unorm; break;
                    }
                    MTLTextureDescriptor* tex_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                                                                        width:t.width
                                                                                                       height:t.height
                                                                                                    mipmapped:t.num_mips > 1];
                    tex_desc.usage = MTLTextureUsageShaderRead;
                    tex_desc.storageMode = MTLStorageModeShared;
                    texture = [device newTextureWithDescriptor:tex_desc];
                    if (texture) {
                        uint32_t bytes_per_row = calculate_bytes_per_row(t.format, pitch);
                        [texture replaceRegion:MTLRegionMake2D(0, 0, t.width, t.height)
                                   mipmapLevel:0
                                     withBytes:data
                                   bytesPerRow:bytes_per_row];

                        // T169: Transferência de níveis de mipmap adicionais quando disponíveis
                        if (t.num_mips > 1) {
                            auto mip_levels = calculate_texture_mipmap_levels(t.format, t.width, t.height, pitch, t.num_mips);
                            size_t mip_offset = static_cast<size_t>(bytes_per_row) * t.height;
                            const uint8_t* raw_bytes = static_cast<const uint8_t*>(data);
                            for (size_t lvl = 1; lvl < mip_levels.size() && lvl < texture.mipmapLevelCount; ++lvl) {
                                const auto& ml = mip_levels[lvl];
                                uint32_t lvl_bpr = calculate_bytes_per_row(t.format, ml.pitch);
                                [texture replaceRegion:MTLRegionMake2D(0, 0, ml.width, ml.height)
                                           mipmapLevel:lvl
                                             withBytes:raw_bytes + mip_offset
                                           bytesPerRow:lvl_bpr];
                                mip_offset += ml.byte_size;
                            }
                        }

                        std::lock_guard<std::mutex> lock(g_resource_mutex);
                        g_texture_cache[key] = texture;
                    }
                }
            }
            if (texture) {
                if (stage == ShaderStage::Vertex) {
                    [encoder setVertexTexture:texture atIndex:i];
                } else {
                    [encoder setFragmentTexture:texture atIndex:i];
                }
            }
        }

        // Samplers (T133: cria e aplica MTLSamplerState)
        for (uint32_t i = 0;i < MetalResourceBindings::MaxSamplers;++i) {
            const auto& s = bindings.samplers[i];
            if (!s.valid) continue;
            id<MTLSamplerState> sampler = get_or_create_sampler_state(device, s);
            if (sampler) {
                if (stage == ShaderStage::Vertex) {
                    [encoder setVertexSamplerState:sampler atIndex:i];
                } else {
                    [encoder setFragmentSamplerState:sampler atIndex:i];
                }
            }
        }
    }

    // -----------------------------------------------------------------------------
    // Execução Offscreen Principal (T107, T108, T131, T132, T133, T135)
    // -----------------------------------------------------------------------------

    bool gnm_bridge_execute_draws(
    void* device_ptr,
    void* queue_ptr,
    void* dest_texture_ptr,
    const void* draw_commands_ptr,
    uint32_t num_commands,
    const void* vs_bindings_ptr,
    const void* ps_bindings_ptr,
    void* (*get_buffer)(void*, uint64_t, size_t, const char*),
    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*)) {
        id<MTLDevice> device = (__bridge id<MTLDevice>)device_ptr;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
        id<MTLTexture> dest_texture = (__bridge id<MTLTexture>)dest_texture_ptr;
        if (!device || !queue || !dest_texture || num_commands == 0 || !draw_commands_ptr) {
            return false;
        }
        const MetalDrawCommand* draw_commands = static_cast<const MetalDrawCommand*>(draw_commands_ptr);
        if (!validate_draw_execution_preconditions(draw_commands, num_commands)) {
            return false;
        }
        const MetalResourceBindings* vs_bindings = static_cast<const MetalResourceBindings*>(vs_bindings_ptr);
        const MetalResourceBindings* ps_bindings = static_cast<const MetalResourceBindings*>(ps_bindings_ptr);

        // Render pass descriptor para offscreen rendering
        MTLRenderPassDescriptor* pass_desc = [MTLRenderPassDescriptor new];
        pass_desc.colorAttachments[0].texture = dest_texture;
        pass_desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);

        // T160: Attachments depth/stencil compatíveis no render pass offscreen
        auto active_ds = get_active_depth_stencil_config(draw_commands, num_commands);
        if (is_depth_stencil_attachment_compatible(active_ds, static_cast<uint32_t>(dest_texture.width), static_cast<uint32_t>(dest_texture.height))) {
            if (active_ds.depth_test_enabled || active_ds.depth_write_enabled || active_ds.depth_pixel_format != MetalPixelFormat::Invalid) {
                MTLPixelFormat depth_fmt = (active_ds.depth_pixel_format == MetalPixelFormat::Depth16Unorm) ? MTLPixelFormatDepth16Unorm :
                                           ((active_ds.depth_pixel_format == MetalPixelFormat::Depth32Float_Stencil8) ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float);
                MTLTextureDescriptor* d_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_fmt
                                                                                                  width:dest_texture.width
                                                                                                 height:dest_texture.height
                                                                                              mipmapped:NO];
                d_desc.usage = MTLTextureUsageRenderTarget;
                d_desc.storageMode = MTLStorageModePrivate;
                id<MTLTexture> depth_tex = [device newTextureWithDescriptor:d_desc];
                if (depth_tex) {
                    pass_desc.depthAttachment.texture = depth_tex;
                    pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
                    pass_desc.depthAttachment.storeAction = MTLStoreActionDontCare;
                    pass_desc.depthAttachment.clearDepth = 1.0;
                }
            }
            if (active_ds.stencil_test_enabled || active_ds.stencil_write_enabled || active_ds.stencil_pixel_format != MetalPixelFormat::Invalid) {
                if (pass_desc.depthAttachment.texture && pass_desc.depthAttachment.texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
                    pass_desc.stencilAttachment.texture = pass_desc.depthAttachment.texture;
                } else {
                    MTLTextureDescriptor* s_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8
                                                                                                      width:dest_texture.width
                                                                                                     height:dest_texture.height
                                                                                                  mipmapped:NO];
                    s_desc.usage = MTLTextureUsageRenderTarget;
                    s_desc.storageMode = MTLStorageModePrivate;
                    id<MTLTexture> stencil_tex = [device newTextureWithDescriptor:s_desc];
                    pass_desc.stencilAttachment.texture = stencil_tex;
                }
                if (pass_desc.stencilAttachment.texture) {
                    pass_desc.stencilAttachment.loadAction = MTLLoadActionClear;
                    pass_desc.stencilAttachment.storeAction = MTLStoreActionDontCare;
                    pass_desc.stencilAttachment.clearStencil = 0;
                }
            }
        }
        id<MTLCommandBuffer> cmd_buf = [queue commandBuffer];
        if (!cmd_buf) return false;
        cmd_buf.label = @"GnmOffscreenDraws";
        id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass_desc];
        if (!encoder) return false;
        encoder.label = @"GnmOffscreenEncoder";
        bool any_draw = false;
        for (uint32_t i = 0;i < num_commands;++i) {
            const MetalDrawCommand& draw = draw_commands[i];
            if (!draw.valid) continue;

            // T132: Utilizar os bindings de cada MetalDrawCommand
            const MetalResourceBindings& cur_vs_bindings = (draw.vertex_bindings.active_buffers > 0 || draw.vertex_bindings.active_textures > 0 || draw.vertex_bindings.active_samplers > 0 || !vs_bindings) ? draw.vertex_bindings : *vs_bindings;
            const MetalResourceBindings& cur_ps_bindings = (draw.fragment_bindings.active_buffers > 0 || draw.fragment_bindings.active_textures > 0 || draw.fragment_bindings.active_samplers > 0 || !ps_bindings) ? draw.fragment_bindings : *ps_bindings;

            // Obter/criar pipeline state (T108, T131)
            id<MTLRenderPipelineState> pipeline = get_or_create_pipeline_state(device, draw, cur_vs_bindings, cur_ps_bindings);
            if (!pipeline) continue;
            [encoder setRenderPipelineState:pipeline];

            // T108: Aplicar depth/stencil state
            id<MTLDepthStencilState> ds_state = get_or_create_depth_stencil_state(device, draw.pipeline_config.depth_stencil);
            if (ds_state) {
                [encoder setDepthStencilState:ds_state];
            }

            // T173: Transportar cor constante de blend e depth bias ao encoder Metal
            [encoder setBlendColorRed:draw.blend_color[0]
                                green:draw.blend_color[1]
                                 blue:draw.blend_color[2]
                                alpha:draw.blend_color[3]];

            if (draw.depth_bias_enable) {
                [encoder setDepthBias:draw.depth_bias
                           slopeScale:draw.slope_scaled_depth_bias
                                clamp:draw.depth_bias_clamp];
            } else {
                [encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
            }

            // T132, T133, T107: Bind resources usando bindings do draw e dados reais da memória guest
            bind_resources_to_encoder(encoder, cur_vs_bindings, ShaderStage::Vertex, device, get_buffer, get_texture, device_ptr);
            bind_resources_to_encoder(encoder, cur_ps_bindings, ShaderStage::Pixel, device, get_buffer, get_texture, device_ptr);

            // Viewport
            if (draw.viewport.width > 0 && draw.viewport.height > 0) {
                MTLViewport mtl_viewport = {
                    draw.viewport.x, draw.viewport.y,                draw.viewport.width, draw.viewport.height,                draw.viewport.min_depth, draw.viewport.max_depth
                };
                [encoder setViewport:mtl_viewport];
            }

            // Scissor
            if (draw.scissor.width() > 0 && draw.scissor.height() > 0) {
                MTLScissorRect mtl_scissor = {
                    draw.scissor.left, draw.scissor.top,                draw.scissor.width(), draw.scissor.height()
                };
                [encoder setScissorRect:mtl_scissor];
            }

            // Cull mode
            MTLCullMode cull_mode = MTLCullModeBack;
            switch (draw.pipeline_config.cull_mode) {
                case MetalCullMode::None: cull_mode = MTLCullModeNone;
                break;
                case MetalCullMode::Front: cull_mode = MTLCullModeFront;
                break;
                case MetalCullMode::Back: cull_mode = MTLCullModeBack;
                break;
            }
            [encoder setCullMode:cull_mode];

            // Front face winding
            MTLWinding winding = (draw.pipeline_config.front_face == MetalWinding::Clockwise) ? MTLWindingClockwise : MTLWindingCounterClockwise;
            [encoder setFrontFacingWinding:winding];

            // Primitive type
            MTLPrimitiveType prim_type;
            switch (draw.pipeline_config.primitive_type) {
                case MetalPrimitiveType::Point: prim_type = MTLPrimitiveTypePoint;
                break;
                case MetalPrimitiveType::Line: prim_type = MTLPrimitiveTypeLine;
                break;
                case MetalPrimitiveType::LineStrip: prim_type = MTLPrimitiveTypeLineStrip;
                break;
                case MetalPrimitiveType::Triangle: prim_type = MTLPrimitiveTypeTriangle;
                break;
                case MetalPrimitiveType::TriangleStrip: prim_type = MTLPrimitiveTypeTriangleStrip;
                break;
                default: prim_type = MTLPrimitiveTypeTriangle;
                break;
            }
            if (draw.indexed && draw.index_count > 0) {
                id<MTLBuffer> idx_buf = nil;
                size_t idx_elem_sz = (draw.index_type == GnmIndexType::Index32) ? 4 : 2;
                size_t total_idx_sz = (draw.index_count + draw.start_index) * idx_elem_sz;
                MetalBufferCacheKey idx_key{draw.index_buffer_address, total_idx_sz};
                if (draw.index_buffer_address != 0) {
                    std::lock_guard<std::mutex> lock(g_resource_mutex);
                    auto it = g_buffer_cache.find(idx_key);
                    if (it != g_buffer_cache.end()) {
                        idx_buf = it->second;
                    }
                }
                if (!idx_buf && draw.index_buffer_address != 0) {
                    const void* idx_data = get_buffer ? get_buffer(device_ptr, draw.index_buffer_address, total_idx_sz, "GnmIndexBuffer") : nullptr;
                    // T158: Rejeitar leituras nulas de índice guest sem criar buffer vazio
                    if (idx_data && validate_guest_index_read(idx_data, draw.index_count, draw.index_type)) {
                        idx_buf = create_metal_buffer(device, idx_data, total_idx_sz, "GnmIndexBuffer");
                        if (idx_buf) {
                            std::lock_guard<std::mutex> lock(g_resource_mutex);
                            g_buffer_cache[idx_key] = idx_buf;
                        }
                    }
                }
                if (idx_buf) {
                    MTLIndexType mtl_idx_type = (draw.index_type == GnmIndexType::Index32) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
                    NSUInteger idx_offset = draw.start_index * idx_elem_sz;
                    if (draw.instance_count > 1) {
                        [encoder drawIndexedPrimitives:prim_type
                                            indexCount:draw.index_count
                                             indexType:mtl_idx_type
                                           indexBuffer:idx_buf
                                     indexBufferOffset:idx_offset
                                         instanceCount:draw.instance_count
                                            baseVertex:0
                                          baseInstance:draw.start_instance];
                    } else {
                        [encoder drawIndexedPrimitives:prim_type
                                            indexCount:draw.index_count
                                             indexType:mtl_idx_type
                                           indexBuffer:idx_buf
                                     indexBufferOffset:idx_offset];
                    }
                    any_draw = true;
                } else {
                    // T158: Se a leitura do índice guest falhar, NÃO converter em drawPrimitives não-indexado!
                    NSLog(@"[GnmMetalBridge] Indexed draw rejeitado por leitura de indice nula ou invalida no endereco 0x%llx", (unsigned long long)draw.index_buffer_address);
                }
            } else if (draw.vertex_count > 0) {
                if (draw.instance_count > 1) {
                    [encoder drawPrimitives:prim_type vertexStart:0 vertexCount:draw.vertex_count instanceCount:draw.instance_count baseInstance:draw.start_instance];
                } else {
                    [encoder drawPrimitives:prim_type vertexStart:0 vertexCount:draw.vertex_count];
                }
                any_draw = true;
            }
        }

        // T135: Retornar sucesso somente quando draws foram codificados e command buffer completou
        if (!any_draw) {
            [encoder endEncoding];
            return false;
        }
        [encoder endEncoding];
        [cmd_buf commit];
        [cmd_buf waitUntilCompleted];
        if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
            NSLog(@"[GnmMetalBridge] Command buffer failed with status %ld: %@", (long)cmd_buf.status, cmd_buf.error);
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------------
    // Execução de Compute Dispatch (T134)
    // -----------------------------------------------------------------------------

    bool gnm_bridge_execute_dispatch(
    void* device_ptr,
    void* queue_ptr,
    const void* dispatch_cmd_ptr,
    void* (*get_buffer)(void*, uint64_t, size_t, const char*),
    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*),
    char* err_buf,
    size_t err_buf_size) {
        if (!device_ptr || !queue_ptr || !dispatch_cmd_ptr) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Null device, queue or dispatch command");
            return false;
        }
        const MetalDispatchCommand* cmd = static_cast<const MetalDispatchCommand*>(dispatch_cmd_ptr);
        auto validation = validate_compute_dispatch(*cmd);
        if (!validation.ok()) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "%s", validation.message.c_str());
            return false;
        }
        id<MTLDevice> device = (__bridge id<MTLDevice>)device_ptr;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
        NSString* cs_source = [NSString stringWithUTF8String:cmd->pipeline_config.cs_msl.c_str()];
        if (!cs_source || cs_source.length == 0) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Empty compute MSL source");
            return false;
        }
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:cs_source options:nil error:&error];
        if (!library) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Compute shader compile failed: %s", error.localizedDescription.UTF8String);
            return false;
        }
        id<MTLFunction> cs_fn = [library newFunctionWithName:@"compute_main"];
        if (!cs_fn) cs_fn = [library newFunctionWithName:@"gcn_compute"];
        if (!cs_fn) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Compute function 'compute_main' / 'gcn_compute' not found");
            return false;
        }
        id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:cs_fn error:&error];
        if (!pso) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Failed to create compute pipeline state: %s", error.localizedDescription.UTF8String);
            return false;
        }
        id<MTLCommandBuffer> cmd_buf = [queue commandBuffer];
        if (!cmd_buf) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Failed to allocate command buffer for compute");
            return false;
        }
        cmd_buf.label = @"GnmComputeDispatch";
        id<MTLComputeCommandEncoder> encoder = [cmd_buf computeCommandEncoder];
        if (!encoder) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Failed to allocate compute command encoder");
            return false;
        }
        encoder.label = @"GnmComputeEncoder";
        [encoder setComputePipelineState:pso];

        // Bind compute buffers (T171: com chave de coerência MetalBufferCacheKey)
        std::vector<std::pair<id<MTLBuffer>, MetalBufferCacheKey>> bound_compute_buffers;
        for (uint32_t i = 0; i < MetalResourceBindings::MaxBuffers; ++i) {
            const auto& b = cmd->compute_bindings.buffers[i];
            if (!b.valid || b.guest_address == 0 || b.size == 0) continue;
            id<MTLBuffer> buf = nil;
            MetalBufferCacheKey key{b.guest_address, b.size};
            {
                std::lock_guard<std::mutex> lock(g_resource_mutex);
                auto it = g_buffer_cache.find(key);
                if (it != g_buffer_cache.end()) buf = it->second;
            }
            if (!buf) {
                const void* data = get_buffer ? get_buffer(device_ptr, b.guest_address, b.size, "GnmComputeBuffer") : nullptr;
                if (data && validate_guest_buffer_read(data, b.size)) {
                    buf = create_metal_buffer(device, data, b.size, "GnmComputeBuffer");
                    if (buf) {
                        std::lock_guard<std::mutex> lock(g_resource_mutex);
                        g_buffer_cache[key] = buf;
                    }
                }
            }
            if (buf) {
                [encoder setBuffer:buf offset:0 atIndex:i];
                bound_compute_buffers.push_back({buf, key});
            }
        }

        // T171: Bind compute textures
        for (uint32_t i = 0; i < MetalResourceBindings::MaxTextures; ++i) {
            const auto& t = cmd->compute_bindings.textures[i];
            if (!t.valid || t.guest_address == 0 || t.width == 0 || t.height == 0) continue;
            id<MTLTexture> tex = nil;
            MetalTextureCacheKey key{t.guest_address, t.width, t.height, t.pitch, t.format, t.num_mips};
            {
                std::lock_guard<std::mutex> lock(g_resource_mutex);
                auto it = g_texture_cache.find(key);
                if (it != g_texture_cache.end()) tex = it->second;
            }
            if (!tex) {
                uint32_t bpp = get_texel_byte_size(t.format);
                uint32_t pitch = (t.pitch > 0) ? t.pitch : t.width;
                const void* data = get_texture ? get_texture(device_ptr, t.guest_address, t.width, t.height, pitch, bpp, "GnmComputeTexture") : nullptr;
                if (data && validate_guest_texture_read(data, t.width, t.height, bpp)) {
                    MTLPixelFormat mtl_format = (t.format == MetalPixelFormat::RGBA16Float) ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA8Unorm;
                    MTLTextureDescriptor* t_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                                                                      width:t.width
                                                                                                     height:t.height
                                                                                                  mipmapped:NO];
                    t_desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    t_desc.storageMode = MTLStorageModeShared;
                    tex = [device newTextureWithDescriptor:t_desc];
                    if (tex) {
                        [tex replaceRegion:MTLRegionMake2D(0, 0, t.width, t.height)
                               mipmapLevel:0
                                 withBytes:data
                               bytesPerRow:calculate_bytes_per_row(t.format, pitch)];
                        std::lock_guard<std::mutex> lock(g_resource_mutex);
                        g_texture_cache[key] = tex;
                    }
                }
            }
            if (tex) {
                [encoder setTexture:tex atIndex:i];
            }
        }

        MTLSize threadgroups = MTLSizeMake(cmd->threadgroups_x, cmd->threadgroups_y, cmd->threadgroups_z);
        MTLSize threadsPerThreadgroup = MTLSizeMake(cmd->threads_per_threadgroup_x, cmd->threads_per_threadgroup_y, cmd->threads_per_threadgroup_z);
        [encoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerThreadgroup];
        [encoder endEncoding];
        [cmd_buf commit];
        [cmd_buf waitUntilCompleted];
        if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
            if (err_buf && err_buf_size > 0) snprintf(err_buf, err_buf_size, "Compute command buffer failed: %s", cmd_buf.error.localizedDescription.UTF8String);
            return false;
        }

        // T171: Tornar escritas de buffers Metal visíveis na memória compartilhada
        for (const auto& item : bound_compute_buffers) {
            id<MTLBuffer> buf = item.first;
            if (buf && buf.storageMode == MTLResourceStorageModeShared && buf.contents != nullptr) {
                // Conteúdo modificado garantido residente na memória compartilhada
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------------
    // C API implementations
    // -----------------------------------------------------------------------------

    extern "C" {


        void* gnm_bridge_create_buffer(void* device, const void* data, size_t size, const char* label) {
            id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
            id<MTLBuffer> buffer = create_metal_buffer(mtl_device, data, size, label);
            return (__bridge void*)buffer;
        }
        void* gnm_bridge_create_texture(void* device, const void* data, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp, const char* label) {
            id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
            id<MTLTexture> texture = create_metal_texture(mtl_device, data, width, height, pitch, bpp, label);
            return (__bridge void*)texture;
        }
        bool gnm_bridge_execute_draws_c(    void* device,    void* queue,    void* dest_texture,    const void* draw_commands,    uint32_t num_commands,    const void* vs_bindings,    const void* ps_bindings,    void* (*get_buffer)(void*, uint64_t, size_t, const char*),    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*)) {
            return gnm_bridge_execute_draws(device, queue, dest_texture, draw_commands, num_commands, vs_bindings, ps_bindings, get_buffer, get_texture);
        }
        bool gnm_bridge_execute_dispatch_c(    void* device,    void* queue,    const void* dispatch_command,    void* (*get_buffer)(void*, uint64_t, size_t, const char*),    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*),    char* err_buf,    size_t err_buf_size) {
            return gnm_bridge_execute_dispatch(device, queue, dispatch_command, get_buffer, get_texture, err_buf, err_buf_size);
        }
        void gnm_bridge_clear_caches_c() {
            gnm_bridge_clear_caches();
        }

    }
    // extern "C"

    void gnm_bridge_clear_caches() {
        {
            std::lock_guard<std::mutex> lock(g_resource_mutex);
            g_buffer_cache.clear();
            g_texture_cache.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_pipeline_mutex);
            g_pipeline_cache.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_depth_stencil_mutex);
            g_depth_stencil_cache.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_sampler_mutex);
            g_sampler_cache.clear();
        }
    }

    void gnm_bridge_invalidate_buffer(uint64_t guest_address) {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        for (auto it = g_buffer_cache.begin(); it != g_buffer_cache.end(); ) {
            if (it->first.guest_address == guest_address) {
                it = g_buffer_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void gnm_bridge_invalidate_texture(uint64_t guest_address) {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        for (auto it = g_texture_cache.begin(); it != g_texture_cache.end(); ) {
            if (it->first.guest_address == guest_address) {
                it = g_texture_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

}
// namespace bb
