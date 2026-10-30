// Receives a stream into a video view: the shared receiver session (assembly,
// FEC, feedback), hardware HEVC/H.264/AV1 decoding on a dedicated thread, then the
// view. VideoToolbox gives no picture for a frame whose reference is lost,
// so after a loss the last good picture stays up until the keyframe that
// RECOVER asked for arrives (the box shows damaged frames instead).
#import <Foundation/Foundation.h>

#include "lls_rx_session.h"

@class LLCVideoView;

NS_ASSUME_NONNULL_BEGIN

@interface LLCReceiver : NSObject
- (instancetype)initWithPort:(uint16_t)port view:(LLCVideoView *)view profile:(LlsProfileConfig)profile;
// Binds the port and starts. On failure *error is the socket error.
- (BOOL)start:(int *)error;
- (void)stop;
- (LlsRxStatus)status;
@property(nonatomic, readonly) uint64_t shown;
@property(nonatomic, readonly) uint8_t codec;
@end

NS_ASSUME_NONNULL_END
