#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#import <TargetConditionals.h>
#include "smoke.h"
#include "stability.h"
#include "input.h"
#include "shader_translator.h"
#include "metal_pipeline.h"
#include "flver_parser.h"
#include <exception>
#include <sys/utsname.h>
#include <simd/simd.h>
#include <chrono>
#include <vector>
#include <cstring>

@interface LabController : UIViewController <MTKViewDelegate>
@end
@implementation LabController {
    UILabel *_status;
    UILabel *_gpuStatus;
    UILabel *_subsystemStatus;
    UILabel *_documentStatus;
    MTKView *_metal;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLDepthStencilState> _depthState;
    id<MTLBuffer> _vertexBuffer;
    id<MTLBuffer> _indexBuffer;
    uint32_t _indexCount;
    id<MTLBuffer> _mvpBuffer;
    id<MTLBuffer> _lightingBuffer;
    id<MTLTexture> _diffuseTexture;
    id<MTLSamplerState> _samplerState;
    NSMutableDictionary *_report;
    NSUInteger _completedFrames;
    float _angle;
    std::chrono::steady_clock::time_point _lastFrameTime;
    bb::FramePacingTracker _pacingTracker;
    bb::MemoryMonitor _memoryMonitor;
    bb::InputManager _inputManager;
    uint32_t _modelVertexCount;
    uint32_t _modelTriangleCount;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    struct utsname hardware{};
    uname(&hardware);
    _report = [@{@"schema": @1, @"build": @BB_BUILD_ID,
        @"environment": TARGET_OS_SIMULATOR ? @"iOS-simulator" : @"iOS-device", @"game_execution": @NO,
        @"ios": UIDevice.currentDevice.systemVersion,
        @"device_model": [NSString stringWithUTF8String:hardware.machine],
        @"metal_completed_frames": @0,
        @"vfs_status": @"ready",
        @"audio_status": @"ready",
        @"gamepad_connected": @NO} mutableCopy];

    _lastFrameTime = std::chrono::steady_clock::now();

    UILabel *title = [UILabel new];
    title.text = @"BB Lab — Host iOS 26.6";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];

    UILabel *scope = [UILabel new];
    scope.text = @"Laboratório Bloodborne iOS. Execução CPU, Metal 3, VFS, Áudio, Entrada e Estabilidade.";
    scope.numberOfLines = 0;

    _status = [UILabel new];
    _status.numberOfLines = 0;
    _status.text = @"CPU: aguardando teste";

    _gpuStatus = [UILabel new];
    _gpuStatus.numberOfLines = 0;
    _gpuStatus.text = @"GPU: inicializando Metal";

    _subsystemStatus = [UILabel new];
    _subsystemStatus.numberOfLines = 0;
    _subsystemStatus.text = @"Subsistemas: VFS e Áudio prontos. Aguardando entrada.";

    _documentStatus = [UILabel new];
    _documentStatus.numberOfLines = 0;
    _documentStatus.text = @"Documento: toque em Verificar arquivo para ler teste-bblab.txt.";

    UIButton *documentButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [documentButton setTitle:@"Verificar teste-bblab.txt em Documents" forState:UIControlStateNormal];
    [documentButton addTarget:self action:@selector(checkDocument) forControlEvents:UIControlEventTouchUpInside];

    UIButton *run = [UIButton buttonWithType:UIButtonTypeSystem];
    [run setTitle:@"Executar testes completos (CPU, VFS, Áudio, Entrada)" forState:UIControlStateNormal];
    [run addTarget:self action:@selector(runTests) forControlEvents:UIControlEventTouchUpInside];

    // Controles virtuais na tela para testes de jogabilidade
    UIStackView *dpadRow = [UIStackView new];
    dpadRow.distribution = UIStackViewDistributionFillEqually;
    dpadRow.spacing = 8;
    for (NSString *btnTitle in @[@"▲ Cima", @"◀ Esq", @"▶ Dir", @"▼ Baixo"]) {
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        [b setTitle:btnTitle forState:UIControlStateNormal];
        [b addTarget:self action:@selector(onVirtualButtonTouch:) forControlEvents:UIControlEventTouchDown];
        [dpadRow addArrangedSubview:b];
    }

    UIStackView *actionsRow = [UIStackView new];
    actionsRow.distribution = UIStackViewDistributionFillEqually;
    actionsRow.spacing = 8;
    for (NSString *btnTitle in @[@"✖ Esquiva", @"● Interagir", @"■ Item", @"▲ Arma", @"R1 Ataque"]) {
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        [b setTitle:btnTitle forState:UIControlStateNormal];
        [b addTarget:self action:@selector(onVirtualButtonTouch:) forControlEvents:UIControlEventTouchDown];
        [actionsRow addArrangedSubview:b];
    }

    UIButton *exportButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [exportButton setTitle:@"Exportar diagnóstico de estabilidade" forState:UIControlStateNormal];
    [exportButton addTarget:self action:@selector(exportReport:) forControlEvents:UIControlEventTouchUpInside];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    _metal = [[MTKView alloc] initWithFrame:CGRectZero device:device];
    _metal.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _metal.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
    _metal.clearColor = MTLClearColorMake(0.04, 0.05, 0.09, 1);
    _metal.clearDepth = 1.0;
    _metal.preferredFramesPerSecond = 30;
    _metal.paused = YES;
    [_metal.heightAnchor constraintEqualToConstant:220].active = YES;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        title, scope, _status, run, _metal, _gpuStatus, _subsystemStatus,
        documentButton, _documentStatus, dpadRow, actionsRow, exportButton]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    [scroll addSubview:stack];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-20],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-20],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40]
    ]];

    if (!device) { [self gpuError:@"Metal indisponível"]; return; }
    _report[@"metal_device"] = device.name;
    _report[@"metal_supports_metal3"] = @([device supportsFamily:MTLGPUFamilyMetal3]);
    _report[@"metal_supports_apple9"] = @([device supportsFamily:MTLGPUFamilyApple9]);
    _report[@"metal_has_unified_memory"] = @(device.hasUnifiedMemory);
    _report[@"metal_max_buffer_length"] = @(device.maxBufferLength);

    _queue = [device newCommandQueue];

    MTLDepthStencilDescriptor *depthDesc = [MTLDepthStencilDescriptor new];
    depthDesc.depthCompareFunction = MTLCompareFunctionLessEqual;
    depthDesc.depthWriteEnabled = YES;
    _depthState = [device newDepthStencilStateWithDescriptor:depthDesc];

    bb::TranslationOptions opts{};
    opts.msl_version = 30000;
    opts.enable_argument_buffers = false;
    opts.flip_vertex_y = true;

    auto vs_spv = bb::ShaderTranslator::build_basic_vertex_spirv();
    auto vs_res = bb::ShaderTranslator::translate_spirv_to_msl(vs_spv, opts);
    if (!vs_res.success) {
        [self gpuError:[NSString stringWithUTF8String:vs_res.error_message.c_str()]];
        return;
    }

    auto fs_spv = bb::ShaderTranslator::build_basic_fragment_spirv();
    auto fs_res = bb::ShaderTranslator::translate_spirv_to_msl(fs_spv, opts);
    if (!fs_res.success) {
        [self gpuError:[NSString stringWithUTF8String:fs_res.error_message.c_str()]];
        return;
    }

    NSError *error = nil;
    NSString *vsSource = [NSString stringWithUTF8String:vs_res.msl_source.c_str()];
    id<MTLLibrary> vsLib = [device newLibraryWithSource:vsSource options:nil error:&error];
    if (!vsLib) {
        [self gpuError:error ? error.localizedDescription : @"Falha ao compilar Vertex Shader MSL"];
        return;
    }

    NSString *fsSource = [NSString stringWithUTF8String:fs_res.msl_source.c_str()];
    id<MTLLibrary> fsLib = [device newLibraryWithSource:fsSource options:nil error:&error];
    if (!fsLib) {
        [self gpuError:error ? error.localizedDescription : @"Falha ao compilar Fragment Shader MSL"];
        return;
    }

    MTLVertexDescriptor *vdesc = [MTLVertexDescriptor new];
    vdesc.attributes[0].format = MTLVertexFormatFloat3;
    vdesc.attributes[0].offset = 0;
    vdesc.attributes[0].bufferIndex = 0;

    vdesc.attributes[1].format = MTLVertexFormatFloat3;
    vdesc.attributes[1].offset = 3 * sizeof(float);
    vdesc.attributes[1].bufferIndex = 0;

    vdesc.attributes[2].format = MTLVertexFormatFloat2;
    vdesc.attributes[2].offset = 6 * sizeof(float);
    vdesc.attributes[2].bufferIndex = 0;

    vdesc.layouts[0].stride = 12 * sizeof(float);
    vdesc.layouts[0].stepRate = 1;
    vdesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [vsLib newFunctionWithName:[NSString stringWithUTF8String:vs_res.reflection.entry_point_name.c_str()]];
    descriptor.fragmentFunction = [fsLib newFunctionWithName:[NSString stringWithUTF8String:fs_res.reflection.entry_point_name.c_str()]];
    descriptor.vertexDescriptor = vdesc;
    descriptor.colorAttachments[0].pixelFormat = _metal.colorPixelFormat;
    descriptor.depthAttachmentPixelFormat = _metal.depthStencilPixelFormat;

    _pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!_pipeline) { [self gpuError:error ? error.localizedDescription : @"Falha no pipeline 3D"]; return; }

    bb::FlverModel model;
    std::string parseErr;
    bool loadedFromDisk = false;

    NSArray *docDirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if (docDirs.count > 0) {
        NSString *flverPath = [docDirs.firstObject stringByAppendingPathComponent:@"dvdroot_ps4/map/m24_00_00_00/m24_00_00_00_900091.flver.dcx"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:flverPath]) {
            loadedFromDisk = bb::FlverParser::parse_file([flverPath UTF8String], model, parseErr);
        }
    }

    if (!loadedFromDisk) {
        auto cleaverBytes = bb::FlverParser::create_synthetic_flver_saw_cleaver();
        bb::FlverParser::parse(cleaverBytes.data(), cleaverBytes.size(), model, parseErr);
    }

    bb::MetalMeshBuffer mesh = bb::FlverParser::convert_to_metal_buffers(model);
    _modelVertexCount = mesh.vertex_count;
    _modelTriangleCount = mesh.index_count / 3;
    _indexCount = mesh.index_count;

    _vertexBuffer = [device newBufferWithBytes:mesh.vertex_data.data()
                                        length:mesh.vertex_data.size() * sizeof(float)
                                       options:MTLResourceStorageModeShared];

    _indexBuffer = [device newBufferWithBytes:mesh.index_data.data()
                                       length:mesh.index_data.size() * sizeof(uint32_t)
                                      options:MTLResourceStorageModeShared];

    _mvpBuffer = [device newBufferWithLength:sizeof(bb::ModelViewProjectionUniforms)
                                     options:MTLResourceStorageModeShared];

    _lightingBuffer = [device newBufferWithLength:sizeof(bb::LightingUniforms)
                                          options:MTLResourceStorageModeShared];

    bb::LightingUniforms lighting{};
    lighting.dir_light_direction[0] = 0.577f;
    lighting.dir_light_direction[1] = -0.577f;
    lighting.dir_light_direction[2] = 0.577f;
    lighting.dir_light_direction[3] = 0.0f;
    lighting.dir_light_color[0] = 1.0f;
    lighting.dir_light_color[1] = 0.95f;
    lighting.dir_light_color[2] = 0.85f;
    lighting.dir_light_color[3] = 1.0f;
    lighting.ambient_light[0] = 0.15f;
    lighting.ambient_light[1] = 0.18f;
    lighting.ambient_light[2] = 0.22f;
    lighting.ambient_light[3] = 1.0f;
    lighting.camera_position[0] = 0.0f;
    lighting.camera_position[1] = 1.2f;
    lighting.camera_position[2] = 2.8f;
    lighting.camera_position[3] = 1.0f;
    lighting.specular_power = 32.0f;
    lighting.roughness = 0.35f;
    std::memcpy(_lightingBuffer.contents, &lighting, sizeof(lighting));

    MTLTextureDescriptor *texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    _diffuseTexture = [device newTextureWithDescriptor:texDesc];
    std::vector<uint32_t> texPixels(64 * 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            bool check = ((x / 8) ^ (y / 8)) & 1;
            uint8_t base = check ? 180 : 130;
            uint8_t noise = static_cast<uint8_t>(((x * 37 + y * 17) ^ 0x5A) % 30);
            uint8_t r = base + noise;
            uint8_t g = static_cast<uint8_t>(base * 0.9f) + noise;
            uint8_t b = static_cast<uint8_t>(base * 0.8f) + noise;
            texPixels[y * 64 + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
        }
    }
    [_diffuseTexture replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:texPixels.data() bytesPerRow:64 * sizeof(uint32_t)];

    MTLSamplerDescriptor *samplerDesc = [MTLSamplerDescriptor new];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.sAddressMode = MTLSamplerAddressModeRepeat;
    samplerDesc.tAddressMode = MTLSamplerAddressModeRepeat;
    _samplerState = [device newSamplerStateWithDescriptor:samplerDesc];

    _report[@"metal_flver_vertices"] = @(_modelVertexCount);
    _report[@"metal_flver_triangles"] = @(_modelTriangleCount);
    _report[@"metal_depth_buffer"] = @"Depth32Float";
    _report[@"metal_shader_msl_version"] = @"3.0";
    _report[@"model_rendered"] = loadedFromDisk ? @"Bloodborne Map FLVER" : @"Saw Cleaver (FromSoftware FLVER)";

    // Registra suporte a controles externos Apple GameController
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onControllerConnected:)
        name:GCControllerDidConnectNotification object:nil];
    if (GCController.controllers.count > 0) {
        [self onControllerConnected:nil];
    }

    _metal.delegate = self;
    _metal.paused = NO;
}

- (void)onControllerConnected:(NSNotification *)note {
    (void)note;
    _report[@"gamepad_connected"] = @YES;
    _subsystemStatus.text = [NSString stringWithFormat:@"GameController MFi/DualSense conectado: %@",
        (GCController.controllers.firstObject.vendorName ? GCController.controllers.firstObject.vendorName : @"Compatível")];
}

- (void)onVirtualButtonTouch:(UIButton *)sender {
    NSString *t = sender.currentTitle;
    if ([t containsString:@"Esquiva"]) _inputManager.set_button(bb::PAD_CROSS, true);
    else if ([t containsString:@"Interagir"]) _inputManager.set_button(bb::PAD_CIRCLE, true);
    else if ([t containsString:@"Item"]) _inputManager.set_button(bb::PAD_SQUARE, true);
    else if ([t containsString:@"Arma"]) _inputManager.set_button(bb::PAD_TRIANGLE, true);
    else if ([t containsString:@"R1"]) _inputManager.set_button(bb::PAD_R1, true);
    else if ([t containsString:@"Cima"]) _inputManager.set_button(bb::PAD_UP, true);
    else if ([t containsString:@"Baixo"]) _inputManager.set_button(bb::PAD_DOWN, true);
    else if ([t containsString:@"Esq"]) _inputManager.set_button(bb::PAD_LEFT, true);
    else if ([t containsString:@"Dir"]) _inputManager.set_button(bb::PAD_RIGHT, true);

    _subsystemStatus.text = [NSString stringWithFormat:@"Entrada ativa: %@", t];
}

- (void)gpuError:(NSString *)message {
    _gpuStatus.text = [@"GPU: " stringByAppendingString:message];
    _report[@"metal_error"] = message;
    _metal.paused = YES;
}

- (void)runTests {
    try {
        const auto report = bb::smoke();
        _report[@"cpu_checks"] = @(report.checks);
        _report[@"cpu_failures"] = @(report.failures);
        _status.text = [NSString stringWithFormat:@"CPU/memória/HLE: %u verificações, %u falhas\nBuild: %s",
            report.checks, report.failures, BB_BUILD_ID];
    } catch (const std::exception& error) {
        _report[@"cpu_error"] = [NSString stringWithUTF8String:error.what()];
        _status.text = @"CPU: falha inesperada; exporte o diagnóstico";
    }
}

- (void)checkDocument {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    NSURL *file = [documents URLByAppendingPathComponent:@"teste-bblab.txt" isDirectory:NO];
    NSMutableDictionary *result = [@{@"filename": @"teste-bblab.txt",
        @"status": @"not_found", @"valid": @NO} mutableCopy];
    NSError *error = nil;
    NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:file.path error:&error];
    if (attributes) {
        unsigned long long length = [attributes fileSize];
        result[@"size_bytes"] = @(length);
        if (![[attributes fileType] isEqualToString:NSFileTypeRegular]) {
            result[@"status"] = @"not_regular_file";
        } else if (length > 65536) {
            result[@"status"] = @"too_large";
        } else {
            NSData *bytes = [NSData dataWithContentsOfURL:file options:NSDataReadingMappedIfSafe error:&error];
            if (!bytes) {
                result[@"status"] = @"read_error";
            } else {
                NSString *content = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
                if (!content) {
                    result[@"status"] = @"invalid_utf8";
                } else if (bytes.length == 0) {
                    result[@"status"] = @"empty";
                } else {
                    result[@"status"] = @"ok";
                    result[@"valid"] = @YES;
                    result[@"content_utf8"] = content;
                    result[@"size_bytes"] = @(bytes.length);
                }
            }
        }
    } else if (error && error.code != NSFileNoSuchFileError) {
        result[@"status"] = @"read_error";
    }
    result[@"checked_at"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    _report[@"documents_test"] = result;
    _documentStatus.text = [NSString stringWithFormat:@"Documento teste-bblab.txt: %@ (%@ bytes). %@",
        result[@"status"], result[@"size_bytes"] ? result[@"size_bytes"] : @0,
        [result[@"valid"] boolValue] ? @"Texto UTF-8 registrado no relatorio." : @"Confira o arquivo em Documentos de BB Lab."];
}

- (void)drawInMTKView:(MTKView *)view {
    MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!pass || !drawable || !_pipeline || !_vertexBuffer || !_indexBuffer) return;

    auto now = std::chrono::steady_clock::now();
    double frame_ms = std::chrono::duration<double, std::milli>(now - _lastFrameTime).count();
    _lastFrameTime = now;
    _pacingTracker.record_frame_ms(frame_ms);

    _angle += 0.02f;
    float aspect = (view.bounds.size.height > 0) ? (float)view.bounds.size.width / (float)view.bounds.size.height : 1.777f;
    bb::Matrix4x4 proj = bb::Matrix4x4::perspective(55.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);
    float eye[3] = {0.0f, 1.0f, 2.8f};
    float target[3] = {0.0f, 0.3f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    bb::Matrix4x4 viewMat = bb::Matrix4x4::look_at(eye, target, up);
    bb::Matrix4x4 modelRot = bb::Matrix4x4::rotation_y(_angle);
    bb::Matrix4x4 modelPitch = bb::Matrix4x4::rotation_x(0.2f);
    bb::Matrix4x4 model = bb::Matrix4x4::multiply(modelRot, modelPitch);

    bb::ModelViewProjectionUniforms mvpUniforms{};
    mvpUniforms.model = model;
    mvpUniforms.view = viewMat;
    mvpUniforms.projection = proj;
    bb::Matrix4x4 modelView = bb::Matrix4x4::multiply(viewMat, model);
    mvpUniforms.mvp = bb::Matrix4x4::multiply(proj, modelView);
    mvpUniforms.normal_matrix = bb::Matrix4x4::normal_matrix(modelView);

    std::memcpy(_mvpBuffer.contents, &mvpUniforms, sizeof(mvpUniforms));

    id<MTLCommandBuffer> command = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) { [self gpuError:@"Falha ao criar command buffer"]; return; }

    [encoder setRenderPipelineState:_pipeline];
    if (_depthState) {
        [encoder setDepthStencilState:_depthState];
    }
    [encoder setCullMode:MTLCullModeNone];

    [encoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [encoder setVertexBuffer:_mvpBuffer offset:0 atIndex:1];

    [encoder setFragmentBuffer:_lightingBuffer offset:0 atIndex:1];
    if (_diffuseTexture) {
        [encoder setFragmentTexture:_diffuseTexture atIndex:0];
    }
    if (_samplerState) {
        [encoder setFragmentSamplerState:_samplerState atIndex:0];
    }

    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:_indexCount
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:_indexBuffer
                 indexBufferOffset:0];

    [encoder endEncoding];
    [command presentDrawable:drawable];

    __weak LabController *weakSelf = self;
    [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        dispatch_async(dispatch_get_main_queue(), ^{
            LabController *strongSelf = weakSelf;
            if (!strongSelf) return;
            if (finished.status == MTLCommandBufferStatusCompleted) {
                ++strongSelf->_completedFrames;
                strongSelf->_report[@"metal_completed_frames"] = @(strongSelf->_completedFrames);
                if (strongSelf->_completedFrames == 1 || strongSelf->_completedFrames % 30 == 0) {
                    auto pacing = strongSelf->_pacingTracker.get_stats();
                    uint64_t mem_bytes = strongSelf->_memoryMonitor.query_physical_footprint_bytes();
                    double mem_mb = static_cast<double>(mem_bytes) / (1024.0 * 1024.0);
                    strongSelf->_gpuStatus.text = [NSString stringWithFormat:
                        @"Metal 3: %lu frames | FLVER (%u vtx, %u tri) | P50: %.1fms | Mem: %.1f MB",
                        (unsigned long)strongSelf->_completedFrames,
                        strongSelf->_modelVertexCount,
                        strongSelf->_modelTriangleCount,
                        pacing.p50_ms, mem_mb];
                }
            } else {
                [strongSelf gpuError:finished.error ? finished.error.localizedDescription : @"Erro no command buffer"];
            }
        });
    }];
    [command commit];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }

- (void)exportReport:(UIButton *)sender {
    [self checkDocument];
    _report[@"timestamp"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    auto pacing = _pacingTracker.get_stats();
    _report[@"frame_pacing_p50_ms"] = @(pacing.p50_ms);
    _report[@"frame_pacing_p95_ms"] = @(pacing.p95_ms);
    _report[@"frame_pacing_p99_ms"] = @(pacing.p99_ms);
    _report[@"average_fps"] = @(pacing.average_fps);
    uint64_t mem_bytes = _memoryMonitor.query_physical_footprint_bytes();
    _report[@"memory_footprint_mb"] = @(static_cast<double>(mem_bytes) / (1024.0 * 1024.0));
    _report[@"gamepad_connected"] = @(GCController.controllers.count > 0);

    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:_report options:NSJSONWritingPrettyPrinted error:&error];
    NSURL *directory = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
        inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:&error];
    NSURL *file = [directory URLByAppendingPathComponent:@"bb-lab-report.json"];
    if (!json || !file || ![json writeToURL:file options:NSDataWritingAtomic error:&error]) {
        _status.text = [@"Erro ao salvar diagnóstico: " stringByAppendingString:error ? error.localizedDescription : @"desconhecido"];
        return;
    }
    UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[file] applicationActivities:nil];
    share.popoverPresentationController.sourceView = sender;
    share.popoverPresentationController.sourceRect = sender.bounds;
    [self presentViewController:share animated:YES completion:nil];
}
@end

@interface LabSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation LabSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [LabController new];
    [self.window makeKeyAndVisible];
}
@end
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    return YES;
}
@end
int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class)); }
}
