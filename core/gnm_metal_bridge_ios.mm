#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include "gnm_metal_bridge.h"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

namespace bb {

// Cache global de recursos Metal (guest_address -> resource)
static std::mutex g_resource_mutex;
static std::unordered_map<uint64_t, id<MTLBuffer>> g_buffer_cache;
static std::unordered_map<uint64_t, id<MTLTexture>> g_texture_cache;

// -----------------------------------------------------------------------------
// Helpers de criação de recursos Metal
// -----------------------------------------------------------------------------

id<MTLBuffer> create_metal_buffer(id<MTLDevice> device, const void* data, size_t size, const char* label) {
    if (!device || size == 0) return nil;
    
    id<MTLBuffer> buffer;
    if (data) {
        buffer = [device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    } else {
        buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    }
    
    if (label && buffer) {
        buffer.label = [NSString stringWithUTF8String:label];
    }
    return buffer;
}

id<MTLTexture> create_metal_texture(id<MTLDevice> device, const void* data, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp, const char* label) {
    if (!device || width == 0 || height == 0) return nil;
    
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                   width:width
                                                                                  height:height
                                                                               mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    
    if (data && texture) {
        // Upload inicial se houver dados
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
// Pipeline State Cache (VS+PS hash -> MTLRenderPipelineState)
// -----------------------------------------------------------------------------

struct PipelineCacheKey {
    uint64_t vs_hash;
    uint64_t ps_hash;
    
    bool operator==(const PipelineCacheKey& other) const {
        return vs_hash == other.vs_hash && ps_hash == other.ps_hash;
    }
};

struct PipelineCacheKeyHash {
    size_t operator()(const PipelineCacheKey& k) const noexcept {
        return std::hash<uint64_t>()(k.vs_hash ^ (k.ps_hash << 1));
    }
};

static std::mutex g_pipeline_mutex;
static std::unordered_map<PipelineCacheKey, id<MTLRenderPipelineState>, PipelineCacheKeyHash> g_pipeline_cache;

id<MTLRenderPipelineState> get_or_create_pipeline_state(
    id<MTLDevice> device,
    const MetalDrawCommand& draw_cmd,
    const MetalResourceBindings& vs_bindings,
    const MetalResourceBindings& ps_bindings
) {
    PipelineCacheKey key{draw_cmd.pipeline_config.vs_hash, draw_cmd.pipeline_config.ps_hash};
    
    {
        std::lock_guard<std::mutex> lock(g_pipeline_mutex);
        auto it = g_pipeline_cache.find(key);
        if (it != g_pipeline_cache.end()) {
            return it->second;
        }
    }
    
    // Compilar MSL sources
    NSString* vs_source = [NSString stringWithUTF8String:draw_cmd.pipeline_config.vs_msl.c_str()];
    NSString* ps_source = draw_cmd.pipeline_config.ps_msl.empty() ? nil : [NSString stringWithUTF8String:draw_cmd.pipeline_config.ps_msl.c_str()];
    
    if (!vs_source || vs_source.length == 0) return nil;
    
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:vs_source options:nil error:&error];
    if (!library) {
        // Tentar com PS source também
        if (ps_source) {
            NSString* combined = [NSString stringWithFormat:@"%@\n%@", vs_source, ps_source];
            library = [device newLibraryWithSource:combined options:nil error:&error];
        }
        if (!library) {
            NSLog(@"[GnmMetalBridge] Failed to compile MSL library: %@", error);
            return nil;
        }
    }
    
    id<MTLFunction> vs_function = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> ps_function = nil;
    if (ps_source) {
        ps_function = [library newFunctionWithName:@"fragment_main"];
    }
    
    if (!vs_function) {
        NSLog(@"[GnmMetalBridge] Vertex function 'vertex_main' not found");
        return nil;
    }
    
    MTLRenderPipelineDescriptor* pipeline_desc = [MTLRenderPipelineDescriptor new];
    pipeline_desc.vertexFunction = vs_function;
    pipeline_desc.fragmentFunction = ps_function;
    pipeline_desc.label = [NSString stringWithFormat:@"GnmPipeline_VS%llu_PS%llu", 
                           (unsigned long long)draw_cmd.pipeline_config.vs_hash,
                           (unsigned long long)draw_cmd.pipeline_config.ps_hash];
    
    // Color attachments
    MTLRenderPassAttachmentDescriptor* color_attachments[8] = {0};
    for (uint32_t i = 0; i < draw_cmd.pipeline_config.active_color_attachments && i < 8; ++i) {
        const auto& ca = draw_cmd.pipeline_config.color_attachments[i];
        MTLPixelFormat mtl_format;
        switch (ca.pixel_format) {
            case MetalPixelFormat::BGRA8Unorm: mtl_format = MTLPixelFormatBGRA8Unorm; break;
            case MetalPixelFormat::RGBA8Unorm: mtl_format = MTLPixelFormatRGBA8Unorm; break;
            case MetalPixelFormat::RGBA16Float: mtl_format = MTLPixelFormatRGBA16Float; break;
            case MetalPixelFormat::RG11B10Float: mtl_format = MTLPixelFormatRG11B10Float; break;
            default: mtl_format = MTLPixelFormatBGRA8Unorm; break;
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
    
    // Depth/Stencil
    if (draw_cmd.pipeline_config.depth_stencil.depth_pixel_format != MetalPixelFormat::Invalid) {
        MTLPixelFormat depth_fmt;
        switch (draw_cmd.pipeline_config.depth_stencil.depth_pixel_format) {
            case MetalPixelFormat::Depth16Unorm: depth_fmt = MTLPixelFormatDepth16Unorm; break;
            case MetalPixelFormat::Depth32Float: depth_fmt = MTLPixelFormatDepth32Float; break;
            case MetalPixelFormat::Depth32Float_Stencil8: depth_fmt = MTLPixelFormatDepth32Float_Stencil8; break;
            default: depth_fmt = MTLPixelFormatDepth32Float; break;
        }
        pipeline_desc.depthAttachmentPixelFormat = depth_fmt;
    }
    if (draw_cmd.pipeline_config.depth_stencil.stencil_pixel_format != MetalPixelFormat::Invalid) {
        MTLPixelFormat stencil_fmt;
        switch (draw_cmd.pipeline_config.depth_stencil.stencil_pixel_format) {
            case MetalPixelFormat::Stencil8: stencil_fmt = MTLPixelFormatStencil8; break;
            case MetalPixelFormat::Depth32Float_Stencil8: stencil_fmt = MTLPixelFormatDepth32Float_Stencil8; break;
            default: stencil_fmt = MTLPixelFormatStencil8; break;
        }
        pipeline_desc.stencilAttachmentPixelFormat = stencil_fmt;
    }
    
    // Rasterizer
    MTLCullMode cull_mode = MTLCullModeBack;
    switch (draw_cmd.pipeline_config.cull_mode) {
        case MetalCullMode::None: cull_mode = MTLCullModeNone; break;
        case MetalCullMode::Front: cull_mode = MTLCullModeFront; break;
        case MetalCullMode::Back: cull_mode = MTLCullModeBack; break;
    }
    MTLWinding winding = (draw_cmd.pipeline_config.front_face == MetalWinding::Clockwise) ? MTLWindingClockwise : MTLWindingCounterClockwise;
    
    MTLRenderPipelineState* pipeline_state = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if (!pipeline_state) {
        NSLog(@"[GnmMetalBridge] Failed to create pipeline state: %@", error);
        return nil;
    }
    
    std::lock_guard<std::mutex> lock(g_pipeline_mutex);
    g_pipeline_cache.emplace(key, pipeline_state);
    return pipeline_state;
}

// -----------------------------------------------------------------------------
// Bind Resources (buffers, textures, samplers) to encoder
// -----------------------------------------------------------------------------

void bind_resources_to_encoder(
    id<MTLRenderCommandEncoder> encoder,
    const MetalResourceBindings& bindings,
    ShaderStage stage,
    id<MTLDevice> device
) {
    // Buffers
    for (uint32_t i = 0; i < MetalResourceBindings::MaxBuffers; ++i) {
        const auto& b = bindings.buffers[i];
        if (!b.valid) continue;
        
        // Buscar ou criar buffer
        id<MTLBuffer> buffer = nil;
        {
            std::lock_guard<std::mutex> lock(g_resource_mutex);
            auto it = g_buffer_cache.find(b.guest_address);
            if (it != g_buffer_cache.end()) {
                buffer = it->second;
            }
        }
        
        if (!buffer && b.guest_address != 0) {
            // Nota: em implementação real, leríamos da memória guest
            // Por ora, criamos buffer vazio do tamanho correto
            buffer = create_metal_buffer(device, nullptr, b.size, "GnmBuffer");
            if (buffer) {
                std::lock_guard<std::mutex> lock(g_resource_mutex);
                g_buffer_cache[b.guest_address] = buffer;
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
    
    // Textures
    for (uint32_t i = 0; i < MetalResourceBindings::MaxTextures; ++i) {
        const auto& t = bindings.textures[i];
        if (!t.valid) continue;
        
        id<MTLTexture> texture = nil;
        {
            std::lock_guard<std::mutex> lock(g_resource_mutex);
            auto it = g_texture_cache.find(t.guest_address);
            if (it != g_texture_cache.end()) {
                texture = it->second;
            }
        }
        
        if (!texture && t.guest_address != 0 && t.width > 0 && t.height > 0) {
            // Criar textura (dados serão lidos da memória guest no host)
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
                std::lock_guard<std::mutex> lock(g_resource_mutex);
                g_texture_cache[t.guest_address] = texture;
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
    
    // Samplers - Metal usa sampler states, não bindings diretos como texturas
    // Por simplicidade, usamos um sampler padrão
}

// -----------------------------------------------------------------------------
// Execução Offscreen Principal
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
    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*)
) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_ptr;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
    id<MTLTexture> dest_texture = (__bridge id<MTLTexture>)dest_texture_ptr;
    
    if (!device || !queue || !dest_texture || num_commands == 0) {
        return false;
    }
    
    const MetalDrawCommand* draw_commands = static_cast<const MetalDrawCommand*>(draw_commands_ptr);
    const MetalResourceBindings* vs_bindings = static_cast<const MetalResourceBindings*>(vs_bindings_ptr);
    const MetalResourceBindings* ps_bindings = static_cast<const MetalResourceBindings*>(ps_bindings_ptr);
    
    // Criar render pass descriptor para offscreen rendering
    MTLRenderPassDescriptor* pass_desc = [MTLRenderPassDescriptor new];
    
    // Color attachment 0 -> dest_texture
    pass_desc.colorAttachments[0].texture = dest_texture;
    pass_desc.colorAttachments[0].loadAction = MTLLoadActionLoad;  // Preservar conteúdo anterior
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    
    // Depth attachment se necessário
    // (simplificado: sem depth por enquanto)
    
    id<MTLCommandBuffer> cmd_buf = [queue commandBuffer];
    if (!cmd_buf) return false;
    
    cmd_buf.label = @"GnmOffscreenDraws";
    
    id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass_desc];
    if (!encoder) return false;
    
    encoder.label = @"GnmOffscreenEncoder";
    
    bool any_draw = false;
    
    for (uint32_t i = 0; i < num_commands; ++i) {
        const MetalDrawCommand& draw = draw_commands[i];
        if (!draw.valid) continue;
        
        // Obter/criar pipeline state
        id<MTLRenderPipelineState> pipeline = get_or_create_pipeline_state(device, draw, *vs_bindings, *ps_bindings);
        if (!pipeline) continue;
        
        [encoder setRenderPipelineState:pipeline];
        
        // Bind resources
        bind_resources_to_encoder(encoder, *vs_bindings, ShaderStage::Vertex, device);
        bind_resources_to_encoder(encoder, *ps_bindings, ShaderStage::Pixel, device);
        
        // Viewport
        if (draw.viewport.width > 0 && draw.viewport.height > 0) {
            MTLViewport mtl_viewport = {
                draw.viewport.x, draw.viewport.y,
                draw.viewport.width, draw.viewport.height,
                draw.viewport.min_depth, draw.viewport.max_depth
            };
            [encoder setViewport:mtl_viewport];
        }
        
        // Scissor
        if (draw.scissor.width() > 0 && draw.scissor.height() > 0) {
            MTLScissorRect mtl_scissor = {
                draw.scissor.left, draw.scissor.top,
                draw.scissor.width(), draw.scissor.height()
            };
            [encoder setScissorRect:mtl_scissor];
        }
        
        // Cull mode
        MTLCullMode cull_mode = MTLCullModeBack;
        switch (draw.pipeline_config.cull_mode) {
            case MetalCullMode::None: cull_mode = MTLCullModeNone; break;
            case MetalCullMode::Front: cull_mode = MTLCullModeFront; break;
            case MetalCullMode::Back: cull_mode = MTLCullModeBack; break;
        }
        [encoder setCullMode:cull_mode];
        
        // Front face winding
        MTLWinding winding = (draw.pipeline_config.front_face == MetalWinding::Clockwise) ? MTLWindingClockwise : MTLWindingCounterClockwise;
        [encoder setFrontFacingWinding:winding];
        
        // Draw call
        MTLPrimitiveType prim_type;
        switch (draw.pipeline_config.primitive_type) {
            case MetalPrimitiveType::Point: prim_type = MTLPrimitiveTypePoint; break;
            case MetalPrimitiveType::Line: prim_type = MTLPrimitiveTypeLine; break;
            case MetalPrimitiveType::LineStrip: prim_type = MTLPrimitiveTypeLineStrip; break;
            case MetalPrimitiveType::Triangle: prim_type = MTLPrimitiveTypeTriangle; break;
            case MetalPrimitiveType::TriangleStrip: prim_type = MTLPrimitiveTypeTriangleStrip; break;
            default: prim_type = MTLPrimitiveTypeTriangle; break;
        }
        
        if (draw.indexed) {
            // Index buffer - precisaríamos criar MTLBuffer do index_buffer_address
            // Por enquanto, draw não-indexado com vertex_count
            [encoder drawPrimitives:prim_type vertexStart:0 vertexCount:draw.vertex_count];
        } else {
            [encoder drawPrimitives:prim_type vertexStart:0 vertexCount:draw.vertex_count];
        }
        
        any_draw = true;
    }
    
    [encoder endEncoding];
    [cmd_buf commit];
    [cmd_buf waitUntilCompleted];
    
    return any_draw;
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

bool gnm_bridge_execute_draws_c(
    void* device,
    void* queue,
    void* dest_texture,
    const void* draw_commands,
    uint32_t num_commands,
    const void* vs_bindings,
    const void* ps_bindings,
    void* (*get_buffer)(void*, uint64_t, size_t, const char*),
    void* (*get_texture)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*)
) {
    return gnm_bridge_execute_draws(device, queue, dest_texture, draw_commands, num_commands, vs_bindings, ps_bindings, get_buffer, get_texture);
}

} // extern "C"

} // namespace bb