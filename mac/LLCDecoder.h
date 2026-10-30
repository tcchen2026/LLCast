// Hardware HEVC/H.264/AV1 decoding of wire access units, synchronous: the
// picture comes back from the call that decodes it.
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, LLCDecodeResult) {
    LLCDecodePicture,     // a picture
    LLCDecodeNeedKey,     // no keyframe yet since the last reset
    LLCDecodeNoPicture,   // a missing reference or bad data; the session is fine
    LLCDecodeFailed,      // the session failed and was dropped: wait for a keyframe
};

@interface LLCDecoder : NSObject
+ (BOOL)hardwareAvailable;
+ (uint32_t)hardwareCodecs;
// A new sender session: forget the stream's format.
- (void)reset;
// *picture is +1 retained when the result is LLCDecodePicture.
- (LLCDecodeResult)decode:(const uint8_t *)au size:(size_t)size key:(BOOL)key codec:(uint8_t)codec
                  picture:(CVPixelBufferRef _Nullable *_Nonnull)picture;
@end

NS_ASSUME_NONNULL_END
