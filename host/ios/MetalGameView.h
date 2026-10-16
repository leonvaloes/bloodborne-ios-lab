#pragma once
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#include "video_out.h"

@class MetalGameView;

@protocol MetalGameViewDelegate <NSObject>
@optional
- (void)metalGameView:(MetalGameView *)view didPresentFrame:(uint64_t)sequence;
- (void)metalGameView:(MetalGameView *)view didFailWithError:(NSString *)error;
@end

@interface MetalGameView : MTKView

@property (nonatomic, weak) id<MetalGameViewDelegate> gameViewDelegate;
@property (nonatomic, readonly) BOOL hasActiveGuestTexture;
@property (nonatomic, readonly) uint64_t presentedFrameCount;
@property (nonatomic, readonly) id<MTLDepthStencilState> depthStencilState;
@property (nonatomic, readonly) id<MTLDepthStencilState> depthStencilStateReadOnly;
@property (nonatomic) float cameraFieldOfView;
@property (nonatomic) float cameraNearZ;
@property (nonatomic) float cameraFarZ;

- (instancetype)initWithFrame:(CGRect)frame device:(id<MTLDevice>)device;
- (void)bindVideoOutManager:(bb::VideoOutManager *)videoMgr;
- (BOOL)updateTextureFromVideoManager;
- (void)drawGuestFrameWithEncoder:(id<MTLRenderCommandEncoder>)encoder;
- (void)setupDepthBufferWithDevice:(id<MTLDevice>)device;
- (void)configureDepthTestingForEncoder:(id<MTLRenderCommandEncoder>)encoder enabled:(BOOL)enabled write:(BOOL)write;

@end
