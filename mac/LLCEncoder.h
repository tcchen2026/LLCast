// Hardware HEVC/H.264/AV1 encoding: the profile's size and rate, P-frames only,
// no reordering, an IDR only when asked, BT.709 limited range. Synchronous:
// encode returns with the bitstream.
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include "llstream_proto.h"

NS_ASSUME_NONNULL_BEGIN

typedef struct {
    const uint8_t *data; // Annex-B, parameter sets first on IDRs; valid until the next encode
    size_t size;         // 0: the rate control dropped this frame
    bool key;
} LLCEncodedFrame;

@interface LLCEncoder : NSObject
// Supported codecs reported by VTCopyVideoEncoderList as hardware accelerated.
+ (uint32_t)hardwareCodecs;
// Explicit codec; nil if hardware encoding is unavailable, with *error.
- (nullable instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height fps:(uint32_t)fps
                                bitrate:(uint32_t)bitsPerSecond codec:(uint8_t)codec
                                  error:(NSError **)error;
// 60 fps.
- (nullable instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                                bitrate:(uint32_t)bitsPerSecond codec:(uint8_t)codec
                                  error:(NSError **)error;
// HEVC compatibility initializer for the existing self-test.
- (nullable instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                                bitrate:(uint32_t)bitsPerSecond
                                  error:(NSError **)error;
@property(nonatomic, readonly) uint8_t codec;
// Low-latency rate control was accepted by VideoToolbox.
@property(nonatomic, readonly) BOOL lowLatency;
// Returns NO only if the encoder failed.
- (BOOL)encode:(CVPixelBufferRef)pixels key:(BOOL)key frame:(LLCEncodedFrame *)frame;
@end

NS_ASSUME_NONNULL_END
