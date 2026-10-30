#import "LLCDecoder.h"

#import <VideoToolbox/VideoToolbox.h>

#include "LLCHevc.h"

@implementation LLCDecoder {
    VTDecompressionSessionRef _session;
    CMVideoFormatDescriptionRef _format;
    CVPixelBufferRef _picture; // set by the output callback during a decode
    OSStatus _pictureStatus;
    uint8_t _codec;
}

+ (void)initialize
{
    // Register Apple's optional AV1 decoder before capability queries or use.
    // NSObject calls this once; the macOS deployment target is 14 or later.
    if (self == [LLCDecoder class])
        VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_AV1);
}

+ (BOOL)hardwareAvailable
{
    return VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
}

+ (uint32_t)hardwareCodecs
{
    uint32_t codecs = 0;
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC)) codecs |= LLS_CODEC_BIT(LLS_CODEC_HEVC);
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_H264)) codecs |= LLS_CODEC_BIT(LLS_CODEC_H264);
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1)) codecs |= LLS_CODEC_BIT(LLS_CODEC_AV1);
    return codecs;
}

static void on_decoded(void *opaque, void *source, OSStatus status, VTDecodeInfoFlags flags,
                       CVImageBufferRef image, CMTime pts, CMTime duration)
{
    (void)source; (void)pts; (void)duration;
    LLCDecoder *decoder = (__bridge LLCDecoder *)opaque;
    decoder->_pictureStatus = status;
    if (status == noErr && image && !(flags & kVTDecodeInfo_FrameDropped))
        decoder->_picture = CVPixelBufferRetain(image);
}

- (void)reset
{
    if (_session) {
        VTDecompressionSessionInvalidate(_session);
        CFRelease(_session);
        _session = NULL;
    }
    if (_format) CFRelease(_format);
    _format = NULL;
}

- (void)dealloc
{
    [self reset];
}

- (BOOL)openWithFormat:(CMVideoFormatDescriptionRef)format
{
    [self reset];
    NSDictionary *spec = @{(id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
    NSDictionary *output = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    VTDecompressionOutputCallbackRecord callback = {on_decoded, (__bridge void *)self};
    const OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, (__bridge CFDictionaryRef)spec,
                                                         (__bridge CFDictionaryRef)output, &callback, &_session);
    if (status != noErr) {
        fprintf(stderr, "Decoder: cannot create a hardware %s session (%d)\n", lls_codec_name(_codec), (int)status);
        _session = NULL;
        return NO;
    }
    VTSessionSetProperty(_session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    _format = (CMVideoFormatDescriptionRef)CFRetain(format);
    const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    fprintf(stderr, "Decoder: hardware %s %dx%d, NV12 to Metal\n",
            lls_codec_name(_codec), dimensions.width, dimensions.height);
    return YES;
}

- (LLCDecodeResult)decode:(const uint8_t *)au size:(size_t)size key:(BOOL)key codec:(uint8_t)codec
                  picture:(CVPixelBufferRef _Nullable *)picture
{
    *picture = NULL;
    if (codec != _codec) { [self reset]; _codec = codec; }
    if (key) {
        CMVideoFormatDescriptionRef format = llc_video_format_from_annexb(codec, au, size);
        if (format) {
            BOOL ok = YES;
            if (!_session || !VTDecompressionSessionCanAcceptFormatDescription(_session, format)) {
                ok = [self openWithFormat:format];
            } else {
                CFRelease(_format);
                _format = (CMVideoFormatDescriptionRef)CFRetain(format);
            }
            CFRelease(format);
            if (!ok) return LLCDecodeFailed;
        }
    }
    if (!_session) return LLCDecodeNeedKey;
    CMSampleBufferRef sample = llc_video_sample_from_annexb(codec, au, size, _format);
    if (!sample) return LLCDecodeNoPicture;
    _picture = NULL;
    _pictureStatus = noErr;
    VTDecodeInfoFlags info = 0;
    // No asynchronous flag: the callback runs before this returns.
    OSStatus status = VTDecompressionSessionDecodeFrame(_session, sample, 0, NULL, &info);
    CFRelease(sample);
    if (status == noErr) status = _pictureStatus;
    if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) {
        fprintf(stderr, "Decoder: session failed (%d); waiting for a keyframe\n", (int)status);
        if (_picture) CVPixelBufferRelease(_picture);
        _picture = NULL;
        [self reset];
        return LLCDecodeFailed;
    }
    if (status != noErr || !_picture) {
        if (_picture) CVPixelBufferRelease(_picture);
        _picture = NULL;
        return LLCDecodeNoPicture;
    }
    *picture = _picture;
    _picture = NULL;
    return LLCDecodePicture;
}

@end
