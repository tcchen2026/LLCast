// Checks before streaming, each explained in plain words, as on Windows.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface LLCProblem : NSObject
@property(nonatomic, copy) NSString *headline; // what is wrong
@property(nonatomic, copy) NSString *advice;   // what to do about it
@property(nonatomic, copy, nullable) NSString *detail;
@property(nonatomic, nullable) NSURL *settings; // the System Settings pane that fixes it
@property(nonatomic) BOOL relaunch;              // the fix takes effect in a new LLCast process
+ (instancetype)headline:(NSString *)headline advice:(NSString *)advice;
@end

// Splits "address" or "address:port" ("[v6]:port" for IPv6) as receivers show it.
// Without a port, fallback; a bare IPv6 address has no port.
void LLCSplitTarget(NSString *text, uint16_t fallback, NSString *_Nonnull *_Nonnull host,
                    NSString *_Nonnull *_Nonnull port);
// The macOS firewall: a problem only if it blocks LLCast or all incoming connections.
LLCProblem *_Nullable LLCCheckFirewall(void);
// Parses the typed address and port; host names are resolved.
LLCProblem *_Nullable LLCCheckAddress(NSString *host, NSString *port, NSString *_Nullable *_Nonnull hostOut,
                                      uint16_t *portOut);
// Screen Recording permission; asks for it the first time. Blocks for up to
// a few seconds: off the main thread of a running app.
LLCProblem *_Nullable LLCCheckScreenRecording(void);
LLCProblem *_Nullable LLCCheckEncoder(void);
LLCProblem *_Nullable LLCCheckDecoder(void);
// Blocks for up to about a second. Off the main thread.
LLCProblem *_Nullable LLCCheckReceiver(NSString *host, uint16_t port, uint32_t *rttUs);
// The same probe without a codec requirement: *codecs is the receiver's LLS_CODEC_BIT set.
LLCProblem *_Nullable LLCProbeReceiver(NSString *host, uint16_t port, uint32_t *rttUs, uint32_t *codecs);
// The send codec for the user's choice (0 = Auto: HEVC, else H.264; never AV1), from what this
// Mac encodes and the receiver decodes (lls_codec_pick). On success *chosen is set.
LLCProblem *_Nullable LLCCheckCodec(uint8_t wanted, uint32_t here, uint32_t there, uint8_t *chosen);
LLCProblem *LLCExplainBind(int error, uint16_t port);
LLCProblem *LLCExplainSender(NSError *error);
// This Mac's IPv4 addresses on active interfaces, Wi-Fi/Ethernet first.
NSArray<NSString *> *LLCLocalAddresses(void);

NS_ASSUME_NONNULL_END
