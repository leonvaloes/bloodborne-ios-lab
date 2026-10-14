#import "MetalViewController.h"
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <sys/utsname.h>

@interface MetalViewController ()

@property (nonatomic, strong) MTKView *metalView;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) UIView *overlayView;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *metricsLabel;

@end

@implementation MetalViewController

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
    self.metalView.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
    self.metalView.clearColor = MTLClearColorMake(0.04, 0.04, 0.06, 1.0);
    [self.view addSubview:self.metalView];

    // Diagnóstico sintético apenas: sem loader, CPU guest ou backend gráfico do jogo.
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
    instructions.text = @"A tela consulta capacidades Metal e limpa a superfície com uma cor fixa. Não há renderização nem contagem de quadros do jogo.";
    instructions.font = [UIFont systemFontOfSize:12.0f];
    instructions.textColor = [UIColor colorWithWhite:0.65f alpha:1.0f];
    instructions.textAlignment = NSTextAlignmentCenter;
    instructions.numberOfLines = 0;
    [stack addArrangedSubview:instructions];

    [self.view addSubview:self.overlayView];
    [self updateDiagnosticMetricsWithDevice:device];
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
    // Apenas clear Metal sintético, sem contabilizar frames como resultado do jogo.
    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
    if (!commandBuffer) return;

    MTLRenderPassDescriptor *renderPass = view.currentRenderPassDescriptor;
    if (renderPass != nil) {
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        [encoder endEncoding];

        if (view.currentDrawable) {
            [commandBuffer presentDrawable:view.currentDrawable];
        }
    }
    [commandBuffer commit];
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
