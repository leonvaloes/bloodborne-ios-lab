#import "MetalViewController.h"
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#import <sys/utsname.h>
#include "host_bridge.h"

@interface MetalViewController ()

@property (nonatomic, strong) MTKView *metalView;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLTexture> screenTexture;
@property (nonatomic, strong) UIView *overlayView;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *metricsLabel;
@property (nonatomic, strong) UIButton *bootButton;

@end

@implementation MetalViewController {
    std::unique_ptr<bb::HostBridge> _bridge;
    id<MTLRenderPipelineState> _screenPipeline;
    id<MTLSamplerState> _screenSampler;
    BOOL _autoStepping;
    NSUInteger _frameCount;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.view.backgroundColor = [UIColor blackColor];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        UILabel *errorLabel = [[UILabel alloc] initWithFrame:self.view.bounds];
        errorLabel.text = @"Protótipo de diagnóstico, jogo não executado. Metal indisponível.";
        errorLabel.numberOfLines = 0;
        errorLabel.textColor = [UIColor redColor];
        errorLabel.textAlignment = NSTextAlignmentCenter;
        [self.view addSubview:errorLabel];
        return;
    }

    self.commandQueue = [device newCommandQueue];
    if (!self.commandQueue) {
        UILabel *errorLabel = [[UILabel alloc] initWithFrame:self.view.bounds];
        errorLabel.text = @"Protótipo de diagnóstico, jogo não executado. Fila Metal indisponível.";
        errorLabel.numberOfLines = 0;
        errorLabel.textColor = [UIColor redColor];
        errorLabel.textAlignment = NSTextAlignmentCenter;
        [self.view addSubview:errorLabel];
        return;
    }

    self.metalView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.metalView.delegate = self;
    self.metalView.paused = YES;
    self.metalView.enableSetNeedsDisplay = YES;
    self.metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.metalView.clearColor = MTLClearColorMake(0.04, 0.04, 0.06, 1.0);
    [self.view addSubview:self.metalView];

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

    NSError *pipeErr = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:shaderSource options:nil error:&pipeErr];
    if (lib) {
        MTLRenderPipelineDescriptor *pDesc = [MTLRenderPipelineDescriptor new];
        pDesc.vertexFunction = [lib newFunctionWithName:@"screenVertex"];
        pDesc.fragmentFunction = [lib newFunctionWithName:@"screenFragment"];
        pDesc.colorAttachments[0].pixelFormat = self.metalView.colorPixelFormat;
        _screenPipeline = [device newRenderPipelineStateWithDescriptor:pDesc error:nil];
    }
    MTLSamplerDescriptor *sDesc = [MTLSamplerDescriptor new];
    sDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _screenSampler = [device newSamplerStateWithDescriptor:sDesc];

    [self setupWelcomeAndDiagnosticUI:device];
}

- (void)setupWelcomeAndDiagnosticUI:(id<MTLDevice>)device {
    self.overlayView = [[UIView alloc] initWithFrame:self.view.bounds];
    self.overlayView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.overlayView.backgroundColor = [UIColor colorWithRed:0.03f green:0.04f blue:0.07f alpha:0.88f];
    self.overlayView.userInteractionEnabled = YES;

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10.0f;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.overlayView addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.overlayView.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.overlayView.centerYAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.overlayView.leadingAnchor constant:24.0f],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:self.overlayView.trailingAnchor constant:-24.0f]
    ]];

    UILabel *titleLabel = [[UILabel alloc] init];
    titleLabel.text = @"Bloodborne iOS Lab";
    titleLabel.font = [UIFont boldSystemFontOfSize:24.0f];
    titleLabel.textColor = [UIColor colorWithRed:0.95f green:0.85f blue:0.65f alpha:1.0f];
    titleLabel.textAlignment = NSTextAlignmentCenter;
    [stack addArrangedSubview:titleLabel];

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.text = @"Protótipo de diagnóstico, jogo não executado.";
    self.statusLabel.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightMedium];
    self.statusLabel.textColor = [UIColor colorWithRed:1.0f green:0.82f blue:0.35f alpha:1.0f];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.numberOfLines = 0;
    [stack addArrangedSubview:self.statusLabel];

    self.metricsLabel = [[UILabel alloc] init];
    self.metricsLabel.font = [UIFont monospacedSystemFontOfSize:12.0f weight:UIFontWeightRegular];
    self.metricsLabel.textColor = [UIColor colorWithWhite:0.82f alpha:1.0f];
    self.metricsLabel.textAlignment = NSTextAlignmentCenter;
    self.metricsLabel.numberOfLines = 0;
    [stack addArrangedSubview:self.metricsLabel];

    UILabel *instructions = [[UILabel alloc] init];
    instructions.text = @"Execução local no iPhone 15 Pro Max: multitoque virtual e suporte GameController ativo.";
    instructions.font = [UIFont systemFontOfSize:12.0f];
    instructions.textColor = [UIColor colorWithWhite:0.65f alpha:1.0f];
    instructions.textAlignment = NSTextAlignmentCenter;
    instructions.numberOfLines = 0;
    [stack addArrangedSubview:instructions];

    self.bootButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.bootButton setTitle:@"▶ Iniciar Motor Bloodborne (HostBridge)" forState:UIControlStateNormal];
    self.bootButton.titleLabel.font = [UIFont boldSystemFontOfSize:15.0f];
    self.bootButton.backgroundColor = [UIColor colorWithRed:0.1f green:0.5f blue:0.9f alpha:0.25f];
    self.bootButton.layer.cornerRadius = 8.0f;
    self.bootButton.contentEdgeInsets = UIEdgeInsetsMake(8, 16, 8, 16);
    [self.bootButton addTarget:self action:@selector(onBootTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:self.bootButton];

    [self.view addSubview:self.overlayView];
    [self updateDiagnosticMetricsWithDevice:device];
    [self setupGameControllers];
}

- (void)setupGameControllers {
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onControllerConnected:)
                                                 name:GCControllerDidConnectNotification object:nil];
    if (GCController.controllers.count > 0) {
        [self setupController:GCController.controllers.firstObject];
    }
}

- (void)onControllerConnected:(NSNotification *)note {
    (void)note;
    if (GCController.controllers.count > 0) {
        [self setupController:GCController.controllers.firstObject];
    }
}

- (void)setupController:(GCController *)controller {
    if (!controller || !controller.extendedGamepad) return;
    GCExtendedGamepad *pad = controller.extendedGamepad;
    __weak MetalViewController *weakSelf = self;
    pad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
        (void)element;
        MetalViewController *strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_bridge) return;
        auto& input = strongSelf->_bridge->input();
        input.set_left_stick(gamepad.leftThumbstick.xAxis.value, gamepad.leftThumbstick.yAxis.value);
        input.set_right_stick(gamepad.rightThumbstick.xAxis.value, gamepad.rightThumbstick.yAxis.value);
        input.set_button(bb::PAD_CROSS, gamepad.buttonA.isPressed);
        input.set_button(bb::PAD_CIRCLE, gamepad.buttonB.isPressed);
        input.set_button(bb::PAD_SQUARE, gamepad.buttonX.isPressed);
        input.set_button(bb::PAD_TRIANGLE, gamepad.buttonY.isPressed);
        input.set_button(bb::PAD_L1, gamepad.leftShoulder.isPressed);
        input.set_button(bb::PAD_R1, gamepad.rightShoulder.isPressed);
        input.set_button(bb::PAD_L2, gamepad.leftTrigger.isPressed);
        input.set_button(bb::PAD_R2, gamepad.rightTrigger.isPressed);
        input.set_button(bb::PAD_UP, gamepad.dpad.up.isPressed);
        input.set_button(bb::PAD_DOWN, gamepad.dpad.down.isPressed);
        input.set_button(bb::PAD_LEFT, gamepad.dpad.left.isPressed);
        input.set_button(bb::PAD_RIGHT, gamepad.dpad.right.isPressed);
    };
}

- (void)onBootTapped {
    if (_bridge && _bridge->is_initialized()) {
        _autoStepping = !_autoStepping;
        self.metalView.paused = !_autoStepping;
        self.metalView.enableSetNeedsDisplay = !_autoStepping;
        [self.bootButton setTitle:_autoStepping ? @"⏸ Pausar Execução" : @"▶ Continuar Execução" forState:UIControlStateNormal];
        return;
    }

    NSString *docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    bb::HostBridgeConfig config;
    config.documents_dir = docs ? docs.UTF8String : "";
    config.save_dir = docs ? [docs stringByAppendingPathComponent:@"savedata"].UTF8String : "";
    config.guest_memory_size = 768u << 20;
    config.enable_hle = true;
    config.use_scheduler = true;

    _bridge = std::make_unique<bb::HostBridge>(config);
    bool ok = _bridge->init(config);
    if (ok) {
        self.statusLabel.text = @"Motor Bloodborne inicializado e em execução.";
        self.statusLabel.textColor = [UIColor colorWithRed:0.2f green:0.85f blue:0.4f alpha:1.0f];
        [self.bootButton setTitle:@"⏸ Pausar Execução" forState:UIControlStateNormal];
        _autoStepping = YES;
        self.metalView.paused = NO;
        self.metalView.enableSetNeedsDisplay = NO;
    } else {
        self.statusLabel.text = [NSString stringWithFormat:@"Falha na inicialização: %s", _bridge->last_error().c_str()];
        self.statusLabel.textColor = [UIColor colorWithRed:1.0f green:0.3f blue:0.3f alpha:1.0f];
    }
}

- (void)handleTouches:(NSSet<UITouch *> *)touches phase:(bb::TouchPhase)phase {
    if (!_bridge) return;
    for (UITouch *touch in touches) {
        CGPoint pt = [touch locationInView:self.view];
        CGSize sz = self.view.bounds.size;
        float nx = sz.width > 0 ? (float)(pt.x / sz.width) : 0.0f;
        float ny = sz.height > 0 ? (float)(pt.y / sz.height) : 0.0f;
        _bridge->on_touch(static_cast<uint64_t>((uintptr_t)touch), nx, ny, phase);
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self handleTouches:touches phase:bb::TouchPhase::Began];
    [super touchesBegan:touches withEvent:event];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self handleTouches:touches phase:bb::TouchPhase::Moved];
    [super touchesMoved:touches withEvent:event];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self handleTouches:touches phase:bb::TouchPhase::Ended];
    [super touchesEnded:touches withEvent:event];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self handleTouches:touches phase:bb::TouchPhase::Cancelled];
    [super touchesCancelled:touches withEvent:event];
}

- (void)updateDiagnosticMetricsWithDevice:(id<MTLDevice>)device {
    struct utsname sysInfo{};
    uname(&sysInfo);
    NSString *deviceModel = [NSString stringWithUTF8String:sysInfo.machine];

    BOOL hasMetal3 = NO;
    if (@available(iOS 16.0, *)) {
        hasMetal3 = [device supportsFamily:MTLGPUFamilyMetal3];
    }

    BOOL hasApple9 = NO;
    if (@available(iOS 17.0, *)) {
        hasApple9 = [device supportsFamily:MTLGPUFamilyApple9];
    }

    NSString *metrics = [NSString stringWithFormat:
        @"Dispositivo: %@ (iOS %@)\n"
        @"GPU Metal: %@\n"
        @"Metal 3: %@ | Família Apple 9: %@\n"
        @"Memória unificada: %@ | Limite de buffer: %llu MB",
        deviceModel ?: @"(indisponível)",
        UIDevice.currentDevice.systemVersion,
        device.name,
        hasMetal3 ? @"Sim" : @"Não",
        hasApple9 ? @"Sim" : @"Não",
        device.hasUnifiedMemory ? @"Sim" : @"Não",
        static_cast<unsigned long long>(device.maxBufferLength / (1024 * 1024))
    ];

    self.metricsLabel.text = metrics;
}

#pragma mark - MTKViewDelegate

- (void)drawInMTKView:(nonnull MTKView *)view {
    if (_bridge && _autoStepping && _bridge->is_running()) {
        _bridge->update(1.0f / 60.0f);
        bb::HostFrameBuffer hostFrame;
        if (_bridge->render_frame(hostFrame) && hostFrame.valid && !hostFrame.pixel_data.empty()) {
            if (!self.screenTexture || self.screenTexture.width != hostFrame.width || self.screenTexture.height != hostFrame.height) {
                MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                width:hostFrame.width
                                                                                               height:hostFrame.height
                                                                                            mipmapped:NO];
                desc.usage = MTLTextureUsageShaderRead;
                self.screenTexture = [view.device newTextureWithDescriptor:desc];
            }
            if (self.screenTexture) {
                NSUInteger pitch = hostFrame.pitch > 0 ? hostFrame.pitch : hostFrame.width * 4;
                [self.screenTexture replaceRegion:MTLRegionMake2D(0, 0, hostFrame.width, hostFrame.height)
                                      mipmapLevel:0
                                        withBytes:hostFrame.pixel_data.data()
                                      bytesPerRow:pitch];
            }
        }
    }

    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
    if (!commandBuffer) return;

    MTLRenderPassDescriptor *renderPass = view.currentRenderPassDescriptor;
    if (renderPass != nil) {
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        if (self.screenTexture && _screenPipeline && _screenSampler) {
            [encoder setRenderPipelineState:_screenPipeline];
            [encoder setFragmentTexture:self.screenTexture atIndex:0];
            [encoder setFragmentSamplerState:_screenSampler atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [encoder endEncoding];

        if (view.currentDrawable) {
            [commandBuffer presentDrawable:view.currentDrawable];
        }
    }
    [commandBuffer commit];

    _frameCount++;
    if (_bridge && (_frameCount % 30 == 0)) {
        auto diag = _bridge->diagnostics();
        self.statusLabel.text = [NSString stringWithFormat:@"Bloodborne: %s | Passos: %llu | Imports: %llu | Flips: %llu",
            diag.boot_state_name.c_str(),
            (unsigned long long)diag.total_steps,
            (unsigned long long)diag.total_import_calls,
            (unsigned long long)diag.video_flip_count];
    }
}

- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
}

#pragma mark - View Controller Properties

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (BOOL)shouldAutorotate {
    return YES;
}

- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return UIRectEdgeAll;
}

@end
