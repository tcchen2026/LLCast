// The LLCast window: send this Mac's screen, or receive a stream full screen.
#import <AppKit/AppKit.h>
#include "lls_profile.h"

@interface LLCAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate>
// Set once from main's argv; not a preference or UI control.
@property(nonatomic) LlsProfileConfig profile;
@end
