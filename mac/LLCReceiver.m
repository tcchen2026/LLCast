#import "LLCReceiver.h"

#include <stdatomic.h>

#import "LLCDecoder.h"
#import "LLCVideoView.h"
#include "lls_net.h"

// Lets late display callbacks find out the session is gone.
@interface LLCSessionRef : NSObject {
@public
    LlsRxSession *session;
}
@end
@implementation LLCSessionRef
@end

@implementation LLCReceiver {
    uint16_t _port;
    LLCVideoView *_view;
    LLCSessionRef *_ref;
    LlsRxSession *_rx;
    LlsProfile *_profile;
    NSString *_profileListen, *_profileConnect;
    uint32_t _profileCapacity;
    NSThread *_thread;
    dispatch_semaphore_t _exited;
    id<NSObject> _activity;
    _Atomic(uint64_t) _shownCount;
    _Atomic(uint8_t) _codec;
}

- (instancetype)initWithPort:(uint16_t)port view:(LLCVideoView *)view profile:(LlsProfileConfig)profile
{
    if (!(self = [super init])) return nil;
    _port = port;
    _view = view;
    _profileListen = profile.listen ? @(profile.listen) : nil;
    _profileConnect = profile.connect ? @(profile.connect) : nil;
    _profileCapacity = profile.capacity;
    return self;
}

- (BOOL)start:(int *)error
{
    LlsRxConfig config;
    lls_rx_default_config(&config);
    // What this Mac decodes in hardware, narrowed by Settings > Accept (0: all of it).
    const uint32_t accepted = (uint32_t)[NSUserDefaults.standardUserDefaults integerForKey:@"acceptCodecs"];
    config.codecs = [LLCDecoder hardwareCodecs] & (accepted ? accepted : UINT32_MAX);
    config.port = _port;
    const LlsProfileConfig profiling = {_profileListen.UTF8String, _profileConnect.UTF8String, _profileCapacity};
    _profile = lls_rx_profile_open(&profiling, "LLCast_Mac_rx");
    if (lls_profile_enabled(&profiling) && !_profile) {
        if (error) *error = EIO;
        return NO;
    }
    config.profile = _profile;
    _rx = lls_rx_open(&config, error);
    if (!_rx) { lls_profile_close(_profile); _profile = NULL; return NO; }
    _ref = [LLCSessionRef new];
    _ref->session = _rx;
    _exited = dispatch_semaphore_create(0);
    _activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                                             reason:@"Receiving a stream"];
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(decodeLoop) object:nil];
    _thread.qualityOfService = NSQualityOfServiceUserInteractive;
    _thread.name = @"LLCast decode";
    [_thread start];
    fprintf(stderr, "Receiver: listening on UDP %u\n", _port);
    return YES;
}

- (void)decodeLoop
{
    LLCDecoder *decoder = [LLCDecoder new];
    LlsRxSession *rx = _rx;
    LLCSessionRef *ref = _ref;
    for (;;) {
        @autoreleasepool {
            bool reset = false;
            LlsRxFrame *frame = lls_rx_next(rx, -1, &reset);
            if (!frame) break;
            atomic_store(&_codec, frame->codec);
            if (reset) [decoder reset];
            const uint32_t session = frame->session, number = frame->frame;
            CVPixelBufferRef picture = NULL;
            const LLCDecodeResult result = [decoder decode:frame->data size:frame->size key:frame->key
                                                     codec:frame->codec picture:&picture];
            lls_rx_release_data(frame);
            if (result != LLCDecodePicture) {
                lls_rx_finish(rx, frame, result == LLCDecodeNeedKey ? LLS_FATE_SKIPPED : LLS_FATE_DECODE_ERROR, 0, 0, 0);
                if (result == LLCDecodeFailed) lls_rx_resync(rx, session, number);
                else lls_rx_recover(rx, session, number);
                continue;
            }
            const int64_t decoded = (int64_t)lls_net_now_ns();
            _Atomic(uint64_t) *shown = &_shownCount;
            [_view showPicture:picture done:^(BOOL wasShown, int64_t presentedNs, uint32_t sequence) {
                @synchronized(ref) {
                    if (!ref->session) return; // stopped; the session is gone
                    if (wasShown) atomic_fetch_add(shown, 1);
                    lls_rx_finish(ref->session, frame, wasShown ? LLS_FATE_SHOWN : LLS_FATE_REPLACED, decoded,
                                  presentedNs, sequence);
                }
            }];
            CVPixelBufferRelease(picture);
        }
    }
    dispatch_semaphore_signal(_exited);
}

- (void)stop
{
    if (!_rx) return;
    lls_rx_stop(_rx);
    dispatch_semaphore_wait(_exited, DISPATCH_TIME_FOREVER);
    // Pictures still on their way to the screen finish now or never.
    [_view drainWithTimeout:0.2];
    @synchronized(_ref) {
        _ref->session = NULL;
    }
    lls_rx_close(_rx);
    _rx = NULL;
    lls_profile_close(_profile);
    _profile = NULL;
    [_view clear];
    if (_activity) [NSProcessInfo.processInfo endActivity:_activity];
    _activity = nil;
}

- (void)dealloc
{
    [self stop];
}

- (LlsRxStatus)status
{
    LlsRxStatus status = {0};
    if (_rx) lls_rx_status(_rx, &status);
    return status;
}

- (uint64_t)shown
{
    return atomic_load(&_shownCount);
}

- (uint8_t)codec { return atomic_load(&_codec); }

@end
