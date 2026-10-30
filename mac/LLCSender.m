#import "LLCSender.h"

#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <mach/mach_time.h>
#include <stdatomic.h>

#import "LLCEncoder.h"
#include "lls_net.h"
#include "lls_sender.h"
#include "lls_frame_policy.h"

NSErrorDomain const LLCSenderErrorDomain = @"LLCSender";
NSString *const LLCSocketErrorKey = @"socket error";

enum { FEC_PERCENT = 10 }; // user decision: 10% parity, inside the budget

LLCStreamMode LLCStreamModeFor(LlsStreamProfile profile, uint32_t budget_bps)
{
    return (LLCStreamMode){profile.width, profile.height, profile.fps, budget_bps};
}

@interface LLCSender () <SCStreamOutput, SCStreamDelegate>
@end

@implementation LLCSender {
    NSString *_host;
    uint16_t _port;
    uint8_t _wantedCodec;
    uint32_t _receiverCodecs;
    LLCStreamMode _mode;
    BOOL _scale;
    dispatch_queue_t _queue; // capture, encode and send: one thread at a time
    dispatch_source_t _timer, _recoveryWake;
    SCStream *_stream;
    LLCEncoder *_encoder;
    LlsSender *_sender;
    LlsProfile *_profile;
    NSString *_profileListen, *_profileConnect;
    uint32_t _profileCapacity;
    id<NSObject> _activity;
    // On _queue only.
    CVPixelBufferRef _pixels;
    uint64_t _captureUs, _acquiredUs;
    LlsFramePolicy _policy;
    BOOL _fresh, _stopped;
    // Read from any thread.
    _Atomic(uint64_t) _frames, _bytes, _keys;
}

- (instancetype)initWithHost:(NSString *)host port:(uint16_t)port mode:(LLCStreamMode)mode scale:(BOOL)scale
                     profile:(LlsProfileConfig)profile
{
    return [self initWithHost:host port:port mode:mode scale:scale codec:0
        receiverCodecs:LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) profile:profile];
}

- (instancetype)initWithHost:(NSString *)host port:(uint16_t)port mode:(LLCStreamMode)mode scale:(BOOL)scale
                       codec:(uint8_t)codec receiverCodecs:(uint32_t)receiverCodecs
                     profile:(LlsProfileConfig)profile
{
    if (!(self = [super init])) return nil;
    _host = [host copy];
    _port = port;
    _wantedCodec = codec;
    _receiverCodecs = receiverCodecs;
    _mode = mode;
    _scale = scale;
    _profileListen = profile.listen ? @(profile.listen) : nil;
    _profileConnect = profile.connect ? @(profile.connect) : nil;
    _profileCapacity = profile.capacity;
    dispatch_queue_attr_t attributes =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    _queue = dispatch_queue_create("LLCast sender", attributes);
    return self;
}

- (void)dealloc
{
    if (_pixels) CVPixelBufferRelease(_pixels);
    if (_sender) lls_sender_close(_sender);
    if (_timer) dispatch_source_cancel(_timer);
    if (_recoveryWake) dispatch_source_cancel(_recoveryWake);
    lls_profile_close(_profile);
}

static NSError *failure(LLCSenderError code, NSString *message, int socket_error)
{
    NSMutableDictionary *info = [@{NSLocalizedDescriptionKey: message} mutableCopy];
    if (socket_error) info[LLCSocketErrorKey] = @(socket_error);
    return [NSError errorWithDomain:LLCSenderErrorDomain code:code userInfo:info];
}

static uint64_t mach_to_us(uint64_t mach)
{
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom) mach_timebase_info(&timebase);
    return mach / 1000 * timebase.numer / timebase.denom +
           mach % 1000 * timebase.numer / timebase.denom / 1000;
}

- (void)startWithCompletion:(void (^)(NSError *_Nullable))completion
{
    void (^finish)(NSError *) = ^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{ completion(error); });
    };
    dispatch_async(_queue, ^{ [self startOnQueue:finish]; });
}

static void recovery_wake(void *context)
{
    // Merge instead of enqueuing one block per request. Lives through sender close.
    dispatch_source_merge_data((__bridge dispatch_source_t)context, 1);
}

- (void)startOnQueue:(void (^)(NSError *))finish
{
    if (_stopped) {
        finish(failure(LLCSenderErrorNoDisplay, @"Stopped before capture started.", 0));
        return;
    }
    NSError *error = nil;
    _codec = _wantedCodec <= LLS_CODEC_AV1 ?
        lls_codec_pick(_wantedCodec, [LLCEncoder hardwareCodecs], _receiverCodecs) : 0;
    if (!_codec) {
        NSString *why = _wantedCodec ? [NSString stringWithFormat:
            @"%@ can't use %s. Choose Auto or a codec supported by both devices.",
            (_wantedCodec <= LLS_CODEC_AV1 && (_receiverCodecs & LLS_CODEC_BIT(_wantedCodec))) ?
                @"This Mac's hardware encoder" : @"The receiver",
            lls_codec_name(_wantedCodec)] : @"This Mac and the receiver have no shared hardware HEVC or H.264 codec.";
        finish(failure(LLCSenderErrorEncoder, why, 0));
        return;
    }
    _encoder = [[LLCEncoder alloc] initWithWidth:_mode.width height:_mode.height fps:_mode.fps
                                         bitrate:(uint32_t)((uint64_t)_mode.budget_bps * 100 / (100 + FEC_PERCENT))
                                           codec:_codec error:&error];
    if (!_encoder) {
        finish(failure(LLCSenderErrorEncoder, [NSString stringWithFormat:
            @"This Mac can't open its %s hardware encoder (VideoToolbox %ld).", lls_codec_name(_codec), (long)error.code], 0));
        return;
    }
    LlsSenderConfig config;
    lls_sender_default_config(&config);
    config.codec = _codec;
    config.host = _host.UTF8String;
    config.port = _port;
    config.qos = true;
    config.fec_percent = FEC_PERCENT;
    config.budget_bps = _mode.budget_bps;
    config.fps = _mode.fps;
    _policy.period_us = 1000000 / (_mode.fps ? _mode.fps : 60);
    const LlsProfileConfig profiling = {_profileListen.UTF8String, _profileConnect.UTF8String, _profileCapacity};
    _profile = lls_sender_profile_open(&profiling, "LLCast_Mac_send", "UPTIME_RAW_us");
    if (lls_profile_enabled(&profiling) && !_profile) {
        finish(failure(LLCSenderErrorProfile, @"Can't open the requested profiling service.", 0));
        return;
    }
    [self startTimer];
    config.profile = _profile;
    config.stage_records = true;
    config.on_recover = recovery_wake;
    config.recover_context = (__bridge void *)_recoveryWake;
    _sender = lls_sender_open(&config);
    if (!_sender) {
        finish(failure(LLCSenderErrorNetwork, @"Can't open a network connection to the receiver.", errno));
        return;
    }
    _activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                                             reason:@"Streaming the screen"];
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent *content, NSError *contentError) {
            if (contentError || !content) {
                finish(contentError ?: failure(LLCSenderErrorNoDisplay, @"No display to capture.", 0));
                return;
            }
            // On the sender queue, so a stop that came first wins.
            dispatch_async(self->_queue, ^{
                if (self->_stopped) {
                    finish(failure(LLCSenderErrorNoDisplay, @"Stopped before capture started.", 0));
                    return;
                }
                [self startCapture:content finish:finish];
            });
        }];
}

// On _queue.
- (void)startCapture:(SCShareableContent *)content finish:(void (^)(NSError *))finish
{
    SCDisplay *display = nil;
    for (SCDisplay *candidate in content.displays)
        if (candidate.displayID == CGMainDisplayID()) display = candidate;
    if (!display) {
        finish(failure(LLCSenderErrorNoDisplay, @"The main display can't be captured.", 0));
        return;
    }
    // LLCast's own windows stay off the stream.
    NSMutableArray<SCRunningApplication *> *own = [NSMutableArray array];
    for (SCRunningApplication *app in content.applications)
        if (app.processID == NSProcessInfo.processInfo.processIdentifier) [own addObject:app];
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingApplications:own
                                                      exceptingWindows:@[]];
    SCStreamConfiguration *config = [SCStreamConfiguration new];
    const uint32_t width = _mode.width, height = _mode.height;
    config.width = width;
    config.height = height;
    // By default the whole display, fitted with its aspect ratio kept. "No
    // scale" takes the top-left mode-sized pixels; sourceRect is in points.
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display.displayID);
    const double scale = mode && CGDisplayModeGetWidth(mode)
        ? (double)CGDisplayModeGetPixelWidth(mode) / (double)CGDisplayModeGetWidth(mode) : 1.0;
    if (mode) CGDisplayModeRelease(mode);
    const BOOL cropped = !_scale && display.width * scale >= width && display.height * scale >= height;
    if (cropped) config.sourceRect = CGRectMake(0, 0, width / scale, height / scale);
    config.captureResolution = SCCaptureResolutionBest;
    config.preservesAspectRatio = YES;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
    config.colorSpaceName = kCGColorSpaceITUR_709;
    config.minimumFrameInterval = CMTimeMake(1, (int32_t)(_mode.fps ? _mode.fps : 60));
    config.queueDepth = 3;
    config.showsCursor = YES;
    config.capturesAudio = NO;
    fprintf(stderr, "Capture: display %u, %zux%zu points, scale %.2f, %s %ux%u\n", display.displayID,
            (size_t)display.width, (size_t)display.height, scale,
            cropped ? "top-left pixels, unscaled," : "whole display fitted to", width, height);
    _stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
    NSError *error = nil;
    if (![_stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:_queue error:&error]) {
        finish(error);
        return;
    }
    [_stream startCaptureWithCompletionHandler:^(NSError *startError) {
        finish(startError);
    }];
}

- (void)startTimer
{
    // No image means no deadline. Captures and recovery requests wake this queue.
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, DISPATCH_TIMER_STRICT, _queue);
    dispatch_source_set_timer(_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    __weak LLCSender *weakSelf = self;
    dispatch_source_set_event_handler(_timer, ^{ [weakSelf pump]; });
    dispatch_resume(_timer);
    _recoveryWake = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_ADD, 0, 0, _queue);
    dispatch_source_set_event_handler(_recoveryWake, ^{ [weakSelf pump]; });
    dispatch_resume(_recoveryWake);
}

- (void)scheduleNext
{
    if (!_timer || _stopped) return;
    const uint64_t due = lls_frame_policy_deadline(&_policy, _fresh);
    dispatch_time_t when = DISPATCH_TIME_FOREVER;
    if (due != UINT64_MAX) {
        const uint64_t now = lls_net_now_us();
        when = dispatch_time(DISPATCH_TIME_NOW, due > now ? (int64_t)(due - now) * NSEC_PER_USEC : 0);
    }
    dispatch_source_set_timer(_timer, when, DISPATCH_TIME_FOREVER, 0);
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen || _stopped) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || !CFArrayGetCount(attachments)) return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    // Idle frames repeat the previous content and carry no image.
    if ([info[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels) return;
    if (_pixels) CVPixelBufferRelease(_pixels);
    _pixels = CVPixelBufferRetain(pixels);
    const uint64_t now = lls_net_now_us();
    _acquiredUs = now;
    NSNumber *displayed = info[SCStreamFrameInfoDisplayTime];
    const uint64_t shown = displayed ? mach_to_us(displayed.unsignedLongLongValue) : 0;
    _captureUs = shown && shown <= now ? shown : now; // mach time is the lls_net clock here
    _fresh = YES;
    [self pump];
}

- (void)pump
{
    if (!_pixels || _stopped) return;
    const uint64_t now = lls_net_now_us();
    const BOOL first = !_policy.have_image;
    const BOOL recover = lls_sender_need_keyframe(_sender);
    const BOOL urgent = lls_sender_recover_now(_sender);
    if (!lls_frame_policy_should_encode(&_policy, now, _fresh, urgent)) { [self scheduleNext]; return; }
    const BOOL repeated = !_fresh;
    LLCEncodedFrame frame;
    if (![_encoder encode:_pixels key:first || recover frame:&frame]) {
        [self failWith:failure(LLCSenderErrorEncoder, @"The video encoder stopped working.", 0)];
        return;
    }
    lls_frame_policy_encoded(&_policy, now, repeated);
    _fresh = NO;
    if (!frame.size) { [self scheduleNext]; return; } // the rate control dropped it
    const LlsSenderFrame send = {frame.data, frame.size, frame.key, _captureUs, lls_net_now_us()};
    uint32_t number = 0;
    if (!lls_sender_send(_sender, &send, &number)) {
        const int error = lls_net_error();
        int64_t offset; uint32_t rtt;
        BOOL unsupported = lls_sender_clock(_sender, &offset, &rtt) &&
            !(lls_sender_peer_codecs(_sender) & LLS_CODEC_BIT(_codec));
        [self failWith:failure(LLCSenderErrorNetwork, unsupported ?
            [NSString stringWithFormat:@"The receiver no longer supports %s. Stop and start a new stream.", lls_codec_name(_codec)] :
            @"Sending to the receiver failed.", error)];
        return;
    }
    const LlsSenderStages stages = {.acquired_us = _acquiredUs, .encode_start_us = now,
        .send_return_us = lls_net_now_us(), .repeated = repeated};
    lls_sender_set_stages(_sender, number, &stages);
    atomic_fetch_add(&_frames, 1);
    atomic_store(&_bytes, lls_sender_sent_bytes(_sender));
    if (frame.key) atomic_fetch_add(&_keys, 1);
    [self scheduleNext];
}

// On _queue.
- (void)failWith:(NSError *)error
{
    if (_stopped) return;
    [self stopOnQueue];
    void (^handler)(NSError *) = self.onFailure;
    if (handler) dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    dispatch_async(_queue, ^{ [self failWith:error]; });
}

// On _queue.
- (void)stopOnQueue
{
    if (_stopped) return;
    _stopped = YES;
    if (_timer) dispatch_source_cancel(_timer);
    _timer = nil;
    [_stream stopCaptureWithCompletionHandler:nil];
    if (_sender) lls_sender_close(_sender); // prints the session summary
    _sender = NULL;
    if (_recoveryWake) dispatch_source_cancel(_recoveryWake);
    _recoveryWake = nil;
    lls_profile_close(_profile);
    _profile = NULL;
    if (_pixels) CVPixelBufferRelease(_pixels);
    _pixels = NULL;
    if (_activity) [NSProcessInfo.processInfo endActivity:_activity];
    _activity = nil;
}

- (void)stop
{
    dispatch_sync(_queue, ^{ [self stopOnQueue]; });
}

- (LLCSenderStatus)status
{
    __block LLCSenderStatus status = {.frames = atomic_load(&_frames), .bytes = atomic_load(&_bytes),
        .keys = atomic_load(&_keys), .rtt_us = UINT32_MAX};
    dispatch_sync(_queue, ^{
        status.low_latency = self->_encoder.lowLatency;
        status.codec = self->_codec;
        int64_t offset;
        uint32_t value;
        if (self->_sender && lls_sender_clock(self->_sender, &offset, &value)) status.rtt_us = value;
        if (self->_sender && !lls_sender_quality(self->_sender, &status.quality)) status.quality = (LlsQuality){0};
    });
    return status;
}

@end
