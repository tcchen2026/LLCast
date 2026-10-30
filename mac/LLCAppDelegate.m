#import "LLCAppDelegate.h"

#import <QuartzCore/QuartzCore.h>

#import "LLCDecoder.h"
#import "LLCEncoder.h"
#import "LLCPrecheck.h"
#import "LLCReceiver.h"
#import "LLCSender.h"
#import "LLCVideoView.h"
#include "llstream_proto.h"

typedef NS_ENUM(NSInteger, LLCState) { LLCIdle, LLCChecking, LLCRunning };

static NSTextField *label(NSString *text, NSRect frame, NSTextAlignment alignment)
{
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = frame;
    field.alignment = alignment;
    return field;
}

static NSTextField *input(NSRect frame, NSString *placeholder)
{
    NSTextField *field = [[NSTextField alloc] initWithFrame:frame];
    field.placeholderString = placeholder;
    field.usesSingleLineMode = YES;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
}

static NSString *LLCMbps(uint32_t bps)
{
    return bps % 1000000 ? [NSString stringWithFormat:@"%.1f", bps / 1e6] : [NSString stringWithFormat:@"%u", bps / 1000000];
}

// The saved profile and budget (user decision): Low, Medium or Custom, and Auto or typed.
static LlsStreamProfile LLCSavedProfile(void)
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    const LlsStreamProfile custom = {(uint32_t)[defaults integerForKey:@"customWidth"],
        (uint32_t)[defaults integerForKey:@"customHeight"], (uint32_t)[defaults integerForKey:@"customFps"]};
    return lls_stream_profile((int)[defaults integerForKey:@"profile"], &custom);
}

// A role card: a rounded fill drawn here, the whole card one click target with
// a matching focus ring, so the icon and name can be laid out freely inside.
@interface LLCCard : NSButton
@end

@implementation LLCCard
- (NSView *)hitTest:(NSPoint)point
{
    return NSPointInRect([self convertPoint:point fromView:self.superview], self.bounds) ? self : nil;
}
- (NSBezierPath *)shape
{
    return [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1) xRadius:12 yRadius:12];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(self.cell.isHighlighted ? NSColor.tertiarySystemFillColor : NSColor.quaternarySystemFillColor) setFill];
    [self.shape fill];
}
- (void)drawFocusRingMask { [self.shape fill]; }
- (NSRect)focusRingMaskBounds { return self.bounds; }
@end

@implementation LLCAppDelegate {
    NSWindow *_window;
    NSView *_chooser, *_role;
    NSButton *_sendCard, *_receiveCard, *_back;
    NSTextField *_roleTitle;
    NSInteger _mode; // 0: send, 1: receive
    NSView *_sendPane;
    NSTextField *_host, *_sendPort, *_status;
    NSButton *_start, *_noScale;
    NSPopUpButton *_receiveMode;
    NSWindow *_settings;
    NSTextField *_defaultPort, *_sendSummary;
    NSButton *_acceptHevc, *_acceptAvc, *_acceptAv1;
    NSPopUpButton *_sendCodec;
    NSPopUpButton *_streamProfile; // LLS_PROFILE_* order
    NSTextField *_customWidth, *_customHeight, *_customFps, *_budget;
    NSButton *_autoBudget;
    NSTimer *_timer;
    LLCState _state;
    LLCSender *_sender;
    LLCReceiver *_receiver;
    NSWindow *_screen;
    LLCVideoView *_video;
    NSTextField *_waiting;
    NSString *_target, *_modeLabel;
    uint16_t _port;
    uint64_t _lastFrames, _lastBytes;
    CFTimeInterval _lastTick;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    [self buildMenu];
    [self buildWindow];
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    // Before profiles, "quality" was 0 for 720p and 1 for 1080p.
    const BOOL was720 = [defaults objectForKey:@"quality"] && [defaults integerForKey:@"quality"] == 0;
    [defaults registerDefaults:@{@"port": @(LLS_DEFAULT_PORT), @"receivePort": @(LLS_DEFAULT_PORT),
                                  @"profile": @(was720 ? LLS_PROFILE_LOW : LLS_PROFILE_MEDIUM),
                                  @"customWidth": @1920, @"customHeight": @1080, @"customFps": @60,
                                  @"autoBudget": @YES, @"budgetKbps": @20000}];
    _host.stringValue = [defaults stringForKey:@"host"] ?: @"";
    const NSInteger sendPort = [defaults integerForKey:@"sendPort"];
    _sendPort.integerValue = sendPort >= 1 && sendPort <= 65535 ? sendPort : [self defaultPort];
    _mode = [defaults integerForKey:@"mode"] == 1 ? 1 : 0;
    [_receiveMode selectItemAtIndex:[defaults boolForKey:@"tearing"] ? 1 : 0];
    _noScale.state = [defaults boolForKey:@"noScale"] ? NSControlStateValueOn : NSControlStateValueOff;
    [self showChooser:nil];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)application
{
    (void)application;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
    (void)notification;
    [self stopStreaming];
}

- (void)buildMenu
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Settings…" action:@selector(openSettings:) keyEquivalent:@","].target = self;
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:@"Hide LLCast" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:@"Quit LLCast" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    // Edit, so that copy and paste work in the address field.
    NSMenuItem *editItem = [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""];
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;
    NSApp.mainMenu = bar;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 290)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"LLCast";
    _window.releasedWhenClosed = NO;
    NSView *window = _window.contentView;
    const NSRect whole = window.bounds;

    // First the choice of role, then that role's own screen.
    _chooser = [[NSView alloc] initWithFrame:whole];
    NSTextField *question = label(@"How do you want to use this Mac?", NSMakeRect(20, 236, 440, 24),
                                  NSTextAlignmentCenter);
    question.font = [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
    [_chooser addSubview:question];
    _sendCard = [self card:@"Send" symbol:@"airplayvideo" action:@selector(chooseSend:)
                     frame:NSMakeRect(40, 98, 190, 118)];
    _receiveCard = [self card:@"Receive" symbol:@"tv" action:@selector(chooseReceive:)
                        frame:NSMakeRect(250, 98, 190, 118)];
    for (NSButton *card in @[_sendCard, _receiveCard]) [_chooser addSubview:card];
    NSTextField *sendText = label(@"Show this Mac's screen on a TV box or another screen.",
                                  NSMakeRect(40, 50, 190, 44), NSTextAlignmentCenter);
    NSTextField *receiveText = label(@"Show a PC or another Mac on this screen.",
                                     NSMakeRect(250, 50, 190, 44), NSTextAlignmentCenter);
    for (NSTextField *text in @[sendText, receiveText]) {
        text.textColor = NSColor.secondaryLabelColor;
        text.lineBreakMode = NSLineBreakByWordWrapping;
        text.maximumNumberOfLines = 3;
        [_chooser addSubview:text];
    }
    NSButton *settings = [NSButton buttonWithTitle:@"Settings…" target:self action:@selector(openSettings:)];
    settings.frame = NSMakeRect(80, 12, 110, 30);
    [_chooser addSubview:settings];
    // Receiving has no page: Receive starts at once, in the mode chosen here.
    NSTextField *modeLabel = label(@"Receive mode:", NSMakeRect(244, 18, 92, 18), NSTextAlignmentRight);
    [_chooser addSubview:modeLabel];
    _receiveMode = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(338, 14, 122, 26) pullsDown:NO];
    [_receiveMode addItemsWithTitles:@[@"Normal", @"Lowest latency"]];
    _receiveMode.toolTip = @"Normal: each frame waits for the display's refresh.\n"
                            "Lowest latency: each frame appears as soon as it is decoded; the picture may tear.";
    _receiveMode.target = self;
    _receiveMode.action = @selector(receiveModeChanged:);
    [_chooser addSubview:_receiveMode];
    [window addSubview:_chooser];

    _role = [[NSView alloc] initWithFrame:whole];
    _back = [NSButton buttonWithTitle:@"‹ Back" target:self action:@selector(showChooser:)];
    _back.frame = NSMakeRect(12, 250, 84, 28);
    [_role addSubview:_back];
    _roleTitle = label(@"", NSMakeRect(100, 254, 280, 22), NSTextAlignmentCenter);
    _roleTitle.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    [_role addSubview:_roleTitle];
    [window addSubview:_role];
    NSView *content = _role;

    const NSRect pane = NSMakeRect(20, 100, 440, 134);
    _sendPane = [[NSView alloc] initWithFrame:pane];
    [_sendPane addSubview:label(@"Receiver address:", NSMakeRect(0, 104, 120, 20), NSTextAlignmentRight)];
    _host = input(NSMakeRect(126, 102, 196, 24), @"for example 192.168.1.50");
    [_sendPane addSubview:label(@"Port:", NSMakeRect(326, 104, 44, 20), NSTextAlignmentRight)];
    _sendPort = input(NSMakeRect(374, 102, 66, 24), @"47800");
    NSNumberFormatter *portNumber = [NSNumberFormatter new];
    portNumber.minimum = @1;
    portNumber.maximum = @65535;
    portNumber.allowsFloats = NO;
    portNumber.usesGroupingSeparator = NO;
    _sendPort.formatter = portNumber;
    [_sendPane addSubview:_sendPort];
    [_sendPane addSubview:_host];
    _sendSummary = label(@"", NSMakeRect(126, 40, 314, 20), NSTextAlignmentLeft);
    _sendSummary.textColor = NSColor.secondaryLabelColor;
    [_sendPane addSubview:_sendSummary];
    NSTextField *hint = label(@"The receiver shows its address and port while it waits.",
                              NSMakeRect(126, 78, 314, 18), NSTextAlignmentLeft);
    hint.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    hint.textColor = NSColor.secondaryLabelColor;
    [_sendPane addSubview:hint];
    [content addSubview:_sendPane];


    [self buildSettings];

    // Four small lines: streaming, reception (may wrap) and the doctor's hint.
    _status = label(@"Ready.", NSMakeRect(20, 44, 440, 56), NSTextAlignmentLeft);
    _status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByWordWrapping;
    _status.maximumNumberOfLines = 4;
    _status.selectable = YES;
    [content addSubview:_status];
    NSButton *copy = [NSButton buttonWithTitle:@"Copy" target:self action:@selector(copyStatus:)];
    copy.frame = NSMakeRect(14, 12, 80, 32);
    copy.toolTip = @"Copy the status, reception and hint, for a message or a report.";
    [content addSubview:copy];

    _start = [NSButton buttonWithTitle:@"Start" target:self action:@selector(startOrStop:)];
    _start.frame = NSMakeRect(370, 12, 96, 32);
    _start.keyEquivalent = @"\r";
    [content addSubview:_start];
}

// Settings, as a sheet on the main window: sending, receiving and the network.
- (void)buildSettings
{
    _settings = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 406) styleMask:NSWindowStyleMaskTitled
                                              backing:NSBackingStoreBuffered defer:YES];
    NSView *view = _settings.contentView;
    NSTextField *(^section)(NSString *, CGFloat) = ^(NSString *title, CGFloat y) {
        NSTextField *heading = label(title, NSMakeRect(20, y, 400, 20), NSTextAlignmentLeft);
        heading.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
        [view addSubview:heading];
        return heading;
    };
    NSTextField *(^note)(NSString *, NSRect) = ^(NSString *text, NSRect frame) {
        NSTextField *small = [NSTextField wrappingLabelWithString:text];
        small.frame = frame;
        small.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        small.textColor = NSColor.secondaryLabelColor;
        [view addSubview:small];
        return small;
    };

    // User decision, every sender: profile, codec, then the bandwidth budget.
    section(@"Sending", 370);
    [view addSubview:label(@"Profile:", NSMakeRect(20, 342, 100, 20), NSTextAlignmentRight)];
    _streamProfile = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(124, 338, 240, 26) pullsDown:NO];
    for (NSString *title in @[@"Low \u00b7 1280\u00d7720 \u00b7 30 fps", @"Medium \u00b7 1920\u00d71080 \u00b7 60 fps", @"Custom"])
        [_streamProfile addItemWithTitle:title];
    _streamProfile.target = self;
    _streamProfile.action = @selector(streamProfileChanged:);
    [view addSubview:_streamProfile];
    NSNumberFormatter *whole = [NSNumberFormatter new];
    whole.allowsFloats = NO;
    whole.usesGroupingSeparator = NO;
    whole.minimum = @0;
    _customWidth = input(NSMakeRect(126, 310, 54, 24), @"1920");
    _customHeight = input(NSMakeRect(198, 310, 54, 24), @"1080");
    _customFps = input(NSMakeRect(278, 310, 40, 24), @"60");
    for (NSTextField *field in @[_customWidth, _customHeight, _customFps]) {
        field.formatter = whole;
        field.delegate = self;
        [view addSubview:field];
    }
    [view addSubview:label(@"\u00d7", NSMakeRect(182, 312, 14, 20), NSTextAlignmentCenter)];
    [view addSubview:label(@"at", NSMakeRect(256, 312, 20, 20), NSTextAlignmentCenter)];
    [view addSubview:label(@"fps", NSMakeRect(322, 312, 30, 20), NSTextAlignmentLeft)];
    // Auto: HEVC, else H.264 (never AV1). Codecs this Mac can't encode are greyed out.
    [view addSubview:label(@"Codec:", NSMakeRect(20, 282, 100, 20), NSTextAlignmentRight)];
    _sendCodec = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(124, 278, 200, 26) pullsDown:NO];
    _sendCodec.autoenablesItems = NO;
    _sendCodec.target = self;
    _sendCodec.action = @selector(budgetInputsChanged:);
    const uint8_t codecs[] = {0, LLS_CODEC_AV1, LLS_CODEC_HEVC, LLS_CODEC_H264};
    for (int i = 0; i < 4; ++i) {
        [_sendCodec addItemWithTitle:codecs[i] ? @(lls_codec_name(codecs[i])) : @"Auto"];
        _sendCodec.lastItem.tag = codecs[i];
    }
    [view addSubview:_sendCodec];
    // Auto shows its value in the box, for the chosen codec (Auto counts as HEVC).
    [view addSubview:label(@"Bandwidth:", NSMakeRect(20, 252, 100, 20), NSTextAlignmentRight)];
    _autoBudget = [NSButton checkboxWithTitle:@"Auto" target:self action:@selector(budgetInputsChanged:)];
    _autoBudget.frame = NSMakeRect(126, 252, 60, 20);
    [view addSubview:_autoBudget];
    _budget = input(NSMakeRect(190, 250, 60, 24), @"20");
    [view addSubview:_budget];
    [view addSubview:label(@"Mbps", NSMakeRect(256, 252, 50, 20), NSTextAlignmentLeft)];
    _noScale = [NSButton checkboxWithTitle:@"Don't scale: send the top-left part at full size" target:nil action:nil];
    _noScale.frame = NSMakeRect(126, 222, 300, 20);
    [view addSubview:_noScale];

    section(@"Receiving", 186);
    [view addSubview:label(@"Accept:", NSMakeRect(20, 158, 100, 20), NSTextAlignmentRight)];
    _acceptHevc = [NSButton checkboxWithTitle:@"HEVC" target:nil action:nil];
    _acceptHevc.frame = NSMakeRect(126, 158, 80, 20);
    _acceptAvc = [NSButton checkboxWithTitle:@"H.264" target:nil action:nil];
    _acceptAvc.frame = NSMakeRect(210, 158, 80, 20);
    _acceptAv1 = [NSButton checkboxWithTitle:@"AV1" target:nil action:nil];
    _acceptAv1.frame = NSMakeRect(294, 158, 80, 20);
    [view addSubview:_acceptHevc];
    [view addSubview:_acceptAvc];
    [view addSubview:_acceptAv1];
    note(@"Auto uses HEVC or H.264; choose AV1 on the sender. Unavailable formats are off.",
         NSMakeRect(126, 120, 290, 32));

    section(@"Network", 90);
    [view addSubview:label(@"Default port:", NSMakeRect(20, 62, 100, 20), NSTextAlignmentRight)];
    _defaultPort = input(NSMakeRect(126, 60, 70, 24), @"47800");
    NSNumberFormatter *number = [NSNumberFormatter new];
    number.minimum = @1;
    number.maximum = @65535;
    number.allowsFloats = NO;
    number.usesGroupingSeparator = NO;
    _defaultPort.formatter = number;
    [view addSubview:_defaultPort];
    note(@"Receiving listens here, or on the next free port. Sending uses it when the address has no port.",
         NSMakeRect(204, 50, 216, 36));

    NSButton *done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(closeSettings:)];
    done.frame = NSMakeRect(336, 10, 90, 32);
    done.keyEquivalent = @"\r";
    [view addSubview:done];
}

- (void)openSettings:(id)sender
{
    (void)sender;
    if (_state != LLCIdle || _window.attachedSheet) return;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    const uint32_t hardware = [LLCDecoder hardwareCodecs];
    const NSInteger accepted = [defaults integerForKey:@"acceptCodecs"] ?: (NSInteger)hardware;
    NSArray *boxes = @[_acceptHevc, _acceptAvc, _acceptAv1];
    const uint8_t codecs[] = {LLS_CODEC_HEVC, LLS_CODEC_H264, LLS_CODEC_AV1};
    for (NSUInteger i = 0; i < boxes.count; ++i) {
        NSButton *box = boxes[i];
        const BOOL decodable = hardware & LLS_CODEC_BIT(codecs[i]);
        box.enabled = decodable;
        box.state = decodable && (accepted & LLS_CODEC_BIT(codecs[i])) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    _defaultPort.integerValue = [self defaultPort];
    const uint32_t encodable = [LLCEncoder hardwareCodecs];
    for (NSMenuItem *item in _sendCodec.itemArray) item.enabled = !item.tag || (encodable & LLS_CODEC_BIT(item.tag));
    [_sendCodec selectItemWithTag:[defaults integerForKey:@"sendCodec"]];
    const NSInteger profile = [defaults integerForKey:@"profile"];
    [_streamProfile selectItemAtIndex:profile >= 0 && profile < LLS_PROFILE_COUNT ? profile : LLS_PROFILE_MEDIUM];
    _autoBudget.state = [defaults boolForKey:@"autoBudget"] ? NSControlStateValueOn : NSControlStateValueOff;
    if (_autoBudget.state != NSControlStateValueOn)
        _budget.stringValue = LLCMbps((uint32_t)[defaults integerForKey:@"budgetKbps"] * 1000);
    [self showStreamProfile];
    [_window beginSheet:_settings completionHandler:nil];
}

- (LlsStreamProfile)chosenProfile
{
    const LlsStreamProfile custom = {(uint32_t)MAX(_customWidth.integerValue, 0), (uint32_t)MAX(_customHeight.integerValue, 0),
                                     (uint32_t)MAX(_customFps.integerValue, 0)};
    return lls_stream_profile((int)_streamProfile.indexOfSelectedItem, &custom);
}

- (void)showBudget
{
    const BOOL automatic = _autoBudget.state == NSControlStateValueOn;
    _budget.enabled = !automatic;
    if (!automatic) return;
    const LlsStreamProfile profile = [self chosenProfile];
    _budget.stringValue = lls_stream_profile_valid(&profile)
        ? LLCMbps(lls_auto_budget_bps(&profile, (uint8_t)_sendCodec.selectedTag)) : @"";
}

// Low and Medium show their size; only Custom can be edited.
- (void)showStreamProfile
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    const BOOL custom = _streamProfile.indexOfSelectedItem == LLS_PROFILE_CUSTOM;
    const LlsStreamProfile saved = {(uint32_t)[defaults integerForKey:@"customWidth"],
        (uint32_t)[defaults integerForKey:@"customHeight"], (uint32_t)[defaults integerForKey:@"customFps"]};
    const LlsStreamProfile shown = lls_stream_profile((int)_streamProfile.indexOfSelectedItem, &saved);
    _customWidth.integerValue = shown.width;
    _customHeight.integerValue = shown.height;
    _customFps.integerValue = shown.fps;
    _customWidth.enabled = _customHeight.enabled = _customFps.enabled = custom;
    [self showBudget];
}

- (void)streamProfileChanged:(id)sender
{
    (void)sender;
    [self showStreamProfile];
}

- (void)budgetInputsChanged:(id)sender
{
    (void)sender;
    [self showBudget];
}

- (void)controlTextDidChange:(NSNotification *)notification
{
    (void)notification; // the custom size and rate fields
    [self showBudget];
}

- (void)closeSettings:(id)sender
{
    (void)sender;
    const NSInteger port = _defaultPort.integerValue;
    const uint32_t accepted = (_acceptHevc.state == NSControlStateValueOn ? LLS_CODEC_BIT(LLS_CODEC_HEVC) : 0) |
                              (_acceptAvc.state == NSControlStateValueOn ? LLS_CODEC_BIT(LLS_CODEC_H264) : 0) |
                              (_acceptAv1.state == NSControlStateValueOn ? LLS_CODEC_BIT(LLS_CODEC_AV1) : 0);
    const LlsStreamProfile profile = [self chosenProfile];
    const BOOL automatic = _autoBudget.state == NSControlStateValueOn;
    uint32_t budget = 0;
    if (port < 1 || port > 65535 || !accepted || !lls_stream_profile_valid(&profile) ||
        (!automatic && !lls_budget_parse(_budget.stringValue.UTF8String, &budget))) {
        // A port from 1 to 65535, at least one format, an even custom size from 160 to 7680
        // at 1 to 240 fps, and a typed budget from 0.5 to 200 Mbps.
        NSBeep();
        return;
    }
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults setInteger:_streamProfile.indexOfSelectedItem forKey:@"profile"];
    if (_streamProfile.indexOfSelectedItem == LLS_PROFILE_CUSTOM) {
        [defaults setInteger:profile.width forKey:@"customWidth"];
        [defaults setInteger:profile.height forKey:@"customHeight"];
        [defaults setInteger:profile.fps forKey:@"customFps"];
    }
    [defaults setBool:automatic forKey:@"autoBudget"];
    if (!automatic) [defaults setInteger:budget / 1000 forKey:@"budgetKbps"];
    [defaults setInteger:port forKey:@"port"];
    [defaults setInteger:accepted forKey:@"acceptCodecs"];
    [defaults setInteger:_sendCodec.selectedTag forKey:@"sendCodec"];
    [self saveSettings];
    [self updateSendSummary];
    [_window endSheet:_settings];
}

- (NSInteger)defaultPort
{
    const NSInteger port = [NSUserDefaults.standardUserDefaults integerForKey:@"port"];
    return port >= 1 && port <= 65535 ? port : LLS_DEFAULT_PORT;
}

- (void)updateSendSummary
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    const uint8_t codec = (uint8_t)[defaults integerForKey:@"sendCodec"];
    const LlsStreamProfile profile = LLCSavedProfile();
    _sendSummary.stringValue = [NSString stringWithFormat:@"%u\u00d7%u at %u fps, %@, %@, %@. Change in Settings…",
        profile.width, profile.height, profile.fps,
        [defaults boolForKey:@"autoBudget"] ? [NSString stringWithFormat:@"auto budget (%@ Mbps)",
            LLCMbps(lls_auto_budget_bps(&profile, codec))]
            : [NSString stringWithFormat:@"%@ Mbps budget", LLCMbps((uint32_t)[defaults integerForKey:@"budgetKbps"] * 1000)],
        codec ? @(lls_codec_name(codec)) : @"Auto codec",
        _noScale.state == NSControlStateValueOn ? @"top-left part unscaled" : @"whole screen"];
}

- (NSButton *)card:(NSString *)title symbol:(NSString *)symbol action:(SEL)action frame:(NSRect)frame
{
    NSImage *image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    image = [image imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:44
                                                                                                weight:NSFontWeightRegular]];
    LLCCard *card = [[LLCCard alloc] initWithFrame:frame];
    card.title = @"";
    card.bordered = NO;
    card.target = self;
    card.action = action;
    card.accessibilityLabel = title;
    // The symbol and its name, centred together; AppKit's image-above layout
    // places a large symbol unpredictably in a tall square bezel.
    NSImageView *icon = [NSImageView imageViewWithImage:image];
    icon.contentTintColor = NSColor.controlAccentColor;
    NSTextField *name = [NSTextField labelWithString:title];
    name.font = [NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
    NSStackView *stack = [NSStackView stackViewWithViews:@[icon, name]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:card.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
    ]];
    return card;
}

- (void)chooseSend:(id)sender
{
    (void)sender;
    [self showRole:0];
}

// Receiving has nothing to fill in: it starts at once, and the waiting screen
// shows what to type on the sender.
- (void)chooseReceive:(id)sender
{
    (void)sender;
    if (_state != LLCIdle) return;
    _mode = 1;
    [NSUserDefaults.standardUserDefaults setInteger:1 forKey:@"mode"];
    [self startReceiving];
}

- (void)receiveModeChanged:(id)sender
{
    (void)sender;
    [NSUserDefaults.standardUserDefaults setBool:_receiveMode.indexOfSelectedItem == 1 forKey:@"tearing"];
}

- (void)showChooser:(id)sender
{
    (void)sender;
    if (_state != LLCIdle) return;
    _role.hidden = YES;
    _chooser.hidden = NO;
    _window.title = @"LLCast";
    // Return picks the role used last.
    _sendCard.keyEquivalent = _mode == 0 ? @"\r" : @"";
    _receiveCard.keyEquivalent = _mode == 1 ? @"\r" : @"";
    [_window makeFirstResponder:_mode == 0 ? _sendCard : _receiveCard];
}

// Only sending has a page: the receiver's address and quality, then Start.
- (void)showRole:(NSInteger)mode
{
    (void)mode;
    _mode = 0;
    [NSUserDefaults.standardUserDefaults setInteger:0 forKey:@"mode"];
    _chooser.hidden = YES;
    _role.hidden = NO;
    _roleTitle.stringValue = @"Send this Mac's screen";
    _window.title = @"LLCast: Send";
    _status.stringValue = @"Ready.";
    [self updateSendSummary];
    [_window makeFirstResponder:_host.stringValue.length ? (NSView *)_start : _host];
}

- (void)setState:(LLCState)state status:(NSString *)status
{
    _state = state;
    _status.stringValue = status;
    _start.title = state == LLCRunning ? @"Stop" : @"Start";
    _start.enabled = state != LLCChecking;
    _back.enabled = state == LLCIdle;
    _host.enabled = _sendPort.enabled = state == LLCIdle;
    _noScale.enabled = state == LLCIdle;
}

- (void)show:(LLCProblem *)problem
{
    NSAlert *alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = problem.headline;
    alert.informativeText = problem.detail ? [NSString stringWithFormat:@"%@\n\n%@", problem.advice, problem.detail]
                                           : problem.advice;
    [alert addButtonWithTitle:@"OK"];
    if (problem.settings) [alert addButtonWithTitle:@"Open System Settings"];
    if (problem.relaunch) [alert addButtonWithTitle:@"Quit & Reopen"];
    NSURL *settings = problem.settings;
    const NSModalResponse relaunch = NSAlertSecondButtonReturn + (settings ? 1 : 0);
    [alert beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        if (response == NSAlertSecondButtonReturn && settings) [NSWorkspace.sharedWorkspace openURL:settings];
        else if (response == relaunch && problem.relaunch) [self relaunch];
    }];
}

// A new process sees permissions granted since this one started.
- (void)relaunch
{
    [self saveSettings];
    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.createsNewApplicationInstance = YES;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:NSBundle.mainBundle.bundleURL configuration:configuration
        completionHandler:^(NSRunningApplication *app, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (app) [NSApp terminate:nil];
                else [self show:[LLCProblem headline:@"LLCast couldn't reopen itself"
                                              advice:[NSString stringWithFormat:@"Quit LLCast and open it again. (%@)",
                                                                                error.localizedDescription]]];
            });
        }];
}

- (void)copyStatus:(id)sender
{
    (void)sender;
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    [board clearContents];
    [board setString:_status.stringValue forType:NSPasteboardTypeString];
}

- (void)saveSettings
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults setObject:_host.stringValue forKey:@"host"];
    [defaults setInteger:_sendPort.integerValue forKey:@"sendPort"];
    [defaults setInteger:_mode forKey:@"mode"];
    [defaults setBool:_noScale.state == NSControlStateValueOn forKey:@"noScale"];
}

- (void)startOrStop:(id)sender
{
    (void)sender;
    if (_state == LLCRunning) {
        [self stopStreaming];
        [self setState:LLCIdle status:@"Stopped."];
        return;
    }
    if (_state != LLCIdle) return;
    [self saveSettings];
    [self startSending];
}

#pragma mark Sending

- (void)startSending
{
    NSString *host = nil;
    uint16_t port = 0;
    // "address" or "address:port", as receivers show it.
    NSString *typed = nil, *typedPort = nil;
    // The Port box, unless the address carries its own ":port".
    const NSInteger boxPort = _sendPort.integerValue;
    LLCSplitTarget(_host.stringValue, (uint16_t)(boxPort >= 1 && boxPort <= 65535 ? boxPort : [self defaultPort]),
                   &typed, &typedPort);
    LLCProblem *problem = LLCCheckAddress(typed, typedPort, &host, &port);
    if (problem) {
        [self show:problem];
        return;
    }
    [self setState:LLCChecking status:@"Checking this Mac and the receiver…"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint32_t rtt = 0, there = 0;
        uint8_t chosen = 0;
        const uint8_t wanted = (uint8_t)[NSUserDefaults.standardUserDefaults integerForKey:@"sendCodec"];
        LLCProblem *found = LLCCheckScreenRecording();
        if (!found) found = LLCProbeReceiver(host, port, &rtt, &there);
        if (!found) found = LLCCheckCodec(wanted, [LLCEncoder hardwareCodecs], there, &chosen);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (found) {
                [self setState:LLCIdle status:[found.headline stringByAppendingString:@"."]];
                [self show:found];
                return;
            }
            [self beginSendingTo:host port:port codec:wanted chosen:chosen receiverCodecs:there];
        });
    });
}

// chosen: the codec the prechecks agreed on, for the Auto budget.
- (void)beginSendingTo:(NSString *)host port:(uint16_t)port codec:(uint8_t)codec chosen:(uint8_t)chosen
        receiverCodecs:(uint32_t)there
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    const LlsStreamProfile profile = LLCSavedProfile();
    const BOOL automatic = [defaults boolForKey:@"autoBudget"];
    const LLCStreamMode mode = LLCStreamModeFor(profile, automatic ? lls_auto_budget_bps(&profile, chosen)
                                                                   : (uint32_t)[defaults integerForKey:@"budgetKbps"] * 1000);
    LLCSender *sender = [[LLCSender alloc] initWithHost:host port:port mode:mode
                                                  scale:_noScale.state != NSControlStateValueOn codec:codec
                                         receiverCodecs:there profile:self.profile];
    NSString *modeLabel = [NSString stringWithFormat:@"%u\u00d7%u \u00b7 %u fps \u00b7 %@ Mbps", mode.width, mode.height,
                           mode.fps, LLCMbps(mode.budget_bps)];
    _modeLabel = modeLabel;
    __weak LLCAppDelegate *weakSelf = self;
    sender.onFailure = ^(NSError *error) { [weakSelf streamFailed:LLCExplainSender(error)]; };
    _sender = sender;
    _target = host;
    [sender startWithCompletion:^(NSError *error) {
        LLCAppDelegate *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sender != sender) return;
        if (error) {
            [strongSelf stopStreaming];
            [strongSelf streamFailed:LLCExplainSender(error)];
            return;
        }
        strongSelf->_modeLabel = [NSString stringWithFormat:@"%@ · %s", modeLabel, lls_codec_name(sender.codec)];
        [strongSelf setState:LLCRunning
                      status:[NSString stringWithFormat:@"Starting the stream to %@…", host]];
        [strongSelf startStatusTimer];
    }];
}

- (void)streamFailed:(LLCProblem *)problem
{
    [self stopStreaming];
    [self setState:LLCIdle status:[problem.headline stringByAppendingString:@"."]];
    [self show:problem];
}

#pragma mark Receiving

- (void)startReceiving
{
    // The default port, as on every receiver; a "receivePort" default overrides it.
    NSInteger port = [self defaultPort];
    LLCProblem *problem = LLCCheckDecoder();
    if (!problem) problem = LLCCheckFirewall();
    if (problem) {
        [self show:problem];
        return;
    }
    LLCVideoView *video = [LLCVideoView viewWithFrame:NSMakeRect(0, 0, 1280, 720)];
    if (!video) {
        [self show:[LLCProblem headline:@"This Mac's graphics can't show the stream"
                                 advice:@"LLCast needs Metal, available on Macs from 2012 or later."]];
        return;
    }
    video.allowTearing = [NSUserDefaults.standardUserDefaults boolForKey:@"tearing"];
    // The usual port, or the next free one of the following nine; the waiting screen says which.
    const NSInteger preferred = port;
    LLCReceiver *receiver = nil;
    int error = 0;
    for (; port < preferred + 10 && port <= 65535; ++port) {
        receiver = [[LLCReceiver alloc] initWithPort:(uint16_t)port view:video profile:self.profile];
        if ([receiver start:&error]) break;
        receiver = nil;
        if (error != EADDRINUSE) break;
    }
    if (!receiver) {
        [video invalidate];
        [self show:LLCExplainBind(error, (uint16_t)preferred)];
        return;
    }
    _video = video;
    _receiver = receiver;
    _port = (uint16_t)port;
    __weak LLCAppDelegate *weakSelf = self;
    video.onEscape = ^{
        LLCAppDelegate *strongSelf = weakSelf;
        [strongSelf stopStreaming];
        [strongSelf setState:LLCIdle status:@"Stopped."];
        [strongSelf showChooser:nil];
    };

    NSString *address = LLCLocalAddresses().firstObject ?: @"this Mac";
    NSString *target = port == LLS_DEFAULT_PORT ? address : [NSString stringWithFormat:@"%@:%ld", address, (long)port];
    NSString *moved = port == preferred ? @"" :
        [NSString stringWithFormat:@"\n\nPort %ld is in use on this Mac, so LLCast uses port %ld.",
                                   (long)preferred, (long)port];
    _waiting = [NSTextField wrappingLabelWithString:
        [NSString stringWithFormat:@"Waiting for a sender\n\nOn the sender, enter %@%@\n\nPress Esc to stop", target,
                                   moved]];
    _waiting.alignment = NSTextAlignmentCenter;
    _waiting.textColor = NSColor.whiteColor;
    _waiting.font = [NSFont systemFontOfSize:28 weight:NSFontWeightMedium];
    _waiting.frame = NSMakeRect(0, 230, 1280, 260);
    _waiting.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin;
    [video addSubview:_waiting];

    _screen = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _screen.title = @"LLCast";
    _screen.releasedWhenClosed = NO;
    _screen.backgroundColor = NSColor.blackColor;
    _screen.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _screen.contentView = video;
    _screen.delegate = self;
    [_screen center];
    [_screen makeKeyAndOrderFront:nil];
    [_screen makeFirstResponder:video];
    [_screen toggleFullScreen:nil];
    [self setState:LLCRunning status:[NSString stringWithFormat:@"Waiting for a sender on port %ld.", (long)port]];
    [self startStatusTimer];
}

- (void)windowWillClose:(NSNotification *)notification
{
    if (notification.object == _screen && _state == LLCRunning) {
        [self stopStreaming];
        [self setState:LLCIdle status:@"Stopped."];
        if (_mode == 1) [self showChooser:nil];
    }
}

#pragma mark Common

- (void)startStatusTimer
{
    _lastFrames = _lastBytes = 0;
    _lastTick = CACurrentMediaTime();
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(updateStatus:)
                                            userInfo:nil repeats:YES];
}

- (void)updateStatus:(NSTimer *)timer
{
    (void)timer;
    const CFTimeInterval now = CACurrentMediaTime(), seconds = now - _lastTick;
    if (seconds <= 0) return;
    if (_sender) {
        const LLCSenderStatus s = [_sender status];
        NSString *reply = s.rtt_us == UINT32_MAX ? @"The receiver isn't answering."
            : [NSString stringWithFormat:@"The receiver answers in %u ms.", (s.rtt_us + 500) / 1000];
        if (s.rtt_us != UINT32_MAX && s.quality.level) {
            // The receiver's view of the last 5 s; its round trip is part of the latency.
            char line[512];
            lls_quality_describe(&s.quality, line, sizeof(line));
            reply = @(line);
        }
        _status.stringValue = [NSString stringWithFormat:@"Streaming %@ to %@ · %.0f fps · %.1f Mbps\n%@",
                               _modeLabel, _target,
                               (double)(s.frames - _lastFrames) / seconds,
                               (double)(s.bytes - _lastBytes) * 8 / seconds / 1e6, reply];
        _lastFrames = s.frames;
        _lastBytes = s.bytes;
    } else if (_receiver) {
        const LlsRxStatus s = [_receiver status];
        const uint64_t shown = _receiver.shown;
        _waiting.hidden = s.receiving && !s.unsupported_codec;
        if (s.unsupported_codec) {
            _waiting.stringValue = [NSString stringWithFormat:@"This sender uses %s, which this device can't decode.\nPress Esc to stop.", lls_codec_name(s.unsupported_codec)];
            _status.stringValue = _waiting.stringValue;
        } else if (s.receiving) {
            [NSCursor setHiddenUntilMouseMoves:YES];
            _status.stringValue = [NSString stringWithFormat:@"Receiving from %s · %s · %.0f fps shown · %.1f Mbps\n"
                                                              "%llu frames lost so far.",
                                   s.peer, lls_codec_name(_receiver.codec), (double)(shown - _lastFrames) / seconds,
                                   (double)(s.bytes - _lastBytes) * 8 / seconds / 1e6,
                                   (unsigned long long)s.lost];
        } else {
            _status.stringValue = [NSString stringWithFormat:@"Waiting for a sender on port %u.", _port];
            _waiting.stringValue = [NSString stringWithFormat:@"Waiting for a sender\n%@, port %u\nPress Esc to stop.", LLCLocalAddresses().firstObject ?: @"this Mac", _port];
        }
        _lastFrames = shown;
        _lastBytes = s.bytes;
    }
    _lastTick = now;
}

- (void)stopStreaming
{
    [_timer invalidate];
    _timer = nil;
    if (_sender) [_sender stop];
    _sender = nil;
    if (_receiver) [_receiver stop];
    _receiver = nil;
    if (_screen) {
        _screen.delegate = nil;
        [_screen close];
        _screen = nil;
        [_window makeKeyAndOrderFront:nil];
    }
    [_video invalidate];
    _video = nil;
    _waiting = nil;
}

@end
