#import "LLCEncoder.h"

#import <VideoToolbox/VideoToolbox.h>

#include "LLCHevc.h"
#include "hevc_au.h"

@implementation LLCEncoder {
    VTCompressionSessionRef _session;
    CMSampleBufferRef _output; // set by the output callback during an encode
    OSStatus _outputStatus;
    LLCBytes _bytes;
    int64_t _count;
    int32_t _fps;
}

static void on_output(void *opaque, void *source, OSStatus status, VTEncodeInfoFlags flags,
                      CMSampleBufferRef sample)
{
    (void)source;
    LLCEncoder *encoder = (__bridge LLCEncoder *)opaque;
    encoder->_outputStatus = status;
    if (status == noErr && sample && !(flags & kVTEncodeInfo_FrameDropped))
        encoder->_output = (CMSampleBufferRef)CFRetain(sample);
}

static void set_property(VTCompressionSessionRef session, CFStringRef key, id value)
{
    const OSStatus status = VTSessionSetProperty(session, key, (__bridge CFTypeRef)value);
    if (status != noErr)
        fprintf(stderr, "VideoToolbox: %s not accepted (%d)\n", [(__bridge NSString *)key UTF8String], (int)status);
}

- (nullable instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                                bitrate:(uint32_t)bitsPerSecond
                                  error:(NSError **)error
{
    return [self initWithWidth:width height:height bitrate:bitsPerSecond codec:LLS_CODEC_HEVC error:error];
}

+ (uint32_t)hardwareCodecs
{
    CFArrayRef list = NULL;
    if (VTCopyVideoEncoderList(NULL, &list) != noErr || !list) return 0;
    uint32_t codecs = 0;
    for (NSDictionary *encoder in CFBridgingRelease(list)) {
        if (![encoder[(id)kVTVideoEncoderList_IsHardwareAccelerated] boolValue]) continue;
        const CMVideoCodecType type = [encoder[(id)kVTVideoEncoderList_CodecType] unsignedIntValue];
        if (type == kCMVideoCodecType_HEVC) codecs |= LLS_CODEC_BIT(LLS_CODEC_HEVC);
        if (type == kCMVideoCodecType_H264) codecs |= LLS_CODEC_BIT(LLS_CODEC_H264);
    }
    return codecs;
}

- (nullable instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                                bitrate:(uint32_t)bitsPerSecond codec:(uint8_t)codec
                                  error:(NSError **)error
{
    return [self initWithWidth:width height:height fps:60 bitrate:bitsPerSecond codec:codec error:error];
}

- (nullable instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height fps:(uint32_t)fps
                                bitrate:(uint32_t)bitsPerSecond codec:(uint8_t)codec
                                  error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    _fps = (int32_t)(fps ? fps : 60);
    if (codec != LLS_CODEC_HEVC && codec != LLS_CODEC_H264) {
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:kVTCouldNotFindVideoEncoderErr
            userInfo:@{NSLocalizedDescriptionKey: @"Choose a hardware HEVC or H.264 encoder."}];
        return nil;
    }
    _codec = codec;
    const CMVideoCodecType type = codec == LLS_CODEC_H264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
    OSStatus status = noErr;
    // Try the low-latency encoder, then the regular hardware encoder.
    for (int attempt = 0; attempt < 2 && !_session; ++attempt) {
        NSMutableDictionary *spec = [@{
            (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES,
        } mutableCopy];
        if (attempt == 0) spec[(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
        status = VTCompressionSessionCreate(kCFAllocatorDefault, (int32_t)width, (int32_t)height, type,
                                            (__bridge CFDictionaryRef)spec, NULL, kCFAllocatorDefault,
                                            on_output, (__bridge void *)self, &_session);
        if (status == noErr) _lowLatency = attempt == 0;
        else _session = NULL;
    }
    if (!_session) {
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:nil];
        return nil;
    }
    set_property(_session, kVTCompressionPropertyKey_RealTime, @YES);
    set_property(_session, kVTCompressionPropertyKey_ProfileLevel, (__bridge id)(codec == LLS_CODEC_H264
        ? kVTProfileLevel_H264_Main_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel));
    set_property(_session, kVTCompressionPropertyKey_AllowFrameReordering, @NO);
    set_property(_session, kVTCompressionPropertyKey_AllowOpenGOP, @NO);
    set_property(_session, kVTCompressionPropertyKey_MaxKeyFrameInterval, @(INT32_MAX));
    set_property(_session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, @(1e9));
    set_property(_session, kVTCompressionPropertyKey_ExpectedFrameRate, @(_fps));
    set_property(_session, kVTCompressionPropertyKey_AverageBitRate, @(bitsPerSecond));
    set_property(_session, kVTCompressionPropertyKey_DataRateLimits, @[ @(bitsPerSecond / 8), @1.0 ]);
    set_property(_session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, @YES);
    if (!_lowLatency) set_property(_session, kVTCompressionPropertyKey_MaxFrameDelayCount, @0);
    set_property(_session, kVTCompressionPropertyKey_ColorPrimaries, (__bridge id)kCVImageBufferColorPrimaries_ITU_R_709_2);
    set_property(_session, kVTCompressionPropertyKey_TransferFunction, (__bridge id)kCVImageBufferTransferFunction_ITU_R_709_2);
    set_property(_session, kVTCompressionPropertyKey_YCbCrMatrix, (__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    VTCompressionSessionPrepareToEncodeFrames(_session);
    fprintf(stderr, "Encoder: hardware %s %ux%u at %u bit/s, low-latency rate control %s\n",
            lls_codec_name(codec), width, height, bitsPerSecond, _lowLatency ? "on" : "unavailable");
    return self;
}

- (void)dealloc
{
    if (_session) {
        VTCompressionSessionInvalidate(_session);
        CFRelease(_session);
    }
    llc_bytes_free(&_bytes);
}

- (BOOL)encode:(CVPixelBufferRef)pixels key:(BOOL)key frame:(LLCEncodedFrame *)frame
{
    *frame = (LLCEncodedFrame){NULL, 0, false};
    const CMTime pts = CMTimeMake(_count++, _fps);
    NSDictionary *options = key ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
    _output = NULL;
    _outputStatus = noErr;
    VTEncodeInfoFlags flags = 0;
    OSStatus status = VTCompressionSessionEncodeFrame(_session, pixels, pts, kCMTimeInvalid,
                                                      (__bridge CFDictionaryRef)options, NULL, &flags);
    // Waits until the output callback has run for this frame.
    if (status == noErr) status = VTCompressionSessionCompleteFrames(_session, pts);
    if (status == noErr) status = _outputStatus;
    if (status != noErr) {
        fprintf(stderr, "Encoder: frame failed (%d)\n", (int)status);
        if (_output) CFRelease(_output);
        _output = NULL;
        return NO;
    }
    if (!_output) return YES; // dropped by the rate control
    bool is_key = llc_sample_is_key(_output);
    bool ok = llc_video_annexb_from_sample(_codec, _output, is_key || key, &_bytes);
    CFRelease(_output);
    _output = NULL;
    if (ok && (is_key || key)) {
        LlsAccessUnit *units = NULL; size_t count = 0;
        ok = lls_split_access_units(_codec, _bytes.data, _bytes.size, &units, &count) &&
            count == 1 && units[0].key;
        is_key = ok;
        free(units);
    }
    if (!ok) {
        fprintf(stderr, "Encoder: unexpected bitstream layout\n");
        return NO;
    }
    *frame = (LLCEncodedFrame){_bytes.data, _bytes.size, is_key};
    return YES;
}

@end
