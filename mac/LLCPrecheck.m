#import "LLCPrecheck.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Security/Security.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#import "LLCDecoder.h"
#import "LLCEncoder.h"
#import "LLCSender.h"
#include "lls_net.h"
#include "lls_probe.h"

static NSString *const kScreenSettings =
    @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
static NSString *const kNetworkSettings =
    @"x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork";

@implementation LLCProblem
+ (instancetype)headline:(NSString *)headline advice:(NSString *)advice
{
    LLCProblem *problem = [self new];
    problem.headline = headline;
    problem.advice = advice;
    return problem;
}
@end

static LLCProblem *local_network(NSString *where, int error)
{
    LLCProblem *problem = [LLCProblem
        headline:@"LLCast can't reach the local network"
          advice:[NSString stringWithFormat:@"Nothing could be sent to %@. Allow LLCast in System Settings > "
                                             "Privacy & Security > Local Network, then try again. If it is "
                                             "allowed, check the address and that both devices are on the same "
                                             "network.", where]];
    problem.detail = [NSString stringWithFormat:@"socket error %d (%s)", error, strerror(error)];
    problem.settings = [NSURL URLWithString:kNetworkSettings];
    return problem;
}

void LLCSplitTarget(NSString *text, uint16_t defaultPort, NSString **host, NSString **port)
{
    NSString *trimmed = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSString *fallback = @(defaultPort).stringValue;
    if ([trimmed hasPrefix:@"["]) {
        const NSRange close = [trimmed rangeOfString:@"]"];
        if (close.location == NSNotFound) { *host = trimmed; *port = @""; return; }
        NSString *rest = [trimmed substringFromIndex:NSMaxRange(close)];
        *host = [trimmed substringWithRange:NSMakeRange(1, close.location - 1)];
        *port = !rest.length ? fallback : [rest hasPrefix:@":"] ? [rest substringFromIndex:1] : @"";
        return;
    }
    const NSRange colon = [trimmed rangeOfString:@":"];
    if (colon.location == NSNotFound ||
        colon.location != [trimmed rangeOfString:@":" options:NSBackwardsSearch].location) {
        *host = trimmed;
        *port = fallback;
        return;
    }
    *host = [trimmed substringToIndex:colon.location];
    *port = [trimmed substringFromIndex:colon.location + 1];
}

// socketfilterfw's answer, or nil. It needs no administrator rights to read.
static NSString *firewall(NSArray<NSString *> *arguments)
{
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/libexec/ApplicationFirewall/socketfilterfw"];
    task.arguments = arguments;
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;
    if (![task launchAndReturnError:nil]) return nil;
    NSData *output = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    return [[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding] lowercaseString];
}

LLCProblem *LLCCheckFirewall(void)
{
    // Off, or an answer we can't read: nothing to say. On with LLCast not yet
    // listed, macOS itself asks whether LLCast may accept incoming connections.
    if (![firewall(@[@"--getglobalstate"]) containsString:@"enabled"]) return nil;
    LLCProblem *problem = nil;
    if ([firewall(@[@"--getblockall"]) containsString:@"enabled"])
        problem = [LLCProblem headline:@"The macOS firewall blocks all incoming connections"
                                advice:@"LLCast can't receive while it does. In System Settings > Network > Firewall > "
                                        "Options, turn off \"Block all incoming connections\", then try again."];
    else if ([firewall(@[@"--getappblocked", NSBundle.mainBundle.bundlePath]) containsString:@"is blocked"])
        problem = [LLCProblem headline:@"The macOS firewall blocks LLCast"
                                advice:@"In System Settings > Network > Firewall > Options, set LLCast to \"Allow "
                                        "incoming connections\", then try again."];
    problem.settings = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.Network-Settings.extension"];
    return problem;
}

LLCProblem *LLCCheckAddress(NSString *host, NSString *port, NSString **hostOut, uint16_t *portOut)
{
    NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NSString *name = [host stringByTrimmingCharactersInSet:space];
    NSString *number = [port stringByTrimmingCharactersInSet:space];
    if (!name.length)
        return [LLCProblem headline:@"Enter the receiver's address"
                             advice:@"Type the address the receiver shows while it waits, for example 192.168.1.50."];
    NSScanner *scanner = [NSScanner scannerWithString:number];
    NSInteger value = 0;
    if (!number.length || number.length > 5 || ![scanner scanInteger:&value] || !scanner.atEnd || value < 1 ||
        value > 65535)
        return [LLCProblem headline:@"The address has an invalid port"
                             advice:@"Type the address as the receiver shows it, for example 192.168.1.50 or "
                                     "192.168.1.50:47801. A port is a number from 1 to 65535."];
    LlsAddress address;
    if (!lls_net_init()) return [LLCProblem headline:@"Networking isn't available" advice:@"Restart the Mac."];
    const bool found = lls_net_address(name.UTF8String, (uint16_t)value, AF_UNSPEC, &address);
    lls_net_cleanup();
    if (!found)
        return [LLCProblem headline:@"LLCast can't find the receiver"
                             advice:[NSString stringWithFormat:@"No device called \"%@\" was found. Type the numeric "
                                                                "address the receiver shows, for example "
                                                                "192.168.1.50.", name]];
    *hostOut = name;
    *portOut = (uint16_t)value;
    return nil;
}

// Ad hoc (or unsigned): the signature carries no certificate.
static BOOL signed_ad_hoc(void)
{
    SecStaticCodeRef code = NULL;
    if (SecStaticCodeCreateWithPath((__bridge CFURLRef)NSBundle.mainBundle.bundleURL, kSecCSDefaultFlags, &code))
        return NO;
    CFDictionaryRef info = NULL;
    BOOL adhoc = NO;
    if (!SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info)) {
        NSArray *certificates = ((__bridge NSDictionary *)info)[(__bridge NSString *)kSecCodeInfoCertificates];
        adhoc = certificates.count == 0;
        CFRelease(info);
    }
    CFRelease(code);
    return adhoc;
}

static LLCProblem *screen_recording(void)
{
    // Without a certificate, macOS ties the grant to this exact build.
    NSString *unsigned_note = signed_ad_hoc()
        ? @"\n\nThis copy of LLCast is signed ad hoc, so macOS treats every rebuild as a new copy."
        : @"";
    LLCProblem *problem = [LLCProblem
        headline:@"Allow LLCast to record the screen"
          advice:[@"Turn on LLCast in System Settings > Privacy & Security > Screen & System Audio Recording, "
                  "then choose Quit & Reopen.\n\nIf LLCast is already on there, that entry is for an earlier "
                  "copy of LLCast: select it, remove it with the − button, then Quit & Reopen and allow it "
                  "again." stringByAppendingString:unsigned_note]];
    problem.settings = [NSURL URLWithString:kScreenSettings];
    problem.relaunch = YES;
    return problem;
}

LLCProblem *LLCCheckScreenRecording(void)
{
    if (CGPreflightScreenCaptureAccess()) return nil;
    // The preflight answer can stay "no" for the rest of the process after a
    // grant, so ask ScreenCaptureKit itself; this also shows the first prompt.
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block BOOL allowed = NO;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent *content, NSError *error) {
            allowed = content && !error;
            dispatch_semaphore_signal(answered);
        }];
    if (!dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 5 * (int64_t)NSEC_PER_SEC)) && allowed)
        return nil;
    CGRequestScreenCaptureAccess(); // the system prompt, if ScreenCaptureKit showed none
    return screen_recording();
}

LLCProblem *LLCCheckEncoder(void)
{
    NSError *error = nil;
    if ([[LLCEncoder alloc] initWithWidth:1920 height:1080 bitrate:20000000 error:&error]) return nil;
    LLCProblem *problem = [LLCProblem headline:@"This Mac can't encode HEVC video in hardware"
                                        advice:@"LLCast needs a Mac with Apple silicon, or an Intel Mac from 2017 "
                                                "or later."];
    problem.detail = [NSString stringWithFormat:@"VideoToolbox error %ld", (long)error.code];
    return problem;
}

LLCProblem *LLCCheckDecoder(void)
{
    if ([LLCDecoder hardwareCodecs]) return nil;
    return [LLCProblem headline:@"This Mac has no supported hardware video decoder"
                         advice:@"Use a device with hardware HEVC, H.264 or AV1 decoding."];
}

static NSString *codec_list(uint32_t set)
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    const uint8_t order[] = {LLS_CODEC_HEVC, LLS_CODEC_H264, LLS_CODEC_AV1};
    for (size_t i = 0; i < sizeof(order); ++i)
        if (set & LLS_CODEC_BIT(order[i])) [names addObject:@(lls_codec_name(order[i]))];
    return names.count ? [names componentsJoinedByString:@", "] : @"nothing";
}

LLCProblem *LLCCheckCodec(uint8_t wanted, uint32_t here, uint32_t there, uint8_t *chosen)
{
    *chosen = lls_codec_pick(wanted, here, there);
    if (*chosen) return nil;
    if (!here)
        return [LLCProblem headline:@"This Mac can't encode video in hardware"
                             advice:@"LLCast needs a Mac with Apple silicon, or an Intel Mac from 2017 or later."];
    NSString *name = @(lls_codec_name(wanted));
    if (wanted && !(here & LLS_CODEC_BIT(wanted)))
        return [LLCProblem headline:[NSString stringWithFormat:@"This Mac can't encode %@ in hardware", name]
                             advice:[NSString stringWithFormat:@"It encodes %@. Choose Auto or one of those under "
                                                                "Codec in Settings.", codec_list(here)]];
    if (wanted)
        return [LLCProblem headline:[NSString stringWithFormat:@"The receiver can't decode %@", name]
                             advice:[NSString stringWithFormat:@"It decodes %@. Choose Auto or one of those under "
                                                                "Codec in Settings.", codec_list(there)]];
    return [LLCProblem headline:@"The receiver can't decode this Mac's video"
                         advice:[NSString stringWithFormat:@"This Mac encodes %@; the receiver decodes %@.",
                                                            codec_list(here), codec_list(there)]];
}

LLCProblem *LLCCheckReceiver(NSString *host, uint16_t port, uint32_t *rttUs)
{
    uint32_t codecs = 0;
    LLCProblem *problem = LLCProbeReceiver(host, port, rttUs, &codecs);
    if (!problem && !(codecs & LLS_CODEC_BIT(LLS_CODEC_HEVC)))
        problem = [LLCProblem headline:@"This receiver can't decode HEVC"
                                advice:@"The Mac sender uses HEVC. Choose a receiver with hardware HEVC decoding."];
    return problem;
}

LLCProblem *LLCProbeReceiver(NSString *host, uint16_t port, uint32_t *rttUs, uint32_t *codecs)
{
    NSString *where = [NSString stringWithFormat:@"%@, port %u", host, port];
    int error = 0;
    *codecs = 0;
    switch (lls_probe_receiver(host.UTF8String, port, rttUs, codecs, &error)) {
    case LLS_PROBE_ANSWERED:
        return nil;
    case LLS_PROBE_UNREACHABLE:
        // macOS reports a denied Local Network permission as no route to host.
        return local_network(where, error);
    case LLS_PROBE_REFUSED:
        return [LLCProblem headline:@"The receiver is on, but LLCast isn't receiving"
                             advice:[NSString stringWithFormat:@"The device at %@ answered, but LLCast isn't running "
                                                                "on it. Start LLCast there; it then shows its "
                                                                "address.", where]];
    case LLS_PROBE_FAILED: {
        LLCProblem *problem = [LLCProblem headline:@"LLCast can't open a network connection"
                                            advice:@"Check that this Mac is connected to the network."];
        problem.detail = [NSString stringWithFormat:@"socket error %d", error];
        return problem;
    }
    case LLS_PROBE_SILENT:
        break;
    }
    LLCProblem *problem = [LLCProblem
        headline:@"The receiver doesn't answer"
          advice:[NSString stringWithFormat:@"Nothing answered at %@. Check that the receiver shows the LLCast "
                                             "waiting screen, that the address and port match, and that both are on "
                                             "the same network. The first time, macOS may also ask to allow LLCast "
                                             "on the local network.", where]];
    problem.settings = [NSURL URLWithString:kNetworkSettings];
    return problem;
}

LLCProblem *LLCExplainBind(int error, uint16_t port)
{
    LLCProblem *problem;
    if (error == EADDRINUSE)
        problem = [LLCProblem headline:[NSString stringWithFormat:@"Port %u is already in use", port]
                                advice:@"Another receiver may be running. Quit it, or choose another port and "
                                        "use the same port on the sender."];
    else
        problem = [LLCProblem headline:@"LLCast can't start receiving"
                                advice:@"Check that this Mac is connected to the network."];
    problem.detail = [NSString stringWithFormat:@"socket error %d (%s)", error, strerror(error)];
    return problem;
}

LLCProblem *LLCExplainSender(NSError *error)
{
    if ([error.domain isEqualToString:LLCSenderErrorDomain]) {
        const int socket_error = [error.userInfo[LLCSocketErrorKey] intValue];
        if (error.code == LLCSenderErrorNetwork && (socket_error == EHOSTUNREACH || socket_error == ENETUNREACH))
            return local_network(@"the receiver", socket_error);
        if (error.code == LLCSenderErrorEncoder) return LLCCheckEncoder() ?: [LLCProblem
            headline:@"The video encoder stopped" advice:@"Press Start to try again."];
    }
    LLCProblem *problem;
    if ([error.domain isEqualToString:SCStreamErrorDomain] && error.code == SCStreamErrorUserDeclined) {
        problem = screen_recording();
    } else {
        problem = [LLCProblem headline:@"Streaming stopped" advice:@"Press Start to try again."];
    }
    problem.detail = [NSString stringWithFormat:@"%@ (%@ %ld)", error.localizedDescription, error.domain,
                                                (long)error.code];
    return problem;
}

NSArray<NSString *> *LLCLocalAddresses(void)
{
    NSMutableArray<NSString *> *preferred = [NSMutableArray array], *other = [NSMutableArray array];
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return @[];
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK))
            continue;
        char text[INET_ADDRSTRLEN];
        const struct sockaddr_in *address = (const struct sockaddr_in *)i->ifa_addr;
        if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;
        NSString *string = @(text);
        if ([string hasPrefix:@"169.254."]) continue; // link-local
        if (!strncmp(i->ifa_name, "en", 2)) [preferred addObject:string];
        else [other addObject:string];
    }
    freeifaddrs(list);
    return [preferred arrayByAddingObjectsFromArray:other];
}
