#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <TargetConditionals.h>
#include "smoke.h"
#include <exception>
#include <sys/utsname.h>
#include <simd/simd.h>

@interface LabController : UIViewController <MTKViewDelegate>
@end
@implementation LabController {
    UILabel *_status;
    UILabel *_gpuStatus;
    MTKView *_metal;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLBuffer> _uniformBuffer;
    NSMutableDictionary *_report;
    NSUInteger _completedFrames;
    float _angle;
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
        @"metal_completed_frames": @0} mutableCopy];
    UILabel *title = [UILabel new];
    title.text = @"BB Lab ? CPU e Metal";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    UILabel *scope = [UILabel new];
    scope.text = @"Laborat?rio Bloodborne iOS. Testes de CPU, Mem?ria e Renderiza??o Metal.";
    scope.numberOfLines = 0;
    _status = [UILabel new];
    _status.numberOfLines = 0;
    _status.text = @"CPU: aguardando teste";
    _gpuStatus = [UILabel new];
    _gpuStatus.numberOfLines = 0;
    _gpuStatus.text = @"GPU: inicializando Metal";
    UIButton *run = [UIButton buttonWithType:UIButtonTypeSystem];
    [run setTitle:@"Executar testes CPU, mem?ria e HLE" forState:UIControlStateNormal];
    [run addTarget:self action:@selector(runTests) forControlEvents:UIControlEventTouchUpInside];
    UIButton *exportButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [exportButton setTitle:@"Exportar diagn?stico" forState:UIControlStateNormal];
    [exportButton addTarget:self action:@selector(exportReport:) forControlEvents:UIControlEventTouchUpInside];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    _metal = [[MTKView alloc] initWithFrame:CGRectZero device:device];
    _metal.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _metal.clearColor = MTLClearColorMake(0.04, 0.05, 0.09, 1);
    _metal.preferredFramesPerSecond = 30;
    _metal.paused = YES;
    [_metal.heightAnchor constraintEqualToConstant:180].active = YES;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, scope, _status, run, _metal, _gpuStatus, exportButton]];
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
    if (!device) { [self gpuError:@"Metal indispon?vel"]; return; }
    _report[@"metal_device"] = device.name;
    _report[@"metal_supports_metal3"] = @([device supportsFamily:MTLGPUFamilyMetal3]);
    _report[@"metal_supports_apple9"] = @([device supportsFamily:MTLGPUFamilyApple9]);
    _report[@"metal_has_unified_memory"] = @(device.hasUnifiedMemory);
    _report[@"metal_max_buffer_length"] = @(device.maxBufferLength);

    _queue = [device newCommandQueue];
    _uniformBuffer = [device newBufferWithLength:sizeof(simd_float4x4) options:MTLResourceStorageModeShared];

    // Metal shader with vertex transform uniform buffer and vertex color interpolation
    NSString *shader = @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct VertexOut { float4 position [[position]]; float4 color; };\n"
        "struct Uniforms { float4x4 transform; };\n"
        "vertex VertexOut triangle_vs(uint i [[vertex_id]], constant Uniforms &u [[buffer(0)]]) {\n"
        "    const float2 pos[3] = {float2(0, 0.75), float2(-0.7, -0.7), float2(0.7, -0.7)};\n"
        "    const float4 col[3] = {float4(0.9, 0.2, 0.15, 1), float4(0.2, 0.8, 0.3, 1), float4(0.2, 0.4, 0.9, 1)};\n"
        "    VertexOut out;\n"
        "    out.position = u.transform * float4(pos[i], 0, 1);\n"
        "    out.color = col[i];\n"
        "    return out;\n"
        "}\n"
        "fragment float4 color_fs(VertexOut in [[stage_in]]) { return in.color; }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
    if (!library || !_queue) { [self gpuError:error ? error.localizedDescription : @"Falha ao criar fila Metal"]; return; }
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"triangle_vs"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"color_fs"];
    descriptor.colorAttachments[0].pixelFormat = _metal.colorPixelFormat;
    _pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!_pipeline) { [self gpuError:error ? error.localizedDescription : @"Falha no pipeline"]; return; }
    _metal.delegate = self;
    _metal.paused = NO;
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
        _status.text = [NSString stringWithFormat:@"CPU/mem?ria/HLE: %u verifica??es, %u falhas\nBuild: %s",
            report.checks, report.failures, BB_BUILD_ID];
    } catch (const std::exception& error) {
        _report[@"cpu_error"] = [NSString stringWithUTF8String:error.what()];
        _status.text = @"CPU: falha inesperada; exporte o diagn?stico";
    }
}
- (void)drawInMTKView:(MTKView *)view {
    MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!pass || !drawable || !_pipeline) return;

    _angle += 0.02f;
    float cos_a = cosf(_angle), sin_a = sinf(_angle);
    simd_float4x4 rot = matrix_identity_float4x4;
    rot.columns[0][0] = cos_a;
    rot.columns[0][1] = sin_a;
    rot.columns[1][0] = -sin_a;
    rot.columns[1][1] = cos_a;
    memcpy(_uniformBuffer.contents, &rot, sizeof(rot));

    id<MTLCommandBuffer> command = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) { [self gpuError:@"Falha ao criar command buffer"]; return; }
    [encoder setRenderPipelineState:_pipeline];
    [encoder setVertexBuffer:_uniformBuffer offset:0 atIndex:0];
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
                if (strongSelf->_completedFrames == 1 || strongSelf->_completedFrames % 30 == 0)
                    strongSelf->_gpuStatus.text = [NSString stringWithFormat:@"Metal 3: %lu quadros. GPU: %@",
                        (unsigned long)strongSelf->_completedFrames, strongSelf->_report[@"metal_device"]];
            } else {
                [strongSelf gpuError:finished.error ? finished.error.localizedDescription : @"Erro no command buffer"];
            }
        });
    }];
    [command commit];
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }
- (void)exportReport:(UIButton *)sender {
    _report[@"timestamp"] = [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]];
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:_report options:NSJSONWritingPrettyPrinted error:&error];
    NSURL *directory = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
        inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:&error];
    NSURL *file = [directory URLByAppendingPathComponent:@"bb-lab-report.json"];
    if (!json || !file || ![json writeToURL:file options:NSDataWritingAtomic error:&error]) {
        _status.text = [@"Erro ao salvar diagn?stico: " stringByAppendingString:error ? error.localizedDescription : @"desconhecido"];
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
