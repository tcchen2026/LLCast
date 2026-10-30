// Streams this Mac's main display: ScreenCaptureKit (BT.709 NV12 at the mode's
// size, scaled and converted by the window server), hardware HEVC/H.264, then the
// shared sender session. Frame policy as on Windows: encode a change as soon as
// period - 2 ms has passed since the last encode, repeat the last image on
// the 60 Hz grid when nothing changes, and send an IDR at once on RECOVER.
#import <Foundation/Foundation.h>
#include "lls_profile.h"
#include "lls_sender.h"
#include "lls_stream_profile.h"

NS_ASSUME_NONNULL_BEGIN

// A profile and its bandwidth budget (user decision), as on every sender.
typedef struct {
    uint32_t width, height, fps;
    uint32_t budget_bps; // video + FEC: LLCast sends 10% parity, paid from the budget
} LLCStreamMode;
LLCStreamMode LLCStreamModeFor(LlsStreamProfile profile, uint32_t budget_bps);

typedef struct {
    uint64_t frames, bytes, keys;
    uint32_t rtt_us; // UINT32_MAX: the receiver hasn't answered in 5 s
    BOOL low_latency;
    uint8_t codec; // LLS_CODEC_* in use, 0 before the encoder opens
    LlsQuality quality; // level 0: no receiver STATS yet, or none lately
} LLCSenderStatus;

@interface LLCSender : NSObject
// scale (the default): the whole display fitted to the mode, aspect ratio kept.
// Otherwise the top-left part at full size ("no scale"); a display smaller
// than the mode is then still sent whole, scaled.
- (instancetype)initWithHost:(NSString *)host port:(uint16_t)port mode:(LLCStreamMode)mode scale:(BOOL)scale
                       codec:(uint8_t)codec receiverCodecs:(uint32_t)receiverCodecs
                     profile:(LlsProfileConfig)profile;
// Compatibility: Auto, receiver HEVC|H.264. New callers pass the probe's set.
- (instancetype)initWithHost:(NSString *)host port:(uint16_t)port mode:(LLCStreamMode)mode scale:(BOOL)scale
                     profile:(LlsProfileConfig)profile;
@property(nonatomic, readonly) uint8_t codec; // codec in use after start succeeds
// Completion runs on the main queue; error is nil once frames are flowing.
- (void)startWithCompletion:(void (^)(NSError *_Nullable error))completion;
// Called on the main queue if streaming stops by itself.
@property(nonatomic, copy, nullable) void (^onFailure)(NSError *error);
- (void)stop;
- (LLCSenderStatus)status;
@end

// Error domain for failures the window explains; codes below.
extern NSErrorDomain const LLCSenderErrorDomain;
typedef NS_ENUM(NSInteger, LLCSenderError) {
    LLCSenderErrorNoDisplay = 1,
    LLCSenderErrorEncoder,
    LLCSenderErrorNetwork,   // userInfo LLCSocketErrorKey: errno
    LLCSenderErrorProfile,
};
extern NSString *const LLCSocketErrorKey;

NS_ASSUME_NONNULL_END
