// Shows decoded pictures with the lowest delay the display allows: the newest
// picture wins, at most one present is pending (as on the box), pictures near
// the screen's size are shown pixel for pixel, others fitted, and the
// zero-copy path keeps the decoder's IOSurface as the Metal texture. Full
// screen and opaque, so macOS can scan it out directly.
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>

NS_ASSUME_NONNULL_BEGIN

// shown: the picture reached the screen at presentedNs (lls_net_now_ns
// clock); otherwise a newer picture replaced it. Runs once, on any thread.
typedef void (^LLCPictureDone)(BOOL shown, int64_t presentedNs, uint32_t sequence);

@interface LLCVideoView : NSView
// NO (default): present at vblank. YES: present at once and allow tearing.
@property(nonatomic) BOOL allowTearing;
@property(nonatomic, copy, nullable) void (^onEscape)(void);
// nil without a usable Metal device.
+ (nullable instancetype)viewWithFrame:(NSRect)frame;
// Any thread. Retains the picture until it is shown or replaced.
- (void)showPicture:(CVPixelBufferRef)picture done:(LLCPictureDone)done;
// Replaces any waiting picture and waits up to timeout for the pending
// present, so no done block runs after it returns (unless it timed out).
- (BOOL)drainWithTimeout:(NSTimeInterval)timeout;
// Clears the screen to black.
- (void)clear;
// Stops the render thread; call before letting the view go.
- (void)invalidate;
@end

NS_ASSUME_NONNULL_END
