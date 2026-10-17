#import "MetalGameView.h"
#import <Metal/Metal.h>

@implementation MetalGameView {
    bb::VideoOutManager *_videoMgr;
    id<MTLRenderPipelineState> _screenPipeline;
    id<MTLSamplerState> _screenSampler;
    id<MTLTexture> _guestScreenTexture;
    bb::PresentationFrame _cachedPresFrame;
    uint64_t _presentedFrameCount;
    id<MTLDepthStencilState> _depthState;
    id<MTLDepthStencilState> _depthStateReadOnly;
}

- (instancetype)initWithFrame:(CGRect)frame device:(id<MTLDevice>)device {
    self = [super initWithFrame:frame device:device];
    if (self) {
        _presentedFrameCount = 0;
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        self.clearColor = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
        self.clearDepth = 1.0;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        _cameraFieldOfView = 60.0f * (M_PI / 180.0f);
        _cameraNearZ = 0.1f;
        _cameraFarZ = 1000.0f;
        [self setupPipelineWithDevice:device];
        [self setupDepthBufferWithDevice:device];
    }
    return self;
}

- (void)setupPipelineWithDevice:(id<MTLDevice>)device {
    if (!device) return;

    NSString *shaderSource = @""
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct VertexOut {\n"
        "    float4 position [[position]];\n"
        "    float2 uv;\n"
        "};\n"
        "vertex VertexOut screenVertex(uint vid [[vertex_id]]) {\n"
        "    VertexOut out;\n"
        "    float2 uv = float2((vid << 1) & 2, vid & 2);\n"
        "    out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
        "    out.uv = float2(uv.x, 1.0 - uv.y);\n"
        "    return out;\n"
        "}\n"
        "fragment float4 screenFragment(VertexOut in [[stage_in]],\n"
        "                               texture2d<float> tex [[texture(0)]],\n"
        "                               sampler s [[sampler(0)]]) {\n"
        "    return tex.sample(s, in.uv);\n"
        "}\n";

    NSError *error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:shaderSource options:nil error:&error];
    if (!lib) {
        if ([self.gameViewDelegate respondsToSelector:@selector(metalGameView:didFailWithError:)]) {
            [self.gameViewDelegate metalGameView:self didFailWithError:error.localizedDescription];
        }
        return;
    }

    MTLRenderPipelineDescriptor *pDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pDesc.vertexFunction = [lib newFunctionWithName:@"screenVertex"];
    pDesc.fragmentFunction = [lib newFunctionWithName:@"screenFragment"];
    pDesc.colorAttachments[0].pixelFormat = self.colorPixelFormat;
    pDesc.depthAttachmentPixelFormat = self.depthStencilPixelFormat;

    _screenPipeline = [device newRenderPipelineStateWithDescriptor:pDesc error:&error];
    if (!_screenPipeline) {
        if ([self.gameViewDelegate respondsToSelector:@selector(metalGameView:didFailWithError:)]) {
            [self.gameViewDelegate metalGameView:self didFailWithError:error.localizedDescription];
        }
        return;
    }

    MTLSamplerDescriptor *sDesc = [[MTLSamplerDescriptor alloc] init];
    sDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _screenSampler = [device newSamplerStateWithDescriptor:sDesc];
}

- (void)setupDepthBufferWithDevice:(id<MTLDevice>)device {
    if (!device) return;

    MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
    desc.depthCompareFunction = MTLCompareFunctionLessEqual;
    desc.depthWriteEnabled = YES;
    _depthState = [device newDepthStencilStateWithDescriptor:desc];

    MTLDepthStencilDescriptor *readOnlyDesc = [[MTLDepthStencilDescriptor alloc] init];
    readOnlyDesc.depthCompareFunction = MTLCompareFunctionLessEqual;
    readOnlyDesc.depthWriteEnabled = NO;
    _depthStateReadOnly = [device newDepthStencilStateWithDescriptor:readOnlyDesc];
}

- (void)configureDepthTestingForEncoder:(id<MTLRenderCommandEncoder>)encoder enabled:(BOOL)enabled write:(BOOL)write {
    if (!encoder) return;
    if (!enabled) {
        [encoder setDepthStencilState:nil];
    } else if (write) {
        [encoder setDepthStencilState:_depthState];
    } else {
        [encoder setDepthStencilState:_depthStateReadOnly];
    }
}

- (id<MTLDepthStencilState>)depthStencilState {
    return _depthState;
}

- (id<MTLDepthStencilState>)depthStencilStateReadOnly {
    return _depthStateReadOnly;
}

- (void)bindVideoOutManager:(bb::VideoOutManager *)videoMgr {
    _videoMgr = videoMgr;
}

- (BOOL)hasActiveGuestTexture {
    return _guestScreenTexture != nil;
}

- (uint64_t)presentedFrameCount {
    return _presentedFrameCount;
}

- (BOOL)updateTextureFromVideoManager {
    if (!_videoMgr || !_videoMgr->has_new_frame()) {
        return NO;
    }

    if (!_videoMgr->read_presentation_frame(_cachedPresFrame)) {
        _videoMgr->consume_new_frame();
        if ([self.gameViewDelegate respondsToSelector:@selector(metalGameView:didFailWithError:)]) {
            const char* diag = _cachedPresFrame.diagnostic.empty() ? _videoMgr->last_conversion_diagnostic().c_str() : _cachedPresFrame.diagnostic.c_str();
            NSString *msg = [NSString stringWithFormat:@"Rejeicao na conversao de video: %s", (diag && diag[0]) ? diag : "dados invalidos"];
            [self.gameViewDelegate metalGameView:self didFailWithError:msg];
        }
        return NO;
    }

    if (!_cachedPresFrame.valid || _cachedPresFrame.pixel_data.empty()) {
        _videoMgr->consume_new_frame();
        return NO;
    }

    const NSUInteger width = _cachedPresFrame.width;
    const NSUInteger height = _cachedPresFrame.height;
    if (width == 0 || height == 0) {
        _videoMgr->consume_new_frame();
        return NO;
    }

    if (!_guestScreenTexture || _guestScreenTexture.width != width || _guestScreenTexture.height != height) {
        MTLTextureDescriptor *texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
        texDesc.usage = MTLTextureUsageShaderRead;
        _guestScreenTexture = [self.device newTextureWithDescriptor:texDesc];
        if (!_guestScreenTexture) {
            _videoMgr->consume_new_frame();
            if ([self.gameViewDelegate respondsToSelector:@selector(metalGameView:didFailWithError:)]) {
                [self.gameViewDelegate metalGameView:self didFailWithError:@"Falha ao alocar MTLTexture para quadro guest."];
            }
            return NO;
        }
    }

    NSUInteger bytesPerRow = _cachedPresFrame.pitch * _cachedPresFrame.bytes_per_pixel;
    if (bytesPerRow == 0) {
        bytesPerRow = width * 4;
    }

    [_guestScreenTexture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                           mipmapLevel:0
                             withBytes:_cachedPresFrame.pixel_data.data()
                           bytesPerRow:bytesPerRow];

    _presentedFrameCount++;
    uint64_t seq = _cachedPresFrame.flip_sequence;
    _videoMgr->consume_new_frame();

    if ([self.gameViewDelegate respondsToSelector:@selector(metalGameView:didPresentFrame:)]) {
        [self.gameViewDelegate metalGameView:self didPresentFrame:seq];
    }

    return YES;
}

- (void)drawGuestFrameWithEncoder:(id<MTLRenderCommandEncoder>)encoder {
    if (!encoder || !_guestScreenTexture || !_screenPipeline) {
        return;
    }

    [encoder setRenderPipelineState:_screenPipeline];
    [encoder setFragmentTexture:_guestScreenTexture atIndex:0];
    [encoder setFragmentSamplerState:_screenSampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

@end
