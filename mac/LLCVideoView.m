#import "LLCVideoView.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>

// A full-screen triangle; BT.709 limited-range NV12 to RGB.
static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Varyings { float4 position [[position]]; float2 uv; };\n"
    "vertex Varyings vertex_main(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    Varyings out;\n"
    "    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    out.uv = float2(p.x, 1.0 - p.y);\n"
    "    return out;\n"
    "}\n"
    "fragment float4 fragment_main(Varyings in [[stage_in]],\n"
    "                              texture2d<float> luma [[texture(0)]],\n"
    "                              texture2d<float> chroma [[texture(1)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float y = (luma.sample(s, in.uv).r * 255.0 - 16.0) / 219.0;\n"
    "    float2 c = (chroma.sample(s, in.uv).rg * 255.0 - 128.0) / 224.0;\n"
    "    float3 rgb = float3(y + 1.5748 * c.y,\n"
    "                        y - 0.187324 * c.x - 0.468124 * c.y,\n"
    "                        y + 1.8556 * c.x);\n"
    "    return float4(saturate(rgb), 1.0);\n"
    "}\n";

// A present not confirmed by then is treated as done, so a hidden window
// can't stall the stream.
static const NSTimeInterval kPresentTimeout = 0.25;

@implementation LLCVideoView {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    CVMetalTextureCacheRef _cache;
    CAMetalLayer *_metal;
    NSCondition *_condition;
    // Guarded by _condition.
    CVPixelBufferRef _waiting;
    LLCPictureDone _waitingDone;
    BOOL _inFlight, _stopping, _clear;
    uint64_t _token;
    NSDate *_inFlightSince;
    uint32_t _sequence;
}

+ (nullable instancetype)viewWithFrame:(NSRect)frame
{
    return [[self alloc] initWithFrame:frame];
}

// Returns nil without a usable Metal device; viewWithFrame: says so to callers.
- (instancetype)initWithFrame:(NSRect)frame
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || !(self = [super initWithFrame:frame])) return nil;
    _device = device;
    _queue = [device newCommandQueue];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kShaders options:nil error:&error];
    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    description.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _pipeline = library ? [device newRenderPipelineStateWithDescriptor:description error:&error] : nil;
    if (!_pipeline || CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, device, NULL, &_cache) != kCVReturnSuccess) {
        NSLog(@"LLCast: Metal setup failed: %@", error);
        return nil;
    }
    _condition = [NSCondition new];
    self.wantsLayer = YES; // makeBackingLayer below
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    // The render thread keeps the view with its own reference, given back on
    // the main thread: an NSView must be destroyed there. An NSThread's QoS is
    // read-only once it has started.
    __weak LLCVideoView *weakSelf = self;
    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        void *view = (__bridge_retained void *)weakSelf;
        if (!view) return;
        [(__bridge LLCVideoView *)view renderLoop];
        dispatch_async(dispatch_get_main_queue(), ^{ (void)(__bridge_transfer LLCVideoView *)view; });
    }];
    thread.qualityOfService = NSQualityOfServiceUserInteractive;
    thread.name = @"LLCast render";
    [thread start];
    return self;
}

- (void)dealloc
{
    if (_cache) CFRelease(_cache);
    if (_waiting) CVPixelBufferRelease(_waiting);
}

- (CALayer *)makeBackingLayer
{
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = _device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    layer.maximumDrawableCount = 3; // needed for direct-to-display
    layer.displaySyncEnabled = !_allowTearing;
    layer.presentsWithTransaction = NO;
    layer.allowsNextDrawableTimeout = YES;
    layer.backgroundColor = NSColor.blackColor.CGColor;
    _metal = layer;
    return layer;
}

- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {}
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)event
{
    if (event.keyCode == 53 && self.onEscape) self.onEscape(); // Esc
    else [super keyDown:event];
}

- (void)updateDrawableSize
{
    const CGFloat scale = self.window ? self.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    _metal.contentsScale = scale;
    _metal.drawableSize = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
}

- (void)layout
{
    [super layout];
    [self updateDrawableSize];
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

- (void)setAllowTearing:(BOOL)allowTearing
{
    _allowTearing = allowTearing;
    _metal.displaySyncEnabled = !allowTearing;
}

- (void)showPicture:(CVPixelBufferRef)picture done:(LLCPictureDone)done
{
    LLCPictureDone replaced = nil;
    BOOL stopping;
    [_condition lock];
    stopping = _stopping;
    if (!stopping) {
        if (_waiting) {
            CVPixelBufferRelease(_waiting);
            replaced = _waitingDone;
        }
        _waiting = CVPixelBufferRetain(picture);
        _waitingDone = [done copy];
        [_condition signal];
    }
    [_condition unlock];
    if (replaced) replaced(NO, 0, 0);
    if (stopping) done(NO, 0, 0);
}

- (LLCPictureDone)takeWaitingLocked
{
    LLCPictureDone done = _waitingDone;
    if (_waiting) CVPixelBufferRelease(_waiting);
    _waiting = NULL;
    _waitingDone = nil;
    return done;
}

- (BOOL)drainWithTimeout:(NSTimeInterval)timeout
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:timeout];
    [_condition lock];
    LLCPictureDone replaced = [self takeWaitingLocked];
    while (_inFlight && [_condition waitUntilDate:until]) {}
    const BOOL drained = !_inFlight;
    [_condition unlock];
    if (replaced) replaced(NO, 0, 0);
    return drained;
}

- (void)clear
{
    [_condition lock];
    LLCPictureDone replaced = [self takeWaitingLocked];
    _clear = YES;
    [_condition signal];
    [_condition unlock];
    if (replaced) replaced(NO, 0, 0);
}

- (void)invalidate
{
    [_condition lock];
    LLCPictureDone replaced = [self takeWaitingLocked];
    _stopping = YES;
    [_condition signal];
    [_condition unlock];
    if (replaced) replaced(NO, 0, 0);
}

// Called from a presented handler; returns the picture's sequence number.
- (uint32_t)presented:(uint64_t)token shown:(BOOL)shown
{
    [_condition lock];
    if (token == _token) _inFlight = NO;
    const uint32_t sequence = shown ? ++_sequence : 0;
    [_condition signal];
    [_condition unlock];
    return sequence;
}

- (void)renderLoop
{
    for (;;) {
        @autoreleasepool {
            [_condition lock];
            while (!_stopping) {
                if (_inFlight && -[_inFlightSince timeIntervalSinceNow] > kPresentTimeout) _inFlight = NO;
                if (!_inFlight && (_waiting || _clear)) break;
                [_condition waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:_inFlight ? 0.05 : 1.0]];
            }
            if (_stopping) {
                [_condition unlock];
                return;
            }
            CVPixelBufferRef picture = _waiting; // the reference moves to this thread
            LLCPictureDone done = _waitingDone;
            _waiting = NULL;
            _waitingDone = nil;
            _clear = NO;
            _inFlight = YES;
            const uint64_t token = ++_token;
            _inFlightSince = [NSDate date];
            [_condition unlock];
            [self render:picture done:done token:token];
            if (picture) CVPixelBufferRelease(picture);
        }
    }
}

- (void)render:(nullable CVPixelBufferRef)picture done:(nullable LLCPictureDone)done token:(uint64_t)token
{
    id<CAMetalDrawable> drawable = [_metal nextDrawable];
    if (!drawable) {
        [self presented:token shown:NO];
        if (done) done(NO, 0, 0);
        return;
    }
    CVMetalTextureRef luma = NULL, chroma = NULL;
    if (picture) {
        CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _cache, picture, NULL, MTLPixelFormatR8Unorm,
            CVPixelBufferGetWidthOfPlane(picture, 0), CVPixelBufferGetHeightOfPlane(picture, 0), 0, &luma);
        CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _cache, picture, NULL, MTLPixelFormatRG8Unorm,
            CVPixelBufferGetWidthOfPlane(picture, 1), CVPixelBufferGetHeightOfPlane(picture, 1), 1, &chroma);
    }
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (luma && chroma) {
        const double W = drawable.texture.width, H = drawable.texture.height;
        double w = CVPixelBufferGetWidth(picture), h = CVPixelBufferGetHeight(picture);
        // Pixel for pixel when the picture fits and is within 10% of the
        // screen in both dimensions (as on the box); otherwise fitted with
        // its aspect ratio kept. Centred, black bars, never cropped.
        const BOOL near = w <= W && h <= H && w * 10 >= W * 9 && h * 10 >= H * 9;
        if (!near) {
            const double fit = fmin(W / w, H / h);
            w *= fit;
            h *= fit;
        }
        [encoder setViewport:(MTLViewport){floor((W - w) / 2), floor((H - h) / 2), w, h, 0, 1}];
        [encoder setRenderPipelineState:_pipeline];
        [encoder setFragmentTexture:CVMetalTextureGetTexture(luma) atIndex:0];
        [encoder setFragmentTexture:CVMetalTextureGetTexture(chroma) atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    __weak LLCVideoView *weakSelf = self;
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        // presentedTime is CACurrentMediaTime: mach time, the lls_net clock on macOS.
        const CFTimeInterval at = presented.presentedTime;
        const uint32_t sequence = [weakSelf presented:token shown:at > 0];
        if (done) done(at > 0, at > 0 ? (int64_t)(at * 1e9) : 0, sequence);
    }];
    CVPixelBufferRef kept = picture ? CVPixelBufferRetain(picture) : NULL;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        (void)buffer;
        if (luma) CFRelease(luma);
        if (chroma) CFRelease(chroma);
        if (kept) CVPixelBufferRelease(kept);
    }];
    [commands presentDrawable:drawable];
    [commands commit];
}

@end
