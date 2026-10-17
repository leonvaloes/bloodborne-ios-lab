#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <TargetConditionals.h>
#include "smoke.h"
#include "stability.h"
#include "input.h"
#include "shader_translator.h"
#include "metal_pipeline.h"
#include "flver_parser.h"
#include "game_asset_probe.h"
#include "boot_probe.h"
#include "game_boot_coordinator.h"
#include "vfs.h"
#include "video_out.h"
#include "touch_controls.h"
#include "pacing_tracker.h"
#include "dialog_hle.h"
#include "gnm_metal_bridge.h"
#include <exception>
#include <sys/utsname.h>
#include <simd/simd.h>
#include <chrono>
#include <algorithm>
#include <vector>
#include <cstring>

@class LabController;

// View pass-through que permite que toques fora dos botões alcancem o MTKView subjacente
@interface PassThroughView : UIView
@end

@implementation PassThroughView
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    UIView *hitView = [super hitTest:point withEvent:event];
    return hitView == self ? nil : hitView;
}
@end

@interface LabMTKView : MTKView
@property (nonatomic, weak) LabController *controller;
@end

@interface LabController : UIViewController <MTKViewDelegate>
- (void)handleTouchId:(uint64_t)tid x:(float)x y:(float)y phase:(bb::TouchPhase)phase;
- (void)fillAudioBuffer:(AudioQueueBufferRef)buffer;
- (BOOL)handleGnmDrawCalls:(const std::vector<bb::DrawCall>&)draws;
- (BOOL)handleGnmDispatchCalls:(const std::vector<bb::DispatchCall>&)dispatches;
@end

@implementation LabMTKView
- (void)handleTouch:(UITouch *)touch phase:(NSString *)phase {
    if (!self.controller) return;
    CGPoint loc = [touch locationInView:self];
    CGSize size = self.bounds.size;
    if (size.width <= 0 || size.height <= 0) return;

    float px = (float)loc.x;
    float py = (float)loc.y;
    uint64_t tid = (uint64_t)(uintptr_t)touch;

    bb::TouchPhase tp = bb::TouchPhase::Began;
    if ([phase isEqualToString:@"moved"]) tp = bb::TouchPhase::Moved;
    else if ([phase isEqualToString:@"ended"]) tp = bb::TouchPhase::Ended;
    else if ([phase isEqualToString:@"cancelled"]) tp = bb::TouchPhase::Cancelled;

    [self.controller handleTouchId:tid x:px y:py phase:tp];
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) [self handleTouch:t phase:@"began"];
    [super touchesBegan:touches withEvent:event];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) [self handleTouch:t phase:@"moved"];
    [super touchesMoved:touches withEvent:event];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) [self handleTouch:t phase:@"ended"];
    [super touchesEnded:touches withEvent:event];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) [self handleTouch:t phase:@"cancelled"];
    [super touchesCancelled:touches withEvent:event];
}
@end

// Callback de CoreAudio AudioQueue para streaming contínuo em 48 kHz estéreo
static void BB_AudioQueueCallback(void *userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    LabController *controller = (__bridge LabController *)userData;
    if (controller) {
        [controller fillAudioBuffer:buffer];
        AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    }
}

// Motivos de pausa distintos para governança de ciclo de vida (T112 / FR-010)
enum PauseReason : uint32_t {
    PauseReasonNone       = static_cast<uint32_t>(bb::PauseReason::none),
    PauseReasonUser       = static_cast<uint32_t>(bb::PauseReason::user),
    PauseReasonLifecycle  = static_cast<uint32_t>(bb::PauseReason::lifecycle),
    PauseReasonDisconnect = static_cast<uint32_t>(bb::PauseReason::disconnect),
};

@implementation LabController {
    // Componentes de interface do jogo
    LabMTKView *_metal;
    PassThroughView *_controlsOverlay;
    UIView *_welcomeCardView;
    UIView *_drawerOverlay;
    UILabel *_topStatusLabel;
    UIButton *_menuButton;
    BOOL _virtualControlsVisible;

    // Labels de diagnóstico detalhado dentro do menu drawer
    UILabel *_status;
    UILabel *_gpuStatus;
    UILabel *_subsystemStatus;
    UILabel *_documentStatus;
    UILabel *_assetStatus;
    UILabel *_dcxStatus;
    UILabel *_bootStatus;
    UILabel *_gameBootStatus;
    UIButton *_gameBootButton;
    UIButton *_continueSaveButton;
    UIButton *_continuousButton;

    // Estado de execução do motor
    BOOL _continuousExecution;
    uint32_t _pauseReasons;
    __weak GCController *_activeController;
    std::unique_ptr<bb::GameBootCoordinator> _bootCoordinator;
    bb::VideoOutManager _videoOutManager;
    struct GuestFlipSink : bb::VideoOutSink {
        void on_flip(const bb::FlipEvent&) override {}
    } _guestFlipSink;
    class HostGnmSubmissionBridge : public bb::GnmSubmissionListener {
    public:
        explicit HostGnmSubmissionBridge(LabController *ctrl) : _ctrl(ctrl) {}
        bool on_draw_calls(const std::vector<bb::DrawCall>& draws) override {
            if (draws.empty()) return true;
            LabController *s = _ctrl;
            return s ? [s handleGnmDrawCalls:draws] : false;
        }
        bool on_dispatch_calls(const std::vector<bb::DispatchCall>& dispatches) override {
            if (dispatches.empty()) return true;
            LabController *s = _ctrl;
            return s ? [s handleGnmDispatchCalls:dispatches] : false;
        }
    private:
        __weak LabController *_ctrl;
    };
    std::unique_ptr<HostGnmSubmissionBridge> _gnmBridgeListener;
    bb::InputManager _inputManager;
    bb::TouchControlsOverlay _touchOverlay;
    dispatch_queue_t _guestExecutionQueue;
    std::atomic<bool> _guestStepping;
    std::atomic<bool> _guestBooting;
    std::uint64_t _lastGuestSteps;
    std::atomic<bool> _guestCanStep;
    std::atomic<std::uint64_t> _guestSubmits;
    std::atomic<std::uint32_t> _guestTriangles;
    std::atomic<std::uint64_t> _guestRasterizerFailures;
    std::atomic<int> _guestLastRasterizerStatus;
    std::atomic<std::uint32_t> _guestCpuSliceBudget;
    std::atomic<std::uint32_t> _guestCpuRescheduleDelayMs;
    NSString *_selectedSaveSlot;

    // Subsistema Metal 3
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLRenderPipelineState> _screenPipeline;
    id<MTLSamplerState> _screenSampler;
    id<MTLDepthStencilState> _depthState;
    id<MTLBuffer> _vertexBuffer;
    id<MTLBuffer> _indexBuffer;
    uint32_t _indexCount;
    id<MTLBuffer> _lightingBuffer;
    id<MTLTexture> _diffuseTexture;
    id<MTLTexture> _guestScreenTexture;
    id<MTLSamplerState> _samplerState;

    // Métricas de desempenho
    NSUInteger _completedFrames;
    float _angle;
    float _cameraPitch;
    std::chrono::steady_clock::time_point _lastFrameTime;
    bb::PacingTracker _pacingTracker{120, 30.0};
    bb::MemoryMonitor _memoryMonitor;
    uint32_t _modelVertexCount;
    uint32_t _modelTriangleCount;
    NSMutableDictionary *_report;

    // Subsistema CoreAudio
    AudioQueueRef _audioQueue;
    AudioQueueBufferRef _audioBuffers[3];
    BOOL _audioActive;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
    return UIInterfaceOrientationLandscapeRight;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    struct utsname hardware{};
    uname(&hardware);
    _report = [@{
        @"schema": @1,
        @"build": @BB_BUILD_ID,
        @"environment": TARGET_OS_SIMULATOR ? @"iOS-simulator" : @"iOS-device",
        @"game_execution": @NO,
        @"ios": UIDevice.currentDevice.systemVersion,
        @"device_model": [NSString stringWithUTF8String:hardware.machine],
        @"metal_completed_frames": @0,
        @"vfs_status": @"ready",
        @"audio_status": @"ready",
        @"gamepad_connected": @NO
    } mutableCopy];

    _lastFrameTime = std::chrono::steady_clock::now();
    _guestStepping.store(false);
    _guestBooting.store(false);
    _lastGuestSteps = 0;
    _guestCanStep.store(false);
    _guestSubmits.store(0);
    _guestTriangles.store(0);
    _guestRasterizerFailures.store(0);
    _guestLastRasterizerStatus.store(static_cast<int>(bb::RasterizerStatus::Ok));
    _guestCpuSliceBudget.store(50000);
    _guestCpuRescheduleDelayMs.store(0);
    _guestExecutionQueue = dispatch_queue_create("org.bloodborne.lab.guest", DISPATCH_QUEUE_SERIAL);
    _touchOverlay.bind_input_manager(&_inputManager);
    _touchOverlay.set_left_stick_mode(bb::StickMode::Floating);
    _touchOverlay.set_right_stick_mode(bb::StickMode::Floating);
    _virtualControlsVisible = YES;
    _angle = 0.0f;
    _cameraPitch = 0.2f;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        [self showFatalError:@"Dispositivo Metal não disponível neste hardware."];
        return;
    }

    [self setupFullscreenMetalView:device];
    [self setupMetalPipeline:device];
    [self setupGamepadHUD];
    [self setupDrawerMenu];
    [self setupGameControllers];
    [self setupAudio];
    [self setupLifecycleAndThermalMonitoring];
    [self setupDialogIntegration];

    // Callback de descarte de emergência sob pressão de memória Jetsam (T043 / FR-009)
    __weak LabController *weakSelf = self;
    _memoryMonitor.register_trim_callback([weakSelf]() {
        dispatch_async(dispatch_get_main_queue(), ^{
            LabController *strongSelf = weakSelf;
            if (!strongSelf) return;
            if (strongSelf->_metal.paused || !strongSelf->_continuousExecution) {
                strongSelf->_guestScreenTexture = nil;
            }
            bb::gnm_bridge_clear_caches();
            bb::AudioOutManager::instance().purge_inactive_buffers();
            strongSelf->_report[@"memory_trimmed"] = @YES;
        });
    });

    // Adia o auto-boot até a próxima passagem do loop principal. A inicialização
    // síncrona dos módulos ainda pode bloquear a interface: limite conhecido.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self checkDocumentsInventory];
        [self attemptAutoBoot];
    });
}

- (void)showFatalError:(NSString *)msg {
    UILabel *err = [[UILabel alloc] initWithFrame:self.view.bounds];
    err.text = msg;
    err.textColor = [UIColor systemRedColor];
    err.textAlignment = NSTextAlignmentCenter;
    err.numberOfLines = 0;
    err.font = [UIFont boldSystemFontOfSize:18];
    [self.view addSubview:err];
}

- (void)setupDialogIntegration {
    __weak typeof(self) weakSelf = self;
    bb::default_dialog_manager().set_msg_dialog_callback([weakSelf](const bb::MsgDialogRequest& req) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf || !strongSelf.isViewLoaded || strongSelf.view.window == nil) {
                // Se a UI não estiver disponível ou app em background, responder headless
                // para não travar o guest thread indefinidamente
                bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::OK);
                return;
            }
            NSString *msg = [NSString stringWithUTF8String:req.message.c_str()];
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Bloodborne"
                                                                           message:msg
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            switch (req.button_type) {
                case bb::SceMsgDialogButtonType::YESNO: {
                    [alert addAction:[UIAlertAction actionWithTitle:@"Sim" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::YES);
                    }]];
                    [alert addAction:[UIAlertAction actionWithTitle:@"Não" style:UIAlertActionStyleCancel handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::NO);
                    }]];
                    break;
                }
                case bb::SceMsgDialogButtonType::OK_CANCEL: {
                    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::OK);
                    }]];
                    [alert addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::CANCEL);
                    }]];
                    break;
                }
                case bb::SceMsgDialogButtonType::RETRY_CANCEL: {
                    [alert addAction:[UIAlertAction actionWithTitle:@"Tentar Novamente" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::RETRY);
                    }]];
                    [alert addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::CANCEL);
                    }]];
                    break;
                }
                case bb::SceMsgDialogButtonType::OK:
                default: {
                    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
                        bb::default_dialog_manager().submit_msg_result(bb::SceMsgDialogButtonId::OK);
                    }]];
                    break;
                }
            }
            [strongSelf presentViewController:alert animated:YES completion:nil];
        });
    });

    bb::default_dialog_manager().set_ime_dialog_callback([weakSelf](const bb::ImeDialogRequest& req) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf || !strongSelf.isViewLoaded || strongSelf.view.window == nil) {
                // Responde com o texto inicial para evitar trava do guest thread
                bb::default_dialog_manager().submit_ime_result(req.initial_text, bb::SCE_IME_DIALOG_END_STATUS_OK);
                return;
            }
            NSString *title = [NSString stringWithUTF8String:req.title.c_str()];
            NSString *initialText = [NSString stringWithUTF8String:req.initial_text.c_str()];
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:title.length ? title : @"Entrada de Texto"
                                                                           message:@"Digite o texto solicitado pelo jogo:"
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
                textField.text = initialText;
            }];
            [alert addAction:[UIAlertAction actionWithTitle:@"Confirmar" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
                UITextField *field = alert.textFields.firstObject;
                std::string resText = field && field.text ? [field.text UTF8String] : req.initial_text;
                bb::default_dialog_manager().submit_ime_result(resText, bb::SCE_IME_DIALOG_END_STATUS_OK);
            }]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:^(UIAlertAction * _Nonnull action) {
                bb::default_dialog_manager().submit_ime_result("", bb::SCE_IME_DIALOG_END_STATUS_USER_CANCELED);
            }]];
            [strongSelf presentViewController:alert animated:YES completion:nil];
        });
    });
}

#pragma mark - Metal View & Pipeline

- (void)setupFullscreenMetalView:(id<MTLDevice>)device {
    _metal = [[LabMTKView alloc] initWithFrame:self.view.bounds device:device];
    _metal.controller = self;
    _metal.multipleTouchEnabled = YES;
    _metal.userInteractionEnabled = YES;
    _metal.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _metal.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
    _metal.clearColor = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
    _metal.clearDepth = 1.0;
    _metal.preferredFramesPerSecond = 30;
    _metal.paused = NO;
    _metal.enableSetNeedsDisplay = NO;
    _metal.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_metal];

    [NSLayoutConstraint activateConstraints:@[
        [_metal.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_metal.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [_metal.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_metal.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor]
    ]];
}

- (void)setupMetalPipeline:(id<MTLDevice>)device {
    _report[@"metal_device"] = device.name;
    _report[@"metal_supports_metal3"] = @([device supportsFamily:MTLGPUFamilyMetal3]);
    _report[@"metal_supports_apple9"] = @([device supportsFamily:MTLGPUFamilyApple9]);
    _report[@"metal_has_unified_memory"] = @(device.hasUnifiedMemory);
    _report[@"metal_max_buffer_length"] = @(device.maxBufferLength);

    _queue = [device newCommandQueue];

    // Passe fullscreen independente da malha de demonstração. Não interpreta
    // superfícies GNM: mostra somente pixels lineares efetivamente lidos do guest.
    NSString *screenShader = @"#include <metal_stdlib>\n"
        @"using namespace metal;\n"
        @"struct ScreenVertex { float4 position [[position]]; float2 uv; };\n"
        @"vertex ScreenVertex bbScreenVertex(uint id [[vertex_id]]) {\n"
        @"  float2 p = float2((id << 1) & 2, id & 2);\n"
        @"  return {float4(p * 2.0 - 1.0, 0.0, 1.0), float2(p.x, 1.0 - p.y)};\n"
        @"}\n"
        @"fragment float4 bbScreenFragment(ScreenVertex v [[stage_in]], texture2d<float> t [[texture(0)]],\n"
        @"                                  sampler s [[sampler(0)]]) { return t.sample(s, v.uv); }\n";
    NSError *screenError = nil;
    id<MTLLibrary> screenLibrary = [device newLibraryWithSource:screenShader options:nil error:&screenError];
    if (screenLibrary) {
        MTLRenderPipelineDescriptor *screenDesc = [MTLRenderPipelineDescriptor new];
        screenDesc.vertexFunction = [screenLibrary newFunctionWithName:@"bbScreenVertex"];
        screenDesc.fragmentFunction = [screenLibrary newFunctionWithName:@"bbScreenFragment"];
        screenDesc.colorAttachments[0].pixelFormat = _metal.colorPixelFormat;
        _screenPipeline = [device newRenderPipelineStateWithDescriptor:screenDesc error:&screenError];
    }
    if (!_screenPipeline) {
        _report[@"guest_presentation_error"] = screenError ? screenError.localizedDescription : @"Pipeline de vídeo indisponível";
    }
    MTLSamplerDescriptor *screenSamplerDesc = [MTLSamplerDescriptor new];
    screenSamplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    screenSamplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    screenSamplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    screenSamplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _screenSampler = [device newSamplerStateWithDescriptor:screenSamplerDesc];

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
    NSString *modelSource = @"synthetic_saw_cleaver";
    const size_t meshLimit = std::min<size_t>(32 * 1024 * 1024, device.maxBufferLength);
    bb::MetalMeshBuffer mesh;
    auto prepare = [&](const bb::FlverModel& source) {
        auto status = bb::prepare_metal_mesh(source, mesh, meshLimit);
        _report[@"mesh_preparation_status"] = @(bb::mesh_preparation_status_string(status));
        return status == bb::MeshPreparationStatus::Ready;
    };

    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    if (documents) {
        NSString *smallFlver = [documents.path stringByAppendingPathComponent:@"m29_00_00_00_408010.flver.dcx"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:smallFlver] &&
            bb::FlverParser::parse_file(smallFlver.UTF8String, model, parseErr) &&
            prepare(model)) {
            loadedFromDisk = true;
            modelSource = @"documents_small_flver_dcx";
        }
    }

    if (!loadedFromDisk && documents) {
        NSString *mapPath = [documents.path stringByAppendingPathComponent:@"dvdroot_ps4/map/m24_00_00_00/m24_00_00_00_900091.flver.dcx"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:mapPath] &&
            bb::FlverParser::parse_file(mapPath.UTF8String, model, parseErr) &&
            prepare(model)) {
            loadedFromDisk = true;
            modelSource = @"documents_map_flver_dcx";
        }
    }

    if (!loadedFromDisk) {
        model = bb::FlverModel{};
        auto cleaverBytes = bb::FlverParser::create_synthetic_flver_saw_cleaver();
        if (bb::FlverParser::parse(cleaverBytes.data(), cleaverBytes.size(), model, parseErr)) {
            prepare(model);
        }
    }

    _modelVertexCount = mesh.vertex_count;
    _modelTriangleCount = static_cast<uint32_t>(mesh.index_count / 3);
    _report[@"metal_flver_model_source"] = modelSource;
    _report[@"metal_flver_vertices"] = @(_modelVertexCount);
    _report[@"metal_flver_triangles"] = @(_modelTriangleCount);

    _vertexBuffer = [device newBufferWithBytes:mesh.vertex_data.data()
                                        length:mesh.vertex_data.size() * sizeof(float)
                                       options:MTLResourceStorageModeShared];
    _indexBuffer = [device newBufferWithBytes:mesh.index_data.data()
                                       length:mesh.index_data.size() * sizeof(uint32_t)
                                      options:MTLResourceStorageModeShared];
    _indexCount = mesh.index_count;

    bb::LightingUniforms lighting{};
    lighting.camera_position[0] = 0.0f;
    lighting.camera_position[1] = 0.0f;
    lighting.camera_position[2] = 2.5f;
    lighting.camera_position[3] = 1.0f;
    lighting.specular_power = 32.0f;
    _lightingBuffer = [device newBufferWithBytes:&lighting length:sizeof(lighting) options:MTLResourceStorageModeShared];

    MTLTextureDescriptor *texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:256
                                                                                      height:256
                                                                                   mipmapped:NO];
    _diffuseTexture = [device newTextureWithDescriptor:texDesc];
    std::vector<uint32_t> texPixels(256 * 256);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            uint8_t c = ((x ^ y) & 16) ? 140 : 85;
            texPixels[y * 256 + x] = (255u << 24) | (c << 16) | (c << 8) | c;
        }
    }
    [_diffuseTexture replaceRegion:MTLRegionMake2D(0, 0, 256, 256)
                       mipmapLevel:0
                         withBytes:texPixels.data()
                       bytesPerRow:256 * 4];

    MTLSamplerDescriptor *samplerDesc = [MTLSamplerDescriptor new];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.sAddressMode = MTLSamplerAddressModeRepeat;
    samplerDesc.tAddressMode = MTLSamplerAddressModeRepeat;
    _samplerState = [device newSamplerStateWithDescriptor:samplerDesc];

    _metal.delegate = self;
}

#pragma mark - On-Screen Virtual Gamepad HUD

- (UIButton *)createHudButtonWithTitle:(NSString *)title
                                 width:(CGFloat)w
                                height:(CGFloat)h
                                radius:(CGFloat)r
                              fontSize:(CGFloat)fs {
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
    [btn setTitle:title forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont systemFontOfSize:fs weight:UIFontWeightBold];
    [btn setTitleColor:[UIColor colorWithWhite:0.95 alpha:1.0] forState:UIControlStateNormal];
    btn.backgroundColor = [UIColor colorWithWhite:0.12 alpha:0.65];
    btn.layer.cornerRadius = r;
    btn.layer.borderWidth = 1.0;
    btn.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.25].CGColor;
    btn.clipsToBounds = YES;
    btn.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [btn.widthAnchor constraintEqualToConstant:w],
        [btn.heightAnchor constraintEqualToConstant:h]
    ]];

    [btn addTarget:self action:@selector(onVirtualButtonTouch:) forControlEvents:UIControlEventTouchDown];
    [btn addTarget:self action:@selector(onVirtualButtonRelease:)
        forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    return btn;
}

- (void)setupGamepadHUD {
    _controlsOverlay = [[PassThroughView alloc] initWithFrame:self.view.bounds];
    _controlsOverlay.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_controlsOverlay];

    [NSLayoutConstraint activateConstraints:@[
        [_controlsOverlay.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_controlsOverlay.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [_controlsOverlay.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_controlsOverlay.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor]
    ]];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;

    // Barra superior minimalista tipo "Floating Pill"
    UIView *topPill = [UIView new];
    topPill.translatesAutoresizingMaskIntoConstraints = NO;
    topPill.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.78];
    topPill.layer.cornerRadius = 16.0;
    topPill.layer.borderWidth = 0.5;
    topPill.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.2].CGColor;
    [_controlsOverlay addSubview:topPill];

    _topStatusLabel = [UILabel new];
    _topStatusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _topStatusLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightMedium];
    _topStatusLabel.textColor = [UIColor colorWithRed:0.95 green:0.85 blue:0.65 alpha:1.0];
    _topStatusLabel.text = @"BB Lab | demonstração | sem gameplay validado";
    [topPill addSubview:_topStatusLabel];

    _menuButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _menuButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_menuButton setTitle:@"⚙ Menu" forState:UIControlStateNormal];
    _menuButton.titleLabel.font = [UIFont boldSystemFontOfSize:12];
    [_menuButton setTitleColor:[UIColor systemYellowColor] forState:UIControlStateNormal];
    [_menuButton addTarget:self action:@selector(toggleDrawerMenu) forControlEvents:UIControlEventTouchUpInside];
    [topPill addSubview:_menuButton];

    [NSLayoutConstraint activateConstraints:@[
        [topPill.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [topPill.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [topPill.heightAnchor constraintEqualToConstant:32],

        [_topStatusLabel.leadingAnchor constraintEqualToAnchor:topPill.leadingAnchor constant:14],
        [_topStatusLabel.centerYAnchor constraintEqualToAnchor:topPill.centerYAnchor],

        [_menuButton.leadingAnchor constraintEqualToAnchor:_topStatusLabel.trailingAnchor constant:12],
        [_menuButton.trailingAnchor constraintEqualToAnchor:topPill.trailingAnchor constant:-14],
        [_menuButton.centerYAnchor constraintEqualToAnchor:topPill.centerYAnchor]
    ]];

    // Gatilhos de ombro superiores
    UIButton *btnL1 = [self createHudButtonWithTitle:@"L1" width:56 height:34 radius:8 fontSize:13];
    UIButton *btnL2 = [self createHudButtonWithTitle:@"L2" width:56 height:34 radius:8 fontSize:13];
    UIStackView *leftTriggers = [[UIStackView alloc] initWithArrangedSubviews:@[btnL2, btnL1]];
    leftTriggers.spacing = 8;
    leftTriggers.translatesAutoresizingMaskIntoConstraints = NO;
    [_controlsOverlay addSubview:leftTriggers];

    UIButton *btnR1 = [self createHudButtonWithTitle:@"R1" width:56 height:34 radius:8 fontSize:13];
    UIButton *btnR2 = [self createHudButtonWithTitle:@"R2" width:56 height:34 radius:8 fontSize:13];
    UIStackView *rightTriggers = [[UIStackView alloc] initWithArrangedSubviews:@[btnR1, btnR2]];
    rightTriggers.spacing = 8;
    rightTriggers.translatesAutoresizingMaskIntoConstraints = NO;
    [_controlsOverlay addSubview:rightTriggers];

    [NSLayoutConstraint activateConstraints:@[
        [leftTriggers.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [leftTriggers.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12],

        [rightTriggers.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [rightTriggers.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12]
    ]];

    // D-Pad Direcional no canto inferior esquerdo
    UIButton *btnUp = [self createHudButtonWithTitle:@"▲" width:46 height:46 radius:10 fontSize:16];
    UIButton *btnDown = [self createHudButtonWithTitle:@"▼" width:46 height:46 radius:10 fontSize:16];
    UIButton *btnLeft = [self createHudButtonWithTitle:@"◀" width:46 height:46 radius:10 fontSize:16];
    UIButton *btnRight = [self createHudButtonWithTitle:@"▶" width:46 height:46 radius:10 fontSize:16];

    PassThroughView *dpadContainer = [PassThroughView new];
    dpadContainer.translatesAutoresizingMaskIntoConstraints = NO;
    [dpadContainer addSubview:btnUp];
    [dpadContainer addSubview:btnDown];
    [dpadContainer addSubview:btnLeft];
    [dpadContainer addSubview:btnRight];
    [_controlsOverlay addSubview:dpadContainer];

    [NSLayoutConstraint activateConstraints:@[
        [dpadContainer.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [dpadContainer.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16],
        [dpadContainer.widthAnchor constraintEqualToConstant:140],
        [dpadContainer.heightAnchor constraintEqualToConstant:140],

        [btnUp.centerXAnchor constraintEqualToAnchor:dpadContainer.centerXAnchor],
        [btnUp.topAnchor constraintEqualToAnchor:dpadContainer.topAnchor],

        [btnDown.centerXAnchor constraintEqualToAnchor:dpadContainer.centerXAnchor],
        [btnDown.bottomAnchor constraintEqualToAnchor:dpadContainer.bottomAnchor],

        [btnLeft.centerYAnchor constraintEqualToAnchor:dpadContainer.centerYAnchor],
        [btnLeft.leadingAnchor constraintEqualToAnchor:dpadContainer.leadingAnchor],

        [btnRight.centerYAnchor constraintEqualToAnchor:dpadContainer.centerYAnchor],
        [btnRight.trailingAnchor constraintEqualToAnchor:dpadContainer.trailingAnchor]
    ]];

    // Cluster de botões PlayStation no canto inferior direito
    UIButton *btnTriangle = [self createHudButtonWithTitle:@"▲" width:48 height:48 radius:24 fontSize:18];
    btnTriangle.accessibilityIdentifier = @"triangle";
    [btnTriangle setTitleColor:[UIColor colorWithRed:0.3 green:0.9 blue:0.4 alpha:1.0] forState:UIControlStateNormal];

    UIButton *btnCross = [self createHudButtonWithTitle:@"✖" width:48 height:48 radius:24 fontSize:18];
    [btnCross setTitleColor:[UIColor colorWithRed:0.4 green:0.6 blue:1.0 alpha:1.0] forState:UIControlStateNormal];

    UIButton *btnSquare = [self createHudButtonWithTitle:@"■" width:48 height:48 radius:24 fontSize:18];
    [btnSquare setTitleColor:[UIColor colorWithRed:1.0 green:0.4 blue:0.7 alpha:1.0] forState:UIControlStateNormal];

    UIButton *btnCircle = [self createHudButtonWithTitle:@"●" width:48 height:48 radius:24 fontSize:18];
    [btnCircle setTitleColor:[UIColor colorWithRed:1.0 green:0.3 blue:0.3 alpha:1.0] forState:UIControlStateNormal];

    PassThroughView *actionsContainer = [PassThroughView new];
    actionsContainer.translatesAutoresizingMaskIntoConstraints = NO;
    [actionsContainer addSubview:btnTriangle];
    [actionsContainer addSubview:btnCross];
    [actionsContainer addSubview:btnSquare];
    [actionsContainer addSubview:btnCircle];
    [_controlsOverlay addSubview:actionsContainer];

    [NSLayoutConstraint activateConstraints:@[
        [actionsContainer.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [actionsContainer.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16],
        [actionsContainer.widthAnchor constraintEqualToConstant:148],
        [actionsContainer.heightAnchor constraintEqualToConstant:148],

        [btnTriangle.centerXAnchor constraintEqualToAnchor:actionsContainer.centerXAnchor],
        [btnTriangle.topAnchor constraintEqualToAnchor:actionsContainer.topAnchor],

        [btnCross.centerXAnchor constraintEqualToAnchor:actionsContainer.centerXAnchor],
        [btnCross.bottomAnchor constraintEqualToAnchor:actionsContainer.bottomAnchor],

        [btnSquare.centerYAnchor constraintEqualToAnchor:actionsContainer.centerYAnchor],
        [btnSquare.leadingAnchor constraintEqualToAnchor:actionsContainer.leadingAnchor],

        [btnCircle.centerYAnchor constraintEqualToAnchor:actionsContainer.centerYAnchor],
        [btnCircle.trailingAnchor constraintEqualToAnchor:actionsContainer.trailingAnchor]
    ]];

    // Botões utilitários centrais (L3, Touchpad, Options, R3)
    UIButton *btnL3 = [self createHudButtonWithTitle:@"L3" width:40 height:32 radius:8 fontSize:12];
    UIButton *btnTouchpad = [self createHudButtonWithTitle:@"TOUCH" width:56 height:32 radius:8 fontSize:12];
    UIButton *btnOpt = [self createHudButtonWithTitle:@"OPT" width:46 height:32 radius:8 fontSize:12];
    UIButton *btnR3 = [self createHudButtonWithTitle:@"R3" width:40 height:32 radius:8 fontSize:12];

    UIStackView *middleRow = [[UIStackView alloc] initWithArrangedSubviews:@[btnL3, btnTouchpad, btnOpt, btnR3]];
    middleRow.spacing = 10;
    middleRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_controlsOverlay addSubview:middleRow];

    [NSLayoutConstraint activateConstraints:@[
        [middleRow.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [middleRow.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12]
    ]];
}

#pragma mark - Slide-Over Menu & Diagnostics Drawer

- (void)setupDrawerMenu {
    _drawerOverlay = [UIView new];
    _drawerOverlay.translatesAutoresizingMaskIntoConstraints = NO;
    _drawerOverlay.backgroundColor = [UIColor colorWithRed:0.04 green:0.05 blue:0.08 alpha:0.94];
    _drawerOverlay.layer.cornerRadius = 16.0;
    _drawerOverlay.layer.borderWidth = 1.0;
    _drawerOverlay.layer.borderColor = [UIColor colorWithRed:0.85 green:0.75 blue:0.55 alpha:0.35].CGColor;
    _drawerOverlay.clipsToBounds = YES;
    _drawerOverlay.hidden = YES;
    _drawerOverlay.alpha = 0.0;
    [self.view addSubview:_drawerOverlay];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_drawerOverlay.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
        [_drawerOverlay.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
        [_drawerOverlay.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [_drawerOverlay.widthAnchor constraintLessThanOrEqualToConstant:620],
        [_drawerOverlay.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:20],
        [_drawerOverlay.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-20]
    ]];

    UIScrollView *drawerScroll = [UIScrollView new];
    drawerScroll.translatesAutoresizingMaskIntoConstraints = NO;
    [_drawerOverlay addSubview:drawerScroll];

    [NSLayoutConstraint activateConstraints:@[
        [drawerScroll.topAnchor constraintEqualToAnchor:_drawerOverlay.topAnchor constant:14],
        [drawerScroll.bottomAnchor constraintEqualToAnchor:_drawerOverlay.bottomAnchor constant:-14],
        [drawerScroll.leadingAnchor constraintEqualToAnchor:_drawerOverlay.leadingAnchor constant:18],
        [drawerScroll.trailingAnchor constraintEqualToAnchor:_drawerOverlay.trailingAnchor constant:-18]
    ]];

    UILabel *drawerTitle = [UILabel new];
    drawerTitle.font = [UIFont boldSystemFontOfSize:18];
    drawerTitle.textColor = [UIColor colorWithRed:0.95 green:0.85 blue:0.65 alpha:1.0];
    drawerTitle.text = @"⚔ Painel Bloodborne iOS — Controle e Diagnóstico";
    drawerTitle.textAlignment = NSTextAlignmentCenter;

    UIButton *newGameBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [newGameBtn setTitle:@"🩸 Novo Jogo / Introdução (Vídeo & Criação)" forState:UIControlStateNormal];
    newGameBtn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    newGameBtn.backgroundColor = [UIColor colorWithRed:0.75 green:0.15 blue:0.15 alpha:0.35];
    newGameBtn.layer.cornerRadius = 8;
    [newGameBtn addTarget:self action:@selector(startNewGameWithIntro) forControlEvents:UIControlEventTouchUpInside];

    _gameBootButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_gameBootButton setTitle:@"▶ Iniciar / Reiniciar Motor Bloodborne (Auto)" forState:UIControlStateNormal];
    _gameBootButton.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    _gameBootButton.backgroundColor = [UIColor colorWithRed:0.1 green:0.4 blue:0.9 alpha:0.25];
    _gameBootButton.layer.cornerRadius = 8;
    [_gameBootButton addTarget:self action:@selector(initGameBoot) forControlEvents:UIControlEventTouchUpInside];

    _continueSaveButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_continueSaveButton setTitle:@"🗡 Continuar Caçada (Carregar Save)" forState:UIControlStateNormal];
    _continueSaveButton.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    _continueSaveButton.backgroundColor = [UIColor colorWithRed:0.85 green:0.65 blue:0.15 alpha:0.35];
    _continueSaveButton.layer.cornerRadius = 8;
    [_continueSaveButton addTarget:self action:@selector(continueSavedGame) forControlEvents:UIControlEventTouchUpInside];

    _continuousButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_continuousButton setTitle:@"▶ Executar Continuamente (60 FPS)" forState:UIControlStateNormal];
    _continuousButton.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    _continuousButton.backgroundColor = [UIColor colorWithRed:0.1 green:0.7 blue:0.3 alpha:0.25];
    _continuousButton.layer.cornerRadius = 8;
    [_continuousButton addTarget:self action:@selector(toggleContinuousExecution) forControlEvents:UIControlEventTouchUpInside];

    UIButton *toggleControlsBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [toggleControlsBtn setTitle:@"🎮 Ocultar / Mostrar Controles na Tela" forState:UIControlStateNormal];
    toggleControlsBtn.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    toggleControlsBtn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:0.4];
    toggleControlsBtn.layer.cornerRadius = 8;
    [toggleControlsBtn addTarget:self action:@selector(toggleControlsVisibility) forControlEvents:UIControlEventTouchUpInside];

    UIButton *verifyDocsBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [verifyDocsBtn setTitle:@"📂 Verificar Arquivos em Documentos" forState:UIControlStateNormal];
    verifyDocsBtn.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    verifyDocsBtn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:0.4];
    verifyDocsBtn.layer.cornerRadius = 8;
    [verifyDocsBtn addTarget:self action:@selector(checkDocumentsInventory) forControlEvents:UIControlEventTouchUpInside];

    UIButton *runTestsBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [runTestsBtn setTitle:@"⚡ Executar Testes de Validação Internos" forState:UIControlStateNormal];
    runTestsBtn.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    runTestsBtn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:0.4];
    runTestsBtn.layer.cornerRadius = 8;
    [runTestsBtn addTarget:self action:@selector(runTests) forControlEvents:UIControlEventTouchUpInside];

    UIButton *exportBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [exportBtn setTitle:@"📤 Exportar Relatório de Desempenho (JSON)" forState:UIControlStateNormal];
    exportBtn.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    exportBtn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:0.4];
    exportBtn.layer.cornerRadius = 8;
    [exportBtn addTarget:self action:@selector(exportReport:) forControlEvents:UIControlEventTouchUpInside];

    UIButton *closeBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [closeBtn setTitle:@"✖ Fechar Menu" forState:UIControlStateNormal];
    closeBtn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    closeBtn.backgroundColor = [UIColor colorWithRed:0.8 green:0.2 blue:0.2 alpha:0.3];
    closeBtn.layer.cornerRadius = 8;
    [closeBtn addTarget:self action:@selector(toggleDrawerMenu) forControlEvents:UIControlEventTouchUpInside];

    _gameBootStatus = [UILabel new];
    _gameBootStatus.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    _gameBootStatus.textColor = [UIColor whiteColor];
    _gameBootStatus.numberOfLines = 0;
    _gameBootStatus.text = @"Motor: aguardando inicialização.";

    _status = [UILabel new];
    _status.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    _status.textColor = [UIColor secondaryLabelColor];
    _status.numberOfLines = 0;
    _status.text = @"CPU: pronto";

    _gpuStatus = [UILabel new];
    _gpuStatus.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    _gpuStatus.textColor = [UIColor secondaryLabelColor];
    _gpuStatus.numberOfLines = 0;
    _gpuStatus.text = @"GPU Metal 3: inicializado";

    _subsystemStatus = [UILabel new];
    _subsystemStatus.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    _subsystemStatus.textColor = [UIColor secondaryLabelColor];
    _subsystemStatus.numberOfLines = 0;
    _subsystemStatus.text = @"Subsistemas: CoreAudio e GameController prontos";

    _documentStatus = [UILabel new]; _documentStatus.numberOfLines = 0;
    _assetStatus = [UILabel new]; _assetStatus.numberOfLines = 0;
    _dcxStatus = [UILabel new]; _dcxStatus.numberOfLines = 0;
    _bootStatus = [UILabel new]; _bootStatus.numberOfLines = 0;

    UIStackView *drawerStack = [[UIStackView alloc] initWithArrangedSubviews:@[
        drawerTitle, newGameBtn, _gameBootButton, _continueSaveButton, _continuousButton, toggleControlsBtn,
        verifyDocsBtn, runTestsBtn, exportBtn, closeBtn,
        _gameBootStatus, _status, _gpuStatus, _subsystemStatus
    ]];
    drawerStack.axis = UILayoutConstraintAxisVertical;
    drawerStack.spacing = 10;
    drawerStack.translatesAutoresizingMaskIntoConstraints = NO;
    [drawerScroll addSubview:drawerStack];

    [NSLayoutConstraint activateConstraints:@[
        [drawerStack.topAnchor constraintEqualToAnchor:drawerScroll.contentLayoutGuide.topAnchor],
        [drawerStack.bottomAnchor constraintEqualToAnchor:drawerScroll.contentLayoutGuide.bottomAnchor],
        [drawerStack.leadingAnchor constraintEqualToAnchor:drawerScroll.contentLayoutGuide.leadingAnchor],
        [drawerStack.trailingAnchor constraintEqualToAnchor:drawerScroll.contentLayoutGuide.trailingAnchor],
        [drawerStack.widthAnchor constraintEqualToAnchor:drawerScroll.frameLayoutGuide.widthAnchor]
    ]];
}

- (void)toggleDrawerMenu {
    BOOL show = _drawerOverlay.hidden;
    if (show) {
        _drawerOverlay.hidden = NO;
        [UIView animateWithDuration:0.25 animations:^{
            self->_drawerOverlay.alpha = 1.0;
        }];
    } else {
        [UIView animateWithDuration:0.20 animations:^{
            self->_drawerOverlay.alpha = 0.0;
        } completion:^(BOOL finished) {
            if (finished) self->_drawerOverlay.hidden = YES;
        }];
    }
}

- (void)toggleControlsVisibility {
    _virtualControlsVisible = !_virtualControlsVisible;
    _controlsOverlay.hidden = !_virtualControlsVisible;
}

- (void)continueSavedGame {
    NSURL *documents = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSString *saveDir = [documents.path stringByAppendingPathComponent:@"savedata"];

    auto& sm = _bootCoordinator ? _bootCoordinator->save_manager() : bb::SaveDataManager::instance();
    if (!sm.is_initialized()) {
        sm.initialize(saveDir.UTF8String, 16 * 1024 * 1024);
    }
    sm.set_default_title_id("CUSA03173");

    std::vector<bb::SaveSearchEntry> entries;
    int32_t ret = sm.dir_name_search(0, "CUSA03173", "%", SCE_SAVEDATA_SORT_KEY_MTIME, SCE_SAVEDATA_SORT_ORDER_DESC, entries);
    if (ret != SCE_SAVEDATA_OK || entries.empty()) {
        _gameBootStatus.text = @"Save: Nenhum progresso salvo encontrado em Documents/savedata.";
        _topStatusLabel.text = @"🗡 Nenhum Save Encontrado";
        return;
    }

    const auto& latest = entries[0];
    _selectedSaveSlot = [NSString stringWithUTF8String:latest.dir_name.c_str()];
    std::string mountPt;
    uint32_t status = 0;
    int32_t mRet = sm.mount(0, "CUSA03173", latest.dir_name.c_str(), SCE_SAVEDATA_MOUNT_MODE_RDWR, 8192, mountPt, status);
    if (mRet == SCE_SAVEDATA_ERROR_BAD_MOUNTED) {
        for (std::size_t i = 0; i < bb::SaveDataManager::MAX_SLOTS; ++i) {
            const auto *slot = sm.get_slot(static_cast<int>(i));
            if (slot && slot->dir_name == latest.dir_name) {
                mountPt = slot->mount_point;
                status = SCE_SAVEDATA_MOUNT_STATUS_EXISTING;
                mRet = SCE_SAVEDATA_OK;
                break;
            }
        }
    }

    if (mRet != SCE_SAVEDATA_OK) {
        _gameBootStatus.text = [NSString stringWithFormat:@"Save: Erro ao montar slot (%d). Save anterior preservado com integridade.", mRet];
        _topStatusLabel.text = @"⚠️ Falha ao Montar Save";
        _report[@"save_error"] = [NSString stringWithFormat:@"Falha na montagem de save: erro %d; dados anteriores preservados.", mRet];
        return;
    }

    NSString *saveTitle = latest.param.title[0] != '\0' ? [NSString stringWithUTF8String:latest.param.title] : [NSString stringWithUTF8String:latest.dir_name.c_str()];
    _gameBootStatus.text = [NSString stringWithFormat:@"Save Carregado: %@ (Slot: %s, Status: %u)", saveTitle, latest.dir_name.c_str(), status];
    _topStatusLabel.text = [NSString stringWithFormat:@"🗡 Progresso: %@", saveTitle];
    _report[@"save_profile_loaded"] = @{
        @"slot": [NSString stringWithUTF8String:latest.dir_name.c_str()],
        @"title": saveTitle,
        @"mount_point": [NSString stringWithUTF8String:mountPt.c_str()],
        @"mount_status": @(status)
    };

    if (!_guestCanStep.load() && !_guestBooting.load()) {
        [self initGameBoot];
    }
}

#pragma mark - Auto-Boot and Search

- (NSString *)findGameDirectoryIn:(NSString *)baseDir {
    if (!baseDir || baseDir.length == 0) return nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:[baseDir stringByAppendingPathComponent:@"eboot.bin"]] ||
        [fm fileExistsAtPath:[baseDir stringByAppendingPathComponent:@"eboot.elf"]]) {
        return baseDir;
    }

    NSArray<NSString *> *candidates = @[
        @"Image0",
        @"Bloodborne™/Image0",
        @"Bloodborne/Image0",
        @"blodboorn 3171/Bloodborne™/Image0",
        @"app0",
        @"game",
        @"CUSA03173",
        @"CUSA00207"
    ];
    for (NSString *sub in candidates) {
        NSString *candidatePath = [baseDir stringByAppendingPathComponent:sub];
        if ([fm fileExistsAtPath:[candidatePath stringByAppendingPathComponent:@"eboot.bin"]] ||
            [fm fileExistsAtPath:[candidatePath stringByAppendingPathComponent:@"eboot.elf"]]) {
            return candidatePath;
        }
    }

    NSArray<NSString *> *contents = [fm contentsOfDirectoryAtPath:baseDir error:nil];
    for (NSString *item in contents) {
        NSString *subPath = [baseDir stringByAppendingPathComponent:item];
        BOOL isDir = NO;
        if ([fm fileExistsAtPath:subPath isDirectory:&isDir] && isDir) {
            if ([fm fileExistsAtPath:[subPath stringByAppendingPathComponent:@"eboot.bin"]] ||
                [fm fileExistsAtPath:[subPath stringByAppendingPathComponent:@"eboot.elf"]]) {
                return subPath;
            }
            NSArray<NSString *> *subContents = [fm contentsOfDirectoryAtPath:subPath error:nil];
            for (NSString *subItem in subContents) {
                NSString *deepPath = [subPath stringByAppendingPathComponent:subItem];
                if ([fm fileExistsAtPath:deepPath isDirectory:&isDir] && isDir) {
                    if ([fm fileExistsAtPath:[deepPath stringByAppendingPathComponent:@"eboot.bin"]] ||
                        [fm fileExistsAtPath:[deepPath stringByAppendingPathComponent:@"eboot.elf"]]) {
                        return deepPath;
                    }
                }
            }
        }
    }
    return nil;
}

- (void)attemptAutoBoot {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    if (!documents) return;

    NSString *gameDir = [self findGameDirectoryIn:documents.path];
    if (gameDir) {
        [self initGameBoot];
    } else {
        [self showWelcomeCard];
    }
}

- (void)showWelcomeCard {
    if (_welcomeCardView) {
        _welcomeCardView.hidden = NO;
        return;
    }

    _welcomeCardView = [UIView new];
    _welcomeCardView.translatesAutoresizingMaskIntoConstraints = NO;
    _welcomeCardView.backgroundColor = [UIColor colorWithRed:0.04 green:0.05 blue:0.08 alpha:0.92];
    _welcomeCardView.layer.cornerRadius = 16.0;
    _welcomeCardView.layer.borderWidth = 1.0;
    _welcomeCardView.layer.borderColor = [UIColor colorWithRed:0.85 green:0.75 blue:0.55 alpha:0.35].CGColor;
    _welcomeCardView.clipsToBounds = YES;
    [self.view addSubview:_welcomeCardView];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_welcomeCardView.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [_welcomeCardView.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [_welcomeCardView.widthAnchor constraintLessThanOrEqualToConstant:540],
        [_welcomeCardView.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24],
        [_welcomeCardView.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-24]
    ]];

    UILabel *title = [UILabel new];
    title.font = [UIFont boldSystemFontOfSize:22];
    title.textColor = [UIColor colorWithRed:0.95 green:0.85 blue:0.65 alpha:1.0];
    title.text = @"⚔ BLOODBORNE MOBILE";
    title.textAlignment = NSTextAlignmentCenter;

    UILabel *sub = [UILabel new];
    sub.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    sub.textColor = [UIColor colorWithWhite:0.78 alpha:1.0];
    sub.text = @"Laboratório Nativo iOS 17 (Apple A17 Pro / Metal 3)";
    sub.textAlignment = NSTextAlignmentCenter;

    UILabel *desc = [UILabel new];
    desc.font = [UIFont systemFontOfSize:12];
    desc.textColor = [UIColor colorWithWhite:0.65 alpha:1.0];
    desc.numberOfLines = 0;
    desc.textAlignment = NSTextAlignmentCenter;
    desc.text = @"O motor gráfico Metal e o escalonador estão ativos.\n"
                @"Para testar a execução do jogo no iPhone, transfira eboot.bin e a pasta sce_module\n"
                @"(ou o pacote completo com dvdroot_ps4) para a pasta Documentos do BBHost.\n"
                @"Transfira pelo iTunes, AltStore, Apple Devices ou aplicativo Arquivos.";

    UIButton *exploreBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [exploreBtn setTitle:@"▶ Entrar no Modo Demonstração 3D" forState:UIControlStateNormal];
    exploreBtn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    exploreBtn.backgroundColor = [UIColor colorWithRed:0.1 green:0.5 blue:0.9 alpha:0.3];
    exploreBtn.layer.cornerRadius = 8;
    [exploreBtn addTarget:self action:@selector(dismissWelcomeCard) forControlEvents:UIControlEventTouchUpInside];

    UIButton *rescanBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [rescanBtn setTitle:@"🔄 Atualizar e Procurar Arquivos" forState:UIControlStateNormal];
    rescanBtn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    rescanBtn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:0.4];
    rescanBtn.layer.cornerRadius = 8;
    [rescanBtn addTarget:self action:@selector(attemptAutoBoot) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *cardStack = [[UIStackView alloc] initWithArrangedSubviews:@[title, sub, desc, exploreBtn, rescanBtn]];
    cardStack.axis = UILayoutConstraintAxisVertical;
    cardStack.spacing = 12;
    cardStack.translatesAutoresizingMaskIntoConstraints = NO;
    [_welcomeCardView addSubview:cardStack];

    [NSLayoutConstraint activateConstraints:@[
        [cardStack.topAnchor constraintEqualToAnchor:_welcomeCardView.topAnchor constant:20],
        [cardStack.bottomAnchor constraintEqualToAnchor:_welcomeCardView.bottomAnchor constant:-20],
        [cardStack.leadingAnchor constraintEqualToAnchor:_welcomeCardView.leadingAnchor constant:20],
        [cardStack.trailingAnchor constraintEqualToAnchor:_welcomeCardView.trailingAnchor constant:-20]
    ]];
}

- (void)dismissWelcomeCard {
    if (_welcomeCardView) {
        [UIView animateWithDuration:0.25 animations:^{
            self->_welcomeCardView.alpha = 0.0;
        } completion:^(BOOL finished) {
            if (finished) self->_welcomeCardView.hidden = YES;
        }];
    }
}

#pragma mark - Game Execution Engine

- (void)startNewGameWithIntro {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    NSString *docsPath = documents ? documents.path : @"";
    NSString *introMoviePath = [docsPath stringByAppendingPathComponent:@"intro.mp4"];
    NSString *dvdrootMoviePath = [docsPath stringByAppendingPathComponent:@"dvdroot_ps4/movies/intro.mp4"];

    NSString *chosenPath = nil;
    if ([[NSFileManager defaultManager] fileExistsAtPath:introMoviePath]) {
        chosenPath = introMoviePath;
    } else if ([[NSFileManager defaultManager] fileExistsAtPath:dvdrootMoviePath]) {
        chosenPath = dvdrootMoviePath;
    }

    if (chosenPath) {
        // T114: Valida legibilidade básica do arquivo de mídia antes de inicializar o player
        NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:chosenPath error:nil];
        if (!attrs || attrs.fileSize < 16) {
            _report[@"intro_video_status"] = @"offline_fallback_unreadable_media";
            _status.text = @"Mídia de introdução inválida ou ilegível. Fallback offline ativado para Novo Jogo.";
            [self initGameBoot];
            return;
        }

        _report[@"intro_video_status"] = @"playing";
        _status.text = @"Reproduzindo introdução cinematográfica do jogo...";
        NSURL *movieURL = [NSURL fileURLWithPath:chosenPath];
        AVPlayerItem *playerItem = [AVPlayerItem playerItemWithURL:movieURL];
        AVPlayer *player = [AVPlayer playerWithPlayerItem:playerItem];
        AVPlayerViewController *playerVC = [AVPlayerViewController new];
        playerVC.player = player;
        playerVC.showsPlaybackControls = YES;

        __weak LabController *weakSelf = self;
        __block BOOL transitioned = NO;
        __block id endObserver = nil;
        __block id failObserver = nil;

        void (^finishIntroAndBoot)(NSString *, NSString *) = ^(NSString *statusMsg, NSString *reportStatus) {
            if (transitioned) return;
            transitioned = YES;
            if (endObserver) {
                [[NSNotificationCenter defaultCenter] removeObserver:endObserver];
                endObserver = nil;
            }
            if (failObserver) {
                [[NSNotificationCenter defaultCenter] removeObserver:failObserver];
                failObserver = nil;
            }
            [player pause];
            [playerVC dismissViewControllerAnimated:YES completion:^{
                LabController *sSelf = weakSelf;
                if (!sSelf) return;
                sSelf->_report[@"intro_video_status"] = reportStatus;
                sSelf->_status.text = statusMsg;
                [sSelf initGameBoot];
            }];
        };

        // Botão de pular/cancelar reprodução na tela (T114 / US2/AC1)
        UIButton *skipBtn = [UIButton buttonWithType:UIButtonTypeSystem];
        skipBtn.translatesAutoresizingMaskIntoConstraints = NO;
        [skipBtn setTitle:@"⏩ Pular Introdução" forState:UIControlStateNormal];
        skipBtn.titleLabel.font = [UIFont boldSystemFontOfSize:13];
        [skipBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
        skipBtn.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.75];
        skipBtn.layer.cornerRadius = 8;
        skipBtn.layer.borderWidth = 1.0;
        skipBtn.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.25].CGColor;
        [playerVC.contentOverlayView addSubview:skipBtn];
        [NSLayoutConstraint activateConstraints:@[
            [skipBtn.topAnchor constraintEqualToAnchor:playerVC.contentOverlayView.safeAreaLayoutGuide.topAnchor constant:16],
            [skipBtn.trailingAnchor constraintEqualToAnchor:playerVC.contentOverlayView.safeAreaLayoutGuide.trailingAnchor constant:-16],
            [skipBtn.widthAnchor constraintEqualToConstant:150],
            [skipBtn.heightAnchor constraintEqualToConstant:36]
        ]];

        endObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                        object:playerItem
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification * _Nonnull note) {
            (void)note;
            finishIntroAndBoot(@"Introdução concluída. Transição para Novo Jogo...", @"finished");
        }];

        failObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                        object:playerItem
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification * _Nonnull note) {
            (void)note;
            finishIntroAndBoot(@"Falha na reprodução da introdução. Ativando fallback offline...",
                               @"offline_fallback_playback_failed");
        }];

        [self presentViewController:playerVC animated:YES completion:^{
            if (playerItem.status == AVPlayerItemStatusFailed) {
                finishIntroAndBoot(@"Mídia de introdução inválida ou ilegível. Ativando fallback offline...",
                                   @"offline_fallback_unreadable_media");
            } else {
                [player play];
            }
        }];
    } else {
        // T059: Fallback offline explícito sem afirmar falso sucesso para dados inexistentes
        _report[@"intro_video_status"] = @"offline_fallback_missing_media";
        _status.text = @"Vídeo intro não encontrado em Documents. Fallback offline ativado para Novo Jogo.";
        [self initGameBoot];
    }
}

- (void)initGameBoot {
    if (_guestBooting.exchange(true)) {
        return;
    }
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    if (!documents) {
        _gameBootStatus.text = @"Motor Bloodborne: pasta Documents indisponível.";
        _guestBooting.store(false);
        return;
    }

    NSString *gameDir = [self findGameDirectoryIn:documents.path];
    if (!gameDir) {
        _gameBootStatus.text = @"Motor: eboot.bin não encontrado em Documentos.";
        [self showWelcomeCard];
        _guestBooting.store(false);
        return;
    }
    NSString *dataDir = [gameDir stringByAppendingPathComponent:@"dvdroot_ps4"];
    BOOL isDirectory = NO;
    const BOOL hasDvdroot = [[NSFileManager defaultManager] fileExistsAtPath:dataDir isDirectory:&isDirectory] && isDirectory;
    _report[@"has_dvdroot_ps4"] = @(hasDvdroot);
    if (!hasDvdroot) {
        _gameBootStatus.text = @"Iniciando motor e módulos em segundo plano (sem dvdroot_ps4)...";
    } else {
        _gameBootStatus.text = @"Iniciando motor e pacotes em segundo plano...";
    }
    _topStatusLabel.text = @"⚙ Carregando módulos...";

    bb::BootConfig config;
    config.base_dir = gameDir.UTF8String;
    config.save_dir = [documents.path stringByAppendingPathComponent:@"savedata"].UTF8String;
    if (_selectedSaveSlot && _selectedSaveSlot.length > 0) {
        config.save_dir_name = _selectedSaveSlot.UTF8String;
    }
    const unsigned long long physRam = [NSProcessInfo processInfo].physicalMemory;
    if (physRam >= (7ULL << 30)) {
        config.guest_memory_size = static_cast<std::size_t>(5632ULL << 20); // 5.5 GiB (iPhone 15 Pro / Pro Max com 8 GB)
    } else if (physRam >= (5ULL << 30)) {
        config.guest_memory_size = static_cast<std::size_t>(4096ULL << 20); // 4.0 GiB (dispositivos com 6 GB)
    } else {
        config.guest_memory_size = static_cast<std::size_t>(3072ULL << 20); // 3.0 GiB
    }
    config.require_essential_assets = false;
    config.enable_hle = true;
    config.use_scheduler = true;

    // Desanexar na fila guest, após qualquer slice pendente, antes de destruir memória.
    _guestScreenTexture = nil;
    _report[@"guest_frame_flipped"] = @NO;
    _report[@"guest_frame_presented_to_metal"] = @NO;
    _report[@"gnm_draws_executed_offscreen"] = @NO;
    _report[@"guest_steps_executed"] = @NO;
    _report[@"game_execution"] = @NO;
    _guestCanStep.store(false);
    _guestSubmits.store(0);
    _guestTriangles.store(0);
    _guestRasterizerFailures.store(0);
    _guestLastRasterizerStatus.store(static_cast<int>(bb::RasterizerStatus::Ok));

    __weak LabController *weakSelf = self;
    dispatch_async(_guestExecutionQueue, ^{
        LabController *strongSelf = weakSelf;
        if (!strongSelf) return;

        strongSelf->_videoOutManager.detach_guest_memory();
        strongSelf->_bootCoordinator = std::make_unique<bb::GameBootCoordinator>();
        strongSelf->_bootCoordinator->attach_input_manager(&strongSelf->_inputManager);
        strongSelf->_bootCoordinator->attach_video_out(&strongSelf->_videoOutManager);

        bool ok = strongSelf->_bootCoordinator->phase1_mount_packages(config) &&
                  strongSelf->_bootCoordinator->phase2_setup_saves(config) &&
                  strongSelf->_bootCoordinator->phase3_load_modules(config);
        if (ok) {
            strongSelf->_videoOutManager.attach_guest_memory(strongSelf->_bootCoordinator->guest_memory(), &strongSelf->_guestFlipSink);
            ok = strongSelf->_bootCoordinator->phase4_initialize_runtime(config);
        }
        auto diag = strongSelf->_bootCoordinator->diagnostics();

        if (!ok) {
            dispatch_async(dispatch_get_main_queue(), ^{
                LabController *sSelf = weakSelf;
                if (!sSelf) return;
                sSelf->_guestBooting.store(false);
                sSelf->_continuousExecution = NO;
                [sSelf->_continuousButton setTitle:@"▶ Executar Continuamente (60 FPS)" forState:UIControlStateNormal];
                NSString *err = [NSString stringWithUTF8String:diag.last_error.c_str()];
                sSelf->_gameBootStatus.text = [NSString stringWithFormat:@"Motor Bloodborne: FALHA: %@", err];
                sSelf->_topStatusLabel.text = [NSString stringWithFormat:@"⚔ Falha: %@", err];
                sSelf->_report[@"game_boot_coordinator"] = @{
                    @"status": @"init_failed",
                    @"error": err,
                    @"state": [NSString stringWithUTF8String:diag.state_name.c_str()],
                    @"active_modules": @(diag.modules_count),
                    @"total_steps": @(diag.total_steps),
                    @"guest_mapped_bytes": @(diag.guest_mapped_bytes),
                    @"guest_peak_mapped_bytes": @(diag.guest_peak_mapped_bytes),
                    @"guest_budget_bytes": @(diag.guest_budget_bytes),
                    @"game_execution": @NO
                };
                sSelf->_report[@"game_execution"] = @NO;
            });
            return;
        }

        if (strongSelf->_bootCoordinator->runtime()) {
            strongSelf->_bootCoordinator->runtime()->pad_manager().set_default_input(&strongSelf->_inputManager);
            strongSelf->_bootCoordinator->runtime()->attach_video_out(&strongSelf->_videoOutManager);
            strongSelf->_gnmBridgeListener = std::make_unique<HostGnmSubmissionBridge>(strongSelf);
            strongSelf->_bootCoordinator->runtime()->set_gnm_listener(strongSelf->_gnmBridgeListener.get());
        }

        std::uint64_t steps = strongSelf->_bootCoordinator->step(100000);
        diag = strongSelf->_bootCoordinator->diagnostics();
        if (auto *runtime = strongSelf->_bootCoordinator->runtime()) {
            strongSelf->_guestSubmits.store(runtime->submit_count());
            strongSelf->_guestTriangles.store(runtime->rasterizer_stats().triangles_drawn);
            strongSelf->_guestRasterizerFailures.store(runtime->rasterizer_failure_count());
            strongSelf->_guestLastRasterizerStatus.store(static_cast<int>(runtime->last_rasterizer_status()));
        }
        strongSelf->_guestCanStep.store(diag.state == bb::BootState::running ||
                                        diag.state == bb::BootState::modules_initialized);

        dispatch_async(dispatch_get_main_queue(), ^{
            LabController *sSelf = weakSelf;
            if (!sSelf) return;
            sSelf->_guestBooting.store(false);
            sSelf->_lastGuestSteps = diag.total_steps;
            const bool is_running = (diag.total_steps > 0);
            sSelf->_report[@"guest_steps_executed"] = @(is_running);
            sSelf->_report[@"game_execution"] = @NO; // Passos do interpretador não comprovam gameplay.
            sSelf->_continuousExecution = (diag.state == bb::BootState::running || diag.state == bb::BootState::modules_initialized);
            [sSelf->_continuousButton setTitle:sSelf->_continuousExecution ? @"⏸ Pausar Execução Contínua" : @"▶ Executar Continuamente (60 FPS)" forState:UIControlStateNormal];

            if (sSelf->_welcomeCardView) {
                sSelf->_welcomeCardView.hidden = YES;
            }

            [sSelf updateCoordinatorReportAndUI:diag executedSteps:steps];

            // T112: Se o app estiver em background ou houver pausa ativa ao concluir o boot,
            // congela o coordenador e não agenda execução guest em background
            BOOL inBackground = ([UIApplication sharedApplication].applicationState == UIApplicationStateBackground);
            if (inBackground) {
                sSelf->_pauseReasons |= PauseReasonLifecycle;
            }
            if (sSelf->_pauseReasons != PauseReasonNone) {
                if (sSelf->_bootCoordinator) {
                    sSelf->_bootCoordinator->add_pause_reason(static_cast<bb::PauseReason>(sSelf->_pauseReasons));
                }
            } else if (sSelf->_continuousExecution) {
                [sSelf scheduleGuestCpuStep];
            }
        });
    });
}

- (void)toggleContinuousExecution {
    if (!_guestCanStep.load()) return;
    _continuousExecution = !_continuousExecution;
    if (_continuousExecution) {
        _pauseReasons &= ~PauseReasonUser;
        if (_bootCoordinator) {
            _bootCoordinator->clear_pause_reason(bb::PauseReason::user);
        }
        [_continuousButton setTitle:@"⏸ Pausar Execução Contínua" forState:UIControlStateNormal];
        if (_pauseReasons == PauseReasonNone) {
            [self scheduleGuestCpuStep];
        }
    } else {
        _pauseReasons |= PauseReasonUser;
        if (_bootCoordinator) {
            _bootCoordinator->add_pause_reason(bb::PauseReason::user);
        }
        [_continuousButton setTitle:@"▶ Executar Continuamente (60 FPS)" forState:UIControlStateNormal];
    }
}

- (void)scheduleGuestCpuStep {
    if (!_continuousExecution || _pauseReasons != PauseReasonNone || !_guestCanStep.load() || _guestBooting.load()) {
        _guestStepping.store(false);
        return;
    }
    if (_guestStepping.exchange(true)) {
        return;
    }
    __weak LabController *weakSelf = self;
    dispatch_async(_guestExecutionQueue, ^{
        LabController *strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_continuousExecution || !strongSelf->_bootCoordinator) {
            if (strongSelf) strongSelf->_guestStepping.store(false);
            return;
        }
        std::uint32_t slice_budget = strongSelf->_guestCpuSliceBudget.load();
        std::uint64_t steps = strongSelf->_bootCoordinator->step(slice_budget);
        auto diag = strongSelf->_bootCoordinator->diagnostics();
        if (auto *runtime = strongSelf->_bootCoordinator->runtime()) {
            strongSelf->_guestSubmits.store(runtime->submit_count());
            strongSelf->_guestTriangles.store(runtime->rasterizer_stats().triangles_drawn);
            strongSelf->_guestRasterizerFailures.store(runtime->rasterizer_failure_count());
            strongSelf->_guestLastRasterizerStatus.store(static_cast<int>(runtime->last_rasterizer_status()));
        }
        strongSelf->_guestCanStep.store(diag.state == bb::BootState::running ||
                                        diag.state == bb::BootState::modules_initialized);
        strongSelf->_guestStepping.store(false, std::memory_order_release);

        dispatch_async(dispatch_get_main_queue(), ^{
            LabController *sSelf = weakSelf;
            if (!sSelf) return;
            sSelf->_lastGuestSteps = diag.total_steps;
            sSelf->_report[@"guest_steps_executed"] = @(diag.total_steps > 0);
            if (diag.state == bb::BootState::error || diag.state == bb::BootState::halted) {
                sSelf->_continuousExecution = NO;
                [sSelf->_continuousButton setTitle:@"▶ Executar Continuamente (60 FPS)" forState:UIControlStateNormal];
            }
            if (diag.state == bb::BootState::error || diag.state == bb::BootState::halted ||
                diag.total_steps % 500000 < 50000) {
                [sSelf updateCoordinatorReportAndUI:diag executedSteps:steps];
            }
            // Avanço autônomo desacoplado com controle de pressão térmica (T049 / T182 / US5/AC1)
            if (sSelf->_continuousExecution && sSelf->_guestCanStep.load() && !sSelf->_guestBooting.load()) {
                std::uint32_t delay_ms = sSelf->_guestCpuRescheduleDelayMs.load();
                if (delay_ms > 0) {
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_ms * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
                        LabController *again = weakSelf;
                        if (again && again->_continuousExecution && again->_guestCanStep.load() && !again->_guestBooting.load()) {
                            [again scheduleGuestCpuStep];
                        }
                    });
                } else {
                    [sSelf scheduleGuestCpuStep];
                }
            }
        });
    });
}

- (void)updateCoordinatorReportAndUI:(const bb::BootDiagnostics&)diag executedSteps:(std::uint64_t)steps {
    NSMutableDictionary *importCounts = [NSMutableDictionary dictionary];
    for (const auto& [name, count] : diag.import_call_counts) {
        importCounts[[NSString stringWithUTF8String:name.c_str()]] = @(count);
    }
    NSMutableArray *modules = [NSMutableArray array];
    for (const auto& m : diag.active_modules) {
        [modules addObject:[NSString stringWithUTF8String:m.c_str()]];
    }

    const bool is_running = (diag.total_steps > 0);
    _report[@"game_boot_coordinator"] = @{
        @"status": (diag.state == bb::BootState::halted ? @"halted" : (diag.state == bb::BootState::error ? @"error" : @"running")),
        @"state": [NSString stringWithUTF8String:diag.state_name.c_str()],
        @"total_steps": @(diag.total_steps),
        @"total_import_calls": @(diag.total_import_calls),
        @"last_import_called": [NSString stringWithUTF8String:diag.last_import_called.c_str()],
        @"import_call_counts": importCounts,
        @"active_modules": modules,
        @"modules_count": @(diag.modules_count),
        @"entry_point": @(diag.entry_point),
        @"current_rip": @(diag.current_rip),
        @"last_stop_reason": [NSString stringWithUTF8String:diag.last_stop_reason.c_str()],
        @"last_error": [NSString stringWithUTF8String:diag.last_error.c_str()],
        @"guest_mapped_bytes": @(diag.guest_mapped_bytes),
        @"guest_peak_mapped_bytes": @(diag.guest_peak_mapped_bytes),
        @"guest_budget_bytes": @(diag.guest_budget_bytes),
        @"guest_steps_executed": @(is_running),
        @"game_execution": @NO,
        @"checked_at": [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]]
    };

    _gameBootStatus.text = [NSString stringWithFormat:
        @"Guest (%s, +%llu): %llu passos | %llu imports | RIP: 0x%llx | Stop: %s | Erro: %s",
        diag.state_name.c_str(),
        (unsigned long long)steps,
        (unsigned long long)diag.total_steps,
        (unsigned long long)diag.total_import_calls,
        (unsigned long long)diag.current_rip,
        diag.last_stop_reason.c_str(),
        diag.last_error.c_str()];

    _topStatusLabel.text = [NSString stringWithFormat:@"BB Lab | %.1fM passos | %s (não jogável)",
        static_cast<double>(diag.total_steps) / 1000000.0,
        diag.state_name.c_str()];
}

#pragma mark - MTKViewDelegate (Render & Engine Loop)

- (void)drawInMTKView:(MTKView *)view {
    MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!pass || !drawable) return;

    auto now = std::chrono::steady_clock::now();
    double frame_ms = std::chrono::duration<double, std::milli>(now - _lastFrameTime).count();
    _lastFrameTime = now;
    _pacingTracker.record_frame_ms(frame_ms);

    if (_videoOutManager.has_new_frame()) {
        bb::PresentationFrame guestFrame;
        if (_videoOutManager.read_presentation_frame(guestFrame) && guestFrame.valid &&
            (guestFrame.bytes_per_pixel == 4 || guestFrame.bytes_per_pixel == 8) &&
            guestFrame.width > 0 && guestFrame.height > 0 &&
            guestFrame.pitch >= guestFrame.width && !guestFrame.pixel_data.empty()) {
            _report[@"guest_frame_flipped"] = @YES;
            _report[@"guest_frame_seq"] = @(guestFrame.flip_sequence);
            _report[@"guest_frame_w"] = @(guestFrame.width);
            _report[@"guest_frame_h"] = @(guestFrame.height);
            _report[@"guest_frame_bpp"] = @(guestFrame.bytes_per_pixel);
            _report[@"guest_frame_was_tiled"] = @(guestFrame.was_tiled ? YES : NO);

            MTLPixelFormat mtl_pixel_fmt = (guestFrame.bytes_per_pixel == 8) ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;

            if (!_guestScreenTexture || _guestScreenTexture.width != guestFrame.width ||
                _guestScreenTexture.height != guestFrame.height || _guestScreenTexture.pixelFormat != mtl_pixel_fmt) {
                MTLTextureDescriptor *screenDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_pixel_fmt
                                                                                                     width:guestFrame.width
                                                                                                    height:guestFrame.height
                                                                                                 mipmapped:NO];
                screenDesc.usage = MTLTextureUsageShaderRead;
                _guestScreenTexture = [view.device newTextureWithDescriptor:screenDesc];
            }
            if (_guestScreenTexture && !guestFrame.pixel_data.empty()) {
                [_guestScreenTexture replaceRegion:MTLRegionMake2D(0, 0, guestFrame.width, guestFrame.height)
                                       mipmapLevel:0
                                         withBytes:guestFrame.pixel_data.data()
                                       bytesPerRow:static_cast<NSUInteger>(guestFrame.pitch) * guestFrame.bytes_per_pixel];
                const std::uint64_t submits = _guestSubmits.load();
                const std::uint32_t tris = _guestTriangles.load();
                _report[@"gnm_submit_count"] = @(submits);
                _report[@"gnm_cpu_triangles"] = @(tris);
                _report[@"gnm_cpu_rasterizer_failures"] = @(_guestRasterizerFailures.load());
                _report[@"gnm_cpu_last_rasterizer_status_code"] = @(_guestLastRasterizerStatus.load());
                _report[@"gnm_draws_executed_offscreen"] = @NO;
                _report[@"guest_frame_status"] = [NSString stringWithFormat:@"Pixels convertidos guest (%s, %u bpp) enviados a textura Metal.",
                                                  guestFrame.diagnostic.c_str(), guestFrame.bytes_per_pixel];
            } else {
                _report[@"guest_frame_status"] = @"Falha ao alocar textura de vídeo guest.";
            }
        } else {
            _guestScreenTexture = nil;
            _report[@"guest_frame_presented_to_metal"] = @NO;
            const char* diag = guestFrame.diagnostic.empty() ? _videoOutManager.last_conversion_diagnostic().c_str() : guestFrame.diagnostic.c_str();
            _report[@"guest_frame_status"] = [NSString stringWithFormat:@"Quadro rejeitado na conversao: %s", (diag && diag[0]) ? diag : "formato/tiling nao suportados"];
        }
        _videoOutManager.consume_new_frame();
    }

    // Atualização de rotação suave da câmera no modo demonstração 3D
    auto pad = _inputManager.poll();
    _angle += 0.015f + (pad.right_stick_x * 0.04f);
    _cameraPitch += (pad.right_stick_y * 0.03f);
    _cameraPitch = std::max(-1.1f, std::min(1.1f, _cameraPitch));

    float aspect = (view.bounds.size.height > 0) ? (float)view.bounds.size.width / (float)view.bounds.size.height : 1.777f;
    bb::Matrix4x4 proj = bb::Matrix4x4::perspective(55.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);
    float eye[3] = {0.0f, 1.0f, 2.8f};
    float target[3] = {0.0f, 0.3f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    bb::Matrix4x4 viewMat = bb::Matrix4x4::look_at(eye, target, up);
    bb::Matrix4x4 modelRot = bb::Matrix4x4::rotation_y(_angle);
    bb::Matrix4x4 modelPitch = bb::Matrix4x4::rotation_x(_cameraPitch);
    bb::Matrix4x4 model = bb::Matrix4x4::multiply(modelRot, modelPitch);

    bb::ModelViewProjectionUniforms mvpUniforms{};
    mvpUniforms.model = model;
    mvpUniforms.view = viewMat;
    mvpUniforms.projection = proj;
    bb::Matrix4x4 modelView = bb::Matrix4x4::multiply(viewMat, model);
    mvpUniforms.mvp = bb::Matrix4x4::multiply(proj, modelView);
    mvpUniforms.normal_matrix = bb::Matrix4x4::normal_matrix(modelView);

    id<MTLCommandBuffer> command = [_queue commandBuffer];
    if (!command) { [self gpuError:@"Falha ao criar command buffer"]; return; }
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { [self gpuError:@"Falha ao criar render encoder"]; return; }

    if (_guestScreenTexture && _screenPipeline && _screenSampler) {
        [encoder setRenderPipelineState:_screenPipeline];
        [encoder setFragmentTexture:_guestScreenTexture atIndex:0];
        [encoder setFragmentSamplerState:_screenSampler atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
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
                    strongSelf->_report[@"guest_frame_presented_to_metal"] = @YES;
                } else {
                    strongSelf->_report[@"guest_frame_presented_to_metal"] = @NO;
                    [strongSelf gpuError:finished.error ? finished.error.localizedDescription : @"Falha ao apresentar frame guest"];
                }
            });
        }];
        [command commit];
        return;
    }

    // Rota 2: Modo demonstrativo 3D independente da rota guest
    if (_pipeline && _vertexBuffer && _indexBuffer) {
        [encoder setRenderPipelineState:_pipeline];
        if (_depthState) {
            [encoder setDepthStencilState:_depthState];
        }
        [encoder setCullMode:MTLCullModeNone];

        [encoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
        [encoder setVertexBytes:&mvpUniforms length:sizeof(mvpUniforms) atIndex:1];
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

                        if (strongSelf->_lastGuestSteps > 0) {
                            strongSelf->_topStatusLabel.text = [NSString stringWithFormat:@"BB Lab | %.0f FPS host | %.1fM passos guest",
                                pacing.average_fps,
                                static_cast<double>(strongSelf->_lastGuestSteps) / 1000000.0];
                        }
                    }
                } else {
                    [strongSelf gpuError:finished.error ? finished.error.localizedDescription : @"Erro no command buffer"];
                }
            });
        }];
        [command commit];
    } else {
        // Diagnóstico explícito: malha demonstrativa desacoplada ou indisponível
        _report[@"demo_pipeline_status"] = @"Malha/pipeline demonstrativo indisponivel ou desacoplado da apresentacao guest.";
        [encoder endEncoding];
        [command presentDrawable:drawable];
        [command commit];
    }
}

- (void)dealloc {
    _continuousExecution = NO;
    if (_guestExecutionQueue) {
        dispatch_sync(_guestExecutionQueue, ^{});
    }
    if (_audioQueue) {
        AudioQueueStop(_audioQueue, true);
        AudioQueueDispose(_audioQueue, true);
        _audioQueue = nullptr;
    }
    if (_bootCoordinator && _bootCoordinator->runtime()) {
        _bootCoordinator->runtime()->set_gnm_listener(nullptr);
    }
    _gnmBridgeListener.reset();
    _videoOutManager.detach_guest_memory();
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void)view; (void)size;
}

- (BOOL)handleGnmDrawCalls:(const std::vector<bb::DrawCall>&)draws {
    if (draws.empty()) return YES;
    if (!_bootCoordinator || !_bootCoordinator->guest_memory()) return NO;
    bb::GnmMetalBridge bridge(*_bootCoordinator->guest_memory());
    bridge.set_allow_fallback_shaders(true);
    for (const auto& draw : draws) {
        auto res = bridge.process_draw_call(draw);
        if (!res.ok()) {
            _guestRasterizerFailures.fetch_add(1);
            return NO;
        }
    }
    return YES;
}

- (BOOL)handleGnmDispatchCalls:(const std::vector<bb::DispatchCall>&)dispatches {
    if (dispatches.empty()) return YES;
    if (!_bootCoordinator || !_bootCoordinator->guest_memory()) return NO;
    bb::GnmMetalBridge bridge(*_bootCoordinator->guest_memory());
    for (const auto& disp : dispatches) {
        auto res = bridge.process_dispatch(disp);
        if (!res.ok()) {
            _guestRasterizerFailures.fetch_add(1);
            return NO;
        }
    }
    return YES;
}

#pragma mark - Touch & Controller Handlers

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGSize sz = self.view.bounds.size;
    if (sz.width > 0 && sz.height > 0) {
        bb::ScreenLayoutConfig cfg;
        cfg.width = sz.width;
        cfg.height = sz.height;
        UIEdgeInsets insets = self.view.safeAreaInsets;
        cfg.safe_left = insets.left > 0 ? insets.left : bb::IPHONE_15_PRO_MAX_SAFE_LEFT_PT;
        cfg.safe_right = insets.right > 0 ? insets.right : bb::IPHONE_15_PRO_MAX_SAFE_RIGHT_PT;
        cfg.safe_top = insets.top;
        cfg.safe_bottom = insets.bottom > 0 ? insets.bottom : bb::IPHONE_15_PRO_MAX_SAFE_BOTTOM_PT;
        _touchOverlay.set_screen_layout(cfg);
    }
}

- (void)handleTouchId:(uint64_t)tid x:(float)x y:(float)y phase:(bb::TouchPhase)phase {
    _touchOverlay.on_touch(tid, x, y, phase);
}

- (void)onVirtualButtonTouch:(UIButton *)sender {
    NSString *t = sender.currentTitle;
    sender.backgroundColor = [UIColor colorWithRed:0.85 green:0.25 blue:0.25 alpha:0.85];

    if ([t containsString:@"✖"]) _inputManager.set_button(bb::PAD_CROSS, true, bb::InputSource::Touch);
    else if ([t containsString:@"●"]) _inputManager.set_button(bb::PAD_CIRCLE, true, bb::InputSource::Touch);
    else if ([t containsString:@"■"]) _inputManager.set_button(bb::PAD_SQUARE, true, bb::InputSource::Touch);
    else if ([sender.accessibilityIdentifier isEqualToString:@"triangle"]) _inputManager.set_button(bb::PAD_TRIANGLE, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"▲"]) _inputManager.set_button(bb::PAD_UP, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"▼"]) _inputManager.set_button(bb::PAD_DOWN, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"◀"]) _inputManager.set_button(bb::PAD_LEFT, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"▶"]) _inputManager.set_button(bb::PAD_RIGHT, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"R1"]) _inputManager.set_button(bb::PAD_R1, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"R2"]) _inputManager.set_button(bb::PAD_R2, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"L1"]) _inputManager.set_button(bb::PAD_L1, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"L2"]) _inputManager.set_button(bb::PAD_L2, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"R3"]) _inputManager.set_button(bb::PAD_R3, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"L3"]) _inputManager.set_button(bb::PAD_L3, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"OPT"]) _inputManager.set_button(bb::PAD_OPTIONS, true, bb::InputSource::Touch);
    else if ([t isEqualToString:@"TOUCH"]) _inputManager.set_button(bb::PAD_TOUCH_PAD, true, bb::InputSource::Touch);

    if (@available(iOS 10.0, *)) {
        UIImpactFeedbackGenerator *feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
        [feedback impactOccurred];
    }
    AudioServicesPlaySystemSound(1104);
}

- (void)onVirtualButtonRelease:(UIButton *)sender {
    NSString *t = sender.currentTitle;
    sender.backgroundColor = [UIColor colorWithWhite:0.12 alpha:0.65];

    if ([t containsString:@"✖"]) _inputManager.set_button(bb::PAD_CROSS, false, bb::InputSource::Touch);
    else if ([t containsString:@"●"]) _inputManager.set_button(bb::PAD_CIRCLE, false, bb::InputSource::Touch);
    else if ([t containsString:@"■"]) _inputManager.set_button(bb::PAD_SQUARE, false, bb::InputSource::Touch);
    else if ([sender.accessibilityIdentifier isEqualToString:@"triangle"]) _inputManager.set_button(bb::PAD_TRIANGLE, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"▲"]) _inputManager.set_button(bb::PAD_UP, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"▼"]) _inputManager.set_button(bb::PAD_DOWN, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"◀"]) _inputManager.set_button(bb::PAD_LEFT, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"▶"]) _inputManager.set_button(bb::PAD_RIGHT, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"R1"]) _inputManager.set_button(bb::PAD_R1, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"R2"]) _inputManager.set_button(bb::PAD_R2, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"L1"]) _inputManager.set_button(bb::PAD_L1, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"L2"]) _inputManager.set_button(bb::PAD_L2, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"R3"]) _inputManager.set_button(bb::PAD_R3, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"L3"]) _inputManager.set_button(bb::PAD_L3, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"OPT"]) _inputManager.set_button(bb::PAD_OPTIONS, false, bb::InputSource::Touch);
    else if ([t isEqualToString:@"TOUCH"]) _inputManager.set_button(bb::PAD_TOUCH_PAD, false, bb::InputSource::Touch);
}

#pragma mark - Physical GameController Support

- (void)setupGameControllers {
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onControllerConnected:)
        name:GCControllerDidConnectNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onControllerDisconnected:)
        name:GCControllerDidDisconnectNotification object:nil];
    if (GCController.controllers.count > 0) {
        [self onControllerConnected:nil];
    }
}

#pragma mark - iOS Lifecycle and Thermal Monitoring

- (void)setupLifecycleAndThermalMonitoring {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(onAppDidEnterBackground:)
        name:UIApplicationDidEnterBackgroundNotification object:nil];
    [nc addObserver:self selector:@selector(onAppWillEnterForeground:)
        name:UIApplicationWillEnterForegroundNotification object:nil];
    [nc addObserver:self selector:@selector(onAppWillTerminate:)
        name:UIApplicationWillTerminateNotification object:nil];
    if (@available(iOS 13.0, *)) {
        [nc addObserver:self selector:@selector(onAppDidEnterBackground:)
            name:UISceneDidEnterBackgroundNotification object:nil];
        [nc addObserver:self selector:@selector(onAppWillEnterForeground:)
            name:UISceneWillEnterForegroundNotification object:nil];
        [nc addObserver:self selector:@selector(onAppWillTerminate:)
            name:UISceneDidDisconnectNotification object:nil];
    }
    [nc addObserver:self selector:@selector(onAppDidReceiveMemoryWarning:)
        name:UIApplicationDidReceiveMemoryWarningNotification object:nil];
    [nc addObserver:self selector:@selector(onThermalStateChanged:)
        name:NSProcessInfoThermalStateDidChangeNotification object:nil];
}

- (void)onAppWillTerminate:(NSNotification *)note {
    (void)note;
    _continuousExecution = NO;
    if (_bootCoordinator) {
        _bootCoordinator->pause();
        auto& sm = _bootCoordinator->save_manager();
        if (sm.is_initialized()) {
            for (std::size_t i = 0; i < bb::SaveDataManager::MAX_SLOTS; ++i) {
                const auto *slot = sm.get_slot(static_cast<int>(i));
                if (slot && slot->used && !slot->read_only) {
                    sm.umount(slot->mount_point, true);
                }
            }
        }
    }
    bb::AudioOutManager::instance().set_paused(true);
    if (_audioQueue && _audioActive) {
        AudioQueueStop(_audioQueue, true);
        AudioQueueDispose(_audioQueue, true);
        _audioQueue = NULL;
        _audioActive = NO;
    }
    _report[@"lifecycle_state"] = @"app_terminated";
}

- (void)onAppDidEnterBackground:(NSNotification *)note {
    (void)note;
    _pauseReasons |= PauseReasonLifecycle;
    if (_bootCoordinator) {
        _bootCoordinator->add_pause_reason(bb::PauseReason::lifecycle);
    }
    bb::AudioOutManager::instance().set_paused(true);
    bb::AudioOutManager::instance().purge_inactive_buffers();
    if (_audioQueue && _audioActive) {
        AudioQueuePause(_audioQueue);
        AudioQueueReset(_audioQueue);
    }
    _metal.paused = YES;
    _report[@"lifecycle_state"] = @"background_paused";
}

- (void)onAppWillEnterForeground:(NSNotification *)note {
    (void)note;
    _pauseReasons &= ~PauseReasonLifecycle;
    if (_bootCoordinator) {
        _bootCoordinator->clear_pause_reason(bb::PauseReason::lifecycle);
    }
    NSError *error = nil;
    [[AVAudioSession sharedInstance] setActive:YES error:&error];
    _metal.paused = NO;
    _report[@"lifecycle_state"] = @"foreground_active";

    // Preserva pausa se houver motivo manual (PauseReasonUser) ou desconexão (PauseReasonDisconnect)
    if (_pauseReasons == PauseReasonNone) {
        bb::AudioOutManager::instance().set_paused(false);
        if (_audioQueue && _audioActive) {
            AudioQueueStart(_audioQueue, NULL);
        }
        if (_continuousExecution) {
            [self scheduleGuestCpuStep];
        }
    }
}

- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
    _memoryMonitor.trigger_low_memory_warning();
    bb::gnm_bridge_clear_caches();
    _report[@"memory_warning_triggered"] = @YES;
}

- (void)onAppDidReceiveMemoryWarning:(NSNotification *)note {
    (void)note;
    _memoryMonitor.trigger_low_memory_warning();
    bb::gnm_bridge_clear_caches();
    _report[@"memory_warning_triggered"] = @YES;
}

- (void)onThermalStateChanged:(NSNotification *)note {
    (void)note;
    NSProcessInfoThermalState state = [NSProcessInfo processInfo].thermalState;
    bb::ThermalThrottlingLevel level = bb::ThermalThrottlingLevel::Nominal;
    if (state == NSProcessInfoThermalStateCritical) {
        level = bb::ThermalThrottlingLevel::Critical;
        _topStatusLabel.text = @"🔥 Alerta Térmico Crítico: Redução ativa para 20 FPS";
        _report[@"thermal_state"] = @"critical_20fps";
    } else if (state == NSProcessInfoThermalStateSerious) {
        level = bb::ThermalThrottlingLevel::Serious;
        _topStatusLabel.text = @"🌡 Atenção Térmica: Taxa ajustada para 24 FPS";
        _report[@"thermal_state"] = @"serious_24fps";
    } else if (state == NSProcessInfoThermalStateFair) {
        level = bb::ThermalThrottlingLevel::Fair;
        _topStatusLabel.text = @"⚙ Taxa Estável: 30 FPS (Aquece)";
        _report[@"thermal_state"] = @"fair_30fps";
    } else {
        level = bb::ThermalThrottlingLevel::Nominal;
        _topStatusLabel.text = @"⚙ Taxa Estável: 30 FPS";
        _report[@"thermal_state"] = @"nominal_30fps";
    }
    auto cfg = bb::get_thermal_throttling_config(level);
    _pacingTracker.set_target_fps(cfg.target_fps);
    _metal.preferredFramesPerSecond = static_cast<NSInteger>(cfg.target_fps);
    _guestCpuSliceBudget.store(cfg.cpu_step_budget);
    _guestCpuRescheduleDelayMs.store(cfg.reschedule_delay_ms);
}

- (void)setupController:(GCController *)controller {
    if (!controller) return;
    GCExtendedGamepad *gamepad = controller.extendedGamepad;
    if (!gamepad) return;

    __weak LabController *weakSelf = self;
    gamepad.valueChangedHandler = ^(GCExtendedGamepad *pad, GCControllerElement *element) {
        (void)element;
        LabController *strongSelf = weakSelf;
        if (!strongSelf) return;

        strongSelf->_inputManager.set_left_stick(pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_right_stick(pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value, bb::InputSource::Gamepad);

        strongSelf->_inputManager.set_button(bb::PAD_CROSS, pad.buttonA.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_CIRCLE, pad.buttonB.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_SQUARE, pad.buttonX.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_TRIANGLE, pad.buttonY.isPressed, bb::InputSource::Gamepad);

        strongSelf->_inputManager.set_button(bb::PAD_L1, pad.leftShoulder.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_R1, pad.rightShoulder.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_L2, pad.leftTrigger.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_R2, pad.rightTrigger.isPressed, bb::InputSource::Gamepad);

        strongSelf->_inputManager.set_button(bb::PAD_UP, pad.dpad.up.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_DOWN, pad.dpad.down.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_LEFT, pad.dpad.left.isPressed, bb::InputSource::Gamepad);
        strongSelf->_inputManager.set_button(bb::PAD_RIGHT, pad.dpad.right.isPressed, bb::InputSource::Gamepad);

        if (@available(iOS 12.1, *)) {
            if (pad.leftThumbstickButton) {
                strongSelf->_inputManager.set_button(bb::PAD_L3, pad.leftThumbstickButton.isPressed, bb::InputSource::Gamepad);
            }
            if (pad.rightThumbstickButton) {
                strongSelf->_inputManager.set_button(bb::PAD_R3, pad.rightThumbstickButton.isPressed, bb::InputSource::Gamepad);
            }
        }
        if (pad.buttonMenu) {
            strongSelf->_inputManager.set_button(bb::PAD_OPTIONS, pad.buttonMenu.isPressed, bb::InputSource::Gamepad);
        }
        if (@available(iOS 13.0, *)) {
            bool touchpadPressed = false;
            if (pad.buttonOptions && pad.buttonOptions.isPressed) {
                touchpadPressed = true;
            }
            if (@available(iOS 14.0, *)) {
                GCControllerButtonInput *tp = controller.physicalInputProfile.buttons[GCInputButtonTouchpad];
                if (tp && tp.isPressed) {
                    touchpadPressed = true;
                }
            }
            strongSelf->_inputManager.set_button(bb::PAD_TOUCH_PAD, touchpadPressed, bb::InputSource::Gamepad);
        }
    };
}

- (void)onControllerConnected:(NSNotification *)note {
    GCController *controller = note ? (GCController *)note.object : GCController.controllers.firstObject;
    if (!controller) return;

    uint64_t cid = (uint64_t)(uintptr_t)controller;
    bool becameActive = _inputManager.register_controller(cid);

    if (becameActive || _activeController == nil) {
        _activeController = controller;
        [self setupController:controller];
        _report[@"gamepad_connected"] = @YES;
        _touchOverlay.set_gamepad_connected(true);
        _pauseReasons &= ~PauseReasonDisconnect;
        if (_bootCoordinator) {
            _bootCoordinator->clear_pause_reason(bb::PauseReason::disconnect);
        }

        // Auto-ocultação e atenuação gradual dos botões na tela quando o controle físico estiver ativo (T022)
        [UIView animateWithDuration:0.35 animations:^{
            self->_controlsOverlay.alpha = 0.0;
        } completion:^(BOOL finished) {
            if (finished && self->_touchOverlay.is_gamepad_connected()) {
                self->_controlsOverlay.hidden = YES;
            }
        }];

        _subsystemStatus.text = [NSString stringWithFormat:@"GameController conectado: %@ (Mapeamento ativo)",
            controller.vendorName ? controller.vendorName : @"Compatível"];

        if (_continuousExecution && _pauseReasons == PauseReasonNone) {
            _topStatusLabel.text = @"BB Lab | demonstração | sem gameplay validado";
            [self scheduleGuestCpuStep];
        }
    } else {
        // T113: Segundo controle conectado sem perturbar o controle ativo
        _subsystemStatus.text = [NSString stringWithFormat:@"GameController secundário detectado: %@ (Principal: %@)",
            controller.vendorName ? controller.vendorName : @"Compatível",
            _activeController.vendorName ? _activeController.vendorName : @"Ativo"];
    }
    _report[@"connected_controllers_count"] = @(_inputManager.connected_controllers_count());
}

- (void)onControllerDisconnected:(NSNotification *)note {
    GCController *controller = (GCController *)note.object;
    uint64_t cid = controller ? (uint64_t)(uintptr_t)controller : (_activeController ? (uint64_t)(uintptr_t)_activeController : 0);
    if (cid == 0) return;

    bool activeChanged = _inputManager.unregister_controller(cid);
    _report[@"connected_controllers_count"] = @(_inputManager.connected_controllers_count());

    if (!activeChanged) {
        // T113: Controle desconectado não era o ativo. Não limpa entrada nem esconde HUD físico.
        _subsystemStatus.text = [NSString stringWithFormat:@"Controle secundário desconectado. Principal (%@) permanece ativo.",
            _activeController.vendorName ? _activeController.vendorName : @"Ativo"];
        return;
    }

    // Se o controle ativo mudou, verificar se há outro disponível
    uint64_t newActiveId = _inputManager.active_controller_id();
    if (newActiveId != 0) {
        GCController *fallback = nil;
        for (GCController *c in GCController.controllers) {
            if ((uint64_t)(uintptr_t)c == newActiveId) {
                fallback = c;
                break;
            }
        }
        if (fallback) {
            _activeController = fallback;
            [self setupController:fallback];
            _subsystemStatus.text = [NSString stringWithFormat:@"Controle desconectado. Alternado automaticamente para: %@",
                fallback.vendorName ? fallback.vendorName : @"Compatível"];
            return;
        }
    }

    // Nenhum controle restante: desconexão completa (T113 / FR-007)
    _activeController = nil;
    _report[@"gamepad_connected"] = @NO;
    _touchOverlay.set_gamepad_connected(false);
    _controlsOverlay.hidden = NO;
    [UIView animateWithDuration:0.35 animations:^{
        self->_controlsOverlay.alpha = 1.0;
    }];

    _pauseReasons |= PauseReasonDisconnect;
    if (_bootCoordinator) {
        _bootCoordinator->add_pause_reason(bb::PauseReason::disconnect);
        _topStatusLabel.text = @"⏸ Pausado: GameController desconectado";
    }
    _subsystemStatus.text = @"GameController desconectado. Controles virtuais na tela reativados.";
}

#pragma mark - CoreAudio Subsystem

- (void)setupAudio {
    NSError *error = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:&error];
    [session setActive:YES error:&error];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(onAudioSessionInterruption:)
                                                 name:AVAudioSessionInterruptionNotification
                                               object:session];

    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 4;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 16;

    OSStatus status = AudioQueueNewOutput(&format, BB_AudioQueueCallback, (__bridge void *)self, NULL, NULL, 0, &_audioQueue);
    if (status == noErr) {
        const UInt32 bufferSize = 2048 * 4;
        for (int i = 0; i < 3; ++i) {
            AudioQueueAllocateBuffer(_audioQueue, bufferSize, &_audioBuffers[i]);
            _audioBuffers[i]->mAudioDataByteSize = bufferSize;
            std::memset(_audioBuffers[i]->mAudioData, 0, bufferSize);
            AudioQueueEnqueueBuffer(_audioQueue, _audioBuffers[i], 0, NULL);
        }
        AudioQueueStart(_audioQueue, NULL);
        _audioActive = YES;
    }
}

- (void)onAudioSessionInterruption:(NSNotification *)note {
    NSDictionary *userInfo = note.userInfo;
    NSNumber *typeVal = userInfo[AVAudioSessionInterruptionTypeKey];
    if (!typeVal) return;
    AVAudioSessionInterruptionType type = (AVAudioSessionInterruptionType)[typeVal unsignedIntegerValue];

    if (type == AVAudioSessionInterruptionTypeBegan) {
        // T050: Interrupção da sessão de áudio (ligação, alarme, etc.)
        bb::AudioOutManager::instance().set_paused(true);
        bb::AudioOutManager::instance().purge_inactive_buffers();
        if (_audioQueue && _audioActive) {
            AudioQueuePause(_audioQueue);
            AudioQueueReset(_audioQueue);
        }
        _report[@"audio_interrupted"] = @YES;
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        // Retomada sem áudio preso ou estalos
        NSNumber *optionsVal = userInfo[AVAudioSessionInterruptionOptionKey];
        NSUInteger options = [optionsVal unsignedIntegerValue];
        NSError *error = nil;
        [[AVAudioSession sharedInstance] setActive:YES error:&error];
        if (options & AVAudioSessionInterruptionOptionShouldResume) {
            bb::AudioOutManager::instance().set_paused(false);
            if (_audioQueue && _audioActive) {
                AudioQueueStart(_audioQueue, NULL);
            }
        }
        _report[@"audio_interrupted"] = @NO;
    }
}

- (void)fillAudioBuffer:(AudioQueueBufferRef)buffer {
    std::memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    // O gerenciador global recebe as chamadas HLE do guest. Manter a duração
    // do buffer constante e deixar o restante zerado evita acelerar a fila.
    const size_t frames = buffer->mAudioDataBytesCapacity / 4;
    bb::AudioOutManager::instance().drain_main_port_pcm16(
        static_cast<int16_t*>(buffer->mAudioData), frames);
}

#pragma mark - Diagnostics & Documents Inventory

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
            if (bytes) {
                NSString *content = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
                if (content && bytes.length > 0) {
                    result[@"status"] = @"ok";
                    result[@"valid"] = @YES;
                    // T070 / Constituição III: minimizar dados exportáveis; não incluir conteúdo integral do arquivo
                    result[@"size_bytes"] = @(bytes.length);
                }
            }
        }
    }
    result[@"checked_at"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    _report[@"documents_test"] = result;
    _documentStatus.text = [NSString stringWithFormat:@"teste-bblab.txt: %@", result[@"status"]];
}

- (void)checkGameAsset {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    const auto probe = bb::probe_sfo_in_documents(documents.path ? documents.path.UTF8String : "");
    NSString *status = [NSString stringWithUTF8String:probe.status.c_str()];
    NSMutableDictionary *result = [@{@"path": @"/app0/param.sfo", @"status": status,
        @"valid": @(probe.valid), @"size_bytes": @(probe.size_bytes),
        @"crc32": @(probe.crc32)} mutableCopy];
    if (probe.valid) {
        result[@"title_id"] = [NSString stringWithUTF8String:probe.title_id.c_str()];
        result[@"title"] = [NSString stringWithUTF8String:probe.title.c_str()];
        result[@"app_ver"] = [NSString stringWithUTF8String:probe.app_ver.c_str()];
    }
    result[@"checked_at"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    _report[@"sfo_probe"] = result;
    _assetStatus.text = [NSString stringWithFormat:@"SFO: %@ (%@)", status, probe.valid ? result[@"title_id"] : @"sem metadados"];
}

- (void)checkDcx {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    const auto probe = bb::probe_dcx_in_documents(documents.path ? documents.path.UTF8String : "");
    NSString *status = [NSString stringWithUTF8String:probe.status.c_str()];
    _report[@"dcx_probe"] = @{
        @"path": @"/app0/m29_00_00_00_408010.flver.dcx",
        @"status": status, @"valid": @(probe.valid),
        @"size_bytes": @(probe.size_bytes), @"decoded_bytes": @(probe.decoded_bytes),
        @"geometry_status": [NSString stringWithUTF8String:probe.geometry_status.c_str()],
        @"geometry_vertices": @(probe.vertex_count), @"geometry_triangles": @(probe.triangle_count),
        @"checked_at": [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]]
    };
    _dcxStatus.text = [NSString stringWithFormat:@"DCX: %@ (%llu -> %llu bytes)", status, probe.size_bytes, probe.decoded_bytes];
}

- (void)checkBootEntry {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    const auto probe = bb::probe_eboot_entry(documents.path ? documents.path.UTF8String : "");
    NSString *status = [NSString stringWithUTF8String:probe.status.c_str()];
    _report[@"boot_probe"] = @{
        @"path": @"Documents/eboot.elf",
        @"status": status, @"entry_reached": @(probe.entry_reached),
        @"file_size": @(probe.file_size), @"entry_rip": @(probe.entry_rip),
        @"stop_rip": @(probe.stop_rip), @"stack_pointer": @(probe.stack_pointer),
        @"instructions_executed": @(probe.instructions_executed),
        @"game_execution": @NO,
        @"checked_at": [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]]
    };
    _bootStatus.text = [NSString stringWithFormat:@"Entry probe: %@, parada em 0x%llx", status, probe.stop_rip];
}

- (void)checkDocumentsInventory {
    NSURL *documents = [[[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask] firstObject];
    NSFileManager *manager = [NSFileManager defaultManager];
    NSMutableDictionary *inventory = [NSMutableDictionary dictionary];
    inventory[@"directory_available"] = @(documents != nil);
    bb::VirtualFileSystem vfs;
    const bool mounted = documents && vfs.mount("/app0", documents.path.UTF8String, true);
    inventory[@"vfs_mounted"] = @(mounted);

    NSString *gameDir = documents ? [self findGameDirectoryIn:documents.path] : nil;
    inventory[@"game_directory_found"] = @(gameDir != nil);

    BOOL hasDvdroot = NO;
    NSUInteger pkgCount = 0;
    if (gameDir) {
        NSString *dvdrootPath = [gameDir stringByAppendingPathComponent:@"dvdroot_ps4"];
        BOOL isDir = NO;
        if ([manager fileExistsAtPath:dvdrootPath isDirectory:&isDir] && isDir) {
            hasDvdroot = YES;
            vfs.mount("/dvdroot", dvdrootPath.UTF8String, true);
            vfs.mount("/app0/dvdroot_ps4", dvdrootPath.UTF8String, true);
            NSArray *items = [manager contentsOfDirectoryAtPath:dvdrootPath error:nil];
            for (NSString *f in items) {
                if ([f.pathExtension.lowercaseString isEqualToString:@"bhd"] || [f.pathExtension.lowercaseString isEqualToString:@"bhd5"]) {
                    pkgCount++;
                }
            }
        }
    }
    inventory[@"dvdroot_detected"] = @(hasDvdroot);
    inventory[@"packages_detected"] = @(pkgCount);
    inventory[@"path_resolution_verified"] = @(mounted);

    NSMutableDictionary *files = [NSMutableDictionary dictionary];
    for (NSString *name in @[@"teste-bblab.txt", @"param.sfo", @"m29_00_00_00_408010.flver.dcx", @"eboot.elf", @"eboot.bin", @"libc.prx", @"libSceFios2.prx"]) {
        NSError *error = nil;
        NSDictionary *attributes = documents ? [manager attributesOfItemAtPath:
            [documents.path stringByAppendingPathComponent:name] error:&error] : nil;
        NSMutableDictionary *item = [NSMutableDictionary dictionary];
        item[@"foundation_status"] = attributes ? @"regular_file" : @"not_found";
        if (attributes) item[@"size_bytes"] = @([attributes fileSize]);
        files[name] = item;
    }
    inventory[@"files"] = files;
    inventory[@"checked_at"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    _report[@"documents_inventory"] = inventory;
}

- (void)exportReport:(UIButton *)sender {
    if (_bootCoordinator) {
        auto diag = _bootCoordinator->diagnostics();
        [self updateCoordinatorReportAndUI:diag executedSteps:0];
    }
    [self checkDocumentsInventory];
    [self checkDocument];
    [self checkGameAsset];
    [self checkDcx];
    [self checkBootEntry];
    _report[@"guest_steps_executed"] = @(_lastGuestSteps > 0);
    _report[@"game_execution"] = @NO;
    _report[@"timestamp"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    auto pacing = _pacingTracker.get_stats();
    _report[@"frame_pacing_p50_ms"] = @(pacing.p50_ms);
    _report[@"average_fps"] = @(pacing.average_fps);
    uint64_t mem_bytes = _memoryMonitor.query_physical_footprint_bytes();
    _report[@"memory_footprint_mb"] = @(static_cast<double>(mem_bytes) / (1024.0 * 1024.0));
    _report[@"gamepad_connected"] = @(GCController.controllers.count > 0);
    _report[@"video_flips"] = @(_videoOutManager.flip_count());
    _report[@"gnm_submit_count"] = @(_guestSubmits.load());
    _report[@"gnm_cpu_triangles"] = @(_guestTriangles.load());
    _report[@"gnm_cpu_rasterizer_failures"] = @(_guestRasterizerFailures.load());
    _report[@"gnm_cpu_last_rasterizer_status_code"] = @(_guestLastRasterizerStatus.load());

    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:_report options:NSJSONWritingPrettyPrinted error:&error];
    NSURL *directory = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
        inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:&error];
    NSURL *file = [directory URLByAppendingPathComponent:@"bb-lab-report.json"];
    if (json && file && [json writeToURL:file options:NSDataWritingAtomic error:&error]) {
        UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[file] applicationActivities:nil];
        share.popoverPresentationController.sourceView = sender;
        share.popoverPresentationController.sourceRect = sender.bounds;
        [self presentViewController:share animated:YES completion:nil];
    }
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
