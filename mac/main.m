// LLCast for macOS. Without arguments: the window. For Terminal use:
//   LLCast --send HOST [--port 47800] [--mode 720p|1080p] [--codec auto|av1|hevc|h264] [--no-scale]
//                                       stream this Mac's screen until Ctrl-C
//   LLCast --selftest                   encode/network/decode check, no permissions needed
#import <AppKit/AppKit.h>
#include <signal.h>

#import "LLCAppDelegate.h"
#import "LLCPrecheck.h"
#import "LLCSelfTest.h"
#import "LLCSender.h"
#import "LLCEncoder.h"
#include "LLCOptions.h"

static int run_sender(NSString *host, NSString *port, LLCStreamMode mode, BOOL scale, uint8_t codec, LlsProfileConfig profile)
{
    [NSApplication sharedApplication]; // ScreenCaptureKit needs a window server connection
    NSString *name = nil;
    uint16_t number = 0;
    LLCProblem *problem = LLCCheckAddress(host, port, &name, &number);
    if (!problem && (problem = LLCCheckScreenRecording()))
        problem.advice = @"Started from Terminal, macOS asks for the terminal app's permission, not LLCast's. Turn on "
                          "your terminal app in System Settings > Privacy & Security > Screen & System Audio "
                          "Recording and reopen it, or start LLCast with: open LLCast.app --args --send HOST";
    uint32_t rtt = 0, codecs = 0;
    uint8_t chosen = 0;
    if (!problem) problem = LLCProbeReceiver(name, number, &rtt, &codecs);
    if (!problem) problem = LLCCheckCodec(codec, [LLCEncoder hardwareCodecs], codecs, &chosen);
    if (problem) {
        fprintf(stderr, "%s\n%s\n", problem.headline.UTF8String, problem.advice.UTF8String);
        if (problem.detail) fprintf(stderr, "%s\n", problem.detail.UTF8String);
        return 1;
    }
    printf("Receiver answered in %.1f ms\n", rtt / 1000.0);
    static LLCSender *sender; // lives until exit
    sender = [[LLCSender alloc] initWithHost:name port:number mode:mode scale:scale codec:codec
                            receiverCodecs:codecs profile:profile];
    sender.onFailure = ^(NSError *error) {
        LLCProblem *why = LLCExplainSender(error);
        fprintf(stderr, "%s\n%s\n", why.headline.UTF8String, why.advice.UTF8String);
        exit(1);
    };
    [sender startWithCompletion:^(NSError *error) {
        if (error) {
            sender.onFailure(error);
            return;
        }
        printf("Streaming %ux%u at %u fps, %u bit/s budget (10%% FEC) · %s to %s port %u%s; Ctrl-C stops\n", mode.width,
               mode.height, mode.fps, mode.budget_bps, lls_codec_name(sender.codec), name.UTF8String, number,
               scale ? "" : ", top-left unscaled");
    }];
    signal(SIGINT, SIG_IGN);
    dispatch_source_t interrupt = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(interrupt, ^{
        [sender stop];
        exit(0);
    });
    dispatch_resume(interrupt);
    __block uint64_t frames = 0, bytes = 0;
    dispatch_source_t tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(tick, ^{
        const LLCSenderStatus s = [sender status];
        printf("%s · %llu fps, %.1f Mbps, receiver reply %s\n", lls_codec_name(s.codec), (unsigned long long)(s.frames - frames),
               (double)(s.bytes - bytes) * 8 / 1e6,
               s.rtt_us == UINT32_MAX ? "none" : [NSString stringWithFormat:@"%.1f ms", s.rtt_us / 1000.0].UTF8String);
        frames = s.frames;
        bytes = s.bytes;
    });
    dispatch_resume(tick);
    dispatch_main();
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        LLCOptions options;
        const char *error = LLCParseOptions(argc, argv, &options);
        if (error || options.help) {
            if (error) fprintf(stderr, "%s\n", error);
            fprintf(error ? stderr : stdout,
                "usage: LLCast [--send HOST [--port N] [--mode 720p|1080p] [--no-scale] | --selftest]\n"
                "  --send options: [--codec auto|av1|hevc|h264] (Auto: HEVC, then H.264)\n"
                "  [--profile [ADDR:]PORT | --profile-to HOST:PORT] [--profile-capacity N]\n"
                "Without --send: the window. Profiling starts with sending or receiving; off by default.\n");
            return error ? 2 : 0;
        }
        if (options.selftest) return LLCRunSelfTest();
        // --mode 720p is the Low profile (720p30), 1080p Medium (1080p60); the Auto budget.
        const LlsStreamProfile profile = lls_stream_profile(options.hd720 ? LLS_PROFILE_LOW : LLS_PROFILE_MEDIUM, NULL);
        if (options.host) return run_sender(@(options.host), @(options.port),
            LLCStreamModeFor(profile, lls_auto_budget_bps(&profile, options.codec)), options.scale, options.codec,
            options.profile);
        NSApplication *app = NSApplication.sharedApplication;
        static LLCAppDelegate *delegate; // NSApplication holds its delegate weakly
        delegate = [LLCAppDelegate new];
        delegate.profile = options.profile; // argv strings live for the process lifetime
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
