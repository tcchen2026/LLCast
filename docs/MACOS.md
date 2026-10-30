# LLCast for macOS

One app, `LLCast.app` in a signed, notarized DMG, with two roles. It shares the
wire protocol, the portable C core (`src/common/`, `sender/lls_sender.c`) and
the loss rules with the PC and the box. Sources from the API research are at
the end.

## Build and run on a Mac

Requirements: macOS 14+, Xcode command line tools (`xcode-select --install`)
and CMake (`brew install cmake`).

```sh
tools/macos/build.sh --test                       # build/macos/dist/LLCast.app (ad hoc), then the self-test
tools/macos/build.sh --build-dmg                  # Developer ID: signed, notarized LLCast.dmg (CI releases)
build/macos/dist/LLCast.app/Contents/MacOS/LLCast --selftest
open build/macos/dist/LLCast.app                  # the window
build/macos/dist/LLCast.app/Contents/MacOS/LLCast --send 192.168.5.119   # Terminal, Ctrl-C stops
open build/macos/dist/LLCast.app --args --profile 47801              # window, opt-in profiles
```

- **Self-test.** It runs the whole path except capture and display, and
  needs no permissions: synthetic NV12, hardware HEVC, UDP loopback, the
  receiver session, hardware decode, then a luma comparison. It prints
  encode, decode and encode-to-decoded medians. It exits 77 where there is
  no hardware HEVC, as on CI virtual machines.
  It also checks AV1 format/sample conversion and, when AV1 hardware is
  available, decodes two embedded key/inter frames and checks their NV12 pixels.
- **Permissions:**
  - A grant reaches a running LLCast only after it reopens. The alert offers
    Quit & Reopen. Its check also asks ScreenCaptureKit, because
    `CGPreflightScreenCaptureAccess()` can keep saying no.
  - **Signing and grants.** `build.sh` signs ad hoc and never touches the
    keychain unless given `--build-dmg`. An ad-hoc grant belongs to that exact
    build: after a rebuild, remove the entry with − (or run
    `tccutil reset ScreenCapture io.github.tcchen2026.llcast`) and allow it
    again. A certificate-signed LLCast keeps its grants across rebuilds.
  - Sending works when signed with Apple Development. A build left
    half-signed by a refused keychain prompt failed, so `build.sh` now removes
    the app when codesign fails. Whether a clean ad-hoc build can send is
    untested.
  - Started from Terminal (`--send`), macOS checks the terminal app's
    permissions, not LLCast's: Screen Recording must be allowed for the
    terminal app. Local Network doesn't apply there, which is why `--send`
    works there first. `open LLCast.app --args --send HOST` uses LLCast's
    own permissions.
- **Signed DMG.** `build.sh --build-dmg`, with a Developer ID Application
  identity in the keychain and `ASC_KEY_ID`, `ASC_ISSUER_ID` and
  `ASC_KEY_PATH` set, also makes `LLCast.dmg`, then notarizes and staples it.
  `.github/workflows/macos.yml` does the same from the `release` environment's
  secrets, by hand or through `release.yml`. Its release download is named
  `LLCast-macOS.dmg`; without a Developer ID secret, CI keeps only an ad-hoc
  app zip artifact.

## Files

| File | Role |
| --- | --- |
| `mac/main.m` | Window, `--send`, `--selftest` |
| `mac/LLCAppDelegate.m` | The window, prechecks off the main thread, alerts, the full-screen receive window |
| `mac/LLCPrecheck.m` | Plain-language checks and explanations |
| `mac/LLCSender.m` | ScreenCaptureKit, frame policy, encoder, `lls_sender` |
| `mac/LLCReceiver.m` | `lls_rx_session`, decode thread, frame reports after display |
| `mac/LLCVideoView.m` | Metal NV12 view: latest wins, one present pending, optional tearing |
| `mac/LLCEncoder.m`, `mac/LLCDecoder.m` | VideoToolbox HEVC and H.264 (decode also AV1 where the hardware has it) |
| `mac/LLCHevc.c` | Annex-B and length-prefixed conversion, format descriptions |
| `src/common/lls_rx_session.c` | Portable receiver network half; tested on Linux (`tests/test_rx_session.c`) |
| `sender/lls_probe.c` | Receiver PING check, shared with Windows |

## Why Objective-C

ScreenCaptureKit, AppKit and Metal have no C API.
VideoToolbox, CoreMedia and CoreVideo are C. Objective-C (ARC) is compiled by
the same clang as the core and includes its headers directly. It needs no
bridging header and builds with one CMake project, so Swift adds nothing here.
The app code lives in `mac/`. Platform-neutral logic stays in C.

## Requirements

- macOS 14 or later, as a universal binary. Version 14 brings
  ScreenCaptureKit's `captureResolution` and Game Mode.
- Apple silicon for the best path. HEVC low-latency rate control exists only
  there; Intel Macs fall back to ordinary real-time HEVC encoding.

## Sender: this Mac's screen to a box (or another Mac)

```text
SCStream (420v, BT.709, profile size) -> VTCompressionSession HEVC or H.264 -> Annex-B -> lls_sender
```

- **Capture.** ScreenCaptureKit scales and converts on the GPU inside the
  window server, so there is no conversion pass of our own.
  - Output: 420v at the mode's size, `colorMatrix` 709, colour space ITU-R 709,
    `minimumFrameInterval` one frame at the profile's rate, `queueDepth` 3. Surfaces are returned at
    once.
  - **Geometry:** as on Windows (docs/PLAN.md, Decisions). By default the
    whole display is fitted by ScreenCaptureKit, aspect kept.
  - **No scale:** takes the top-left pixels, with `sourceRect` in points
    (mode size / backing scale). A display smaller than the mode is still
    sent whole, scaled.
- **Frame policy.** ScreenCaptureKit only delivers changes; Idle frames carry
  no surface. The sender keeps the last surface and repeats it on the
  profile's frame grid using `src/common/lls_frame_policy.h`, shared with Windows. An update
  encodes at period−2 ms; the first repeat is at period+1 ms, then the nominal
  period. A one-shot timer waits for that deadline. Captures wake the serial
  queue, and RECOVER uses a coalesced dispatch source to force an IDR at once.
  There is no millisecond polling timer.
- **Profiling.** `--profile [ADDR:]PORT` or `--profile-to HOST:PORT` works with
  `--send` or with the window's Send/Receive sessions. Off by default; never
  saved in preferences. Optional `--profile-capacity N` changes ring capacity.
  [PROFILING.md](PROFILING.md) describes collection and the Mac timestamp fields.
- **Encoder:**
  - VTCompressionSession HEVC Main or H.264 Main, as the codec check chose,
    with `EnableLowLatencyRateControl`, falling back without it.
  - `RealTime`, `AllowFrameReordering=false`, `MaxFrameDelayCount=0`,
    `PrioritizeEncodingSpeedOverQuality`, `ExpectedFrameRate` at the profile's rate, and
    `AverageBitRate` plus `DataRateLimits` over one frame.
  - A very large keyframe interval, `ForceKeyFrame` on recovery, and
    BT.709 VUI keys. Parse the SPS to confirm the VUI.
  - Output is length-prefixed. Convert it to Annex-B, and prepend the
    parameter sets from the format description on every IDR.
- **Threads.** Capture callback, encode and send run on one serial queue at
  user-interactive QoS. Encoding is completed synchronously, so there is one
  sending thread, as `lls_sender` requires. Socket service type is
  `NET_SERVICE_TYPE_VI` (Wi-Fi WMM video).
- **Timestamps.** `SCStreamFrameInfo.displayTime` is mach absolute time;
  convert it to the sender clock and reject values in the future.

## Receiver: a PC or Mac stream on this Mac's screen

```text
UDP -> lls receiver core (assembly, FEC, feedback) -> VTDecompressionSession -> CAMetalLayer
```

- **Network and feedback.** This is the box receiver's rx logic (PONG, frame
  reports, RECOVER, FEC, bounded assembly), moved out of `llstream_rx.c` into
  a portable C module that every receiver uses. macOS has no
  `recvmmsg`, so it loops over `recvmsg` with `SO_TIMESTAMP`.
- **Decode.** Synchronous VTDecompressionSession, hardware required; the
  picture arrives before the decode call returns.
  HEVC, H.264 and AV1 are supported. For AV1 on an M3 or newer Mac, enable
  **Settings → Receiving → Accept → AV1** and select **AV1** on the sender;
  Auto still chooses HEVC, then H.264. Apple’s supplemental AV1 decoder is
  registered before querying support. AV1 is disabled when VideoToolbox
  reports no hardware decoder; the Mac has no software fallback.
- **Losses: a platform difference.** VideoToolbox returns no picture for a
  frame whose reference is missing; no concealed frame comes back. So the
  Mac keeps the last good picture on screen and asks for recovery at once,
  instead of showing damaged frames like the box. FEC matters more here. The
  session is recreated on `kVTInvalidSessionErr` or a malfunction.
- **Display.**
  - Zero-copy: decoded IOSurface, then `CVMetalTextureCache` (R8 and RG8
    planes), then a BT.709 limited-range shader, then a CAMetalLayer.
  - Full screen, opaque, `maximumDrawableCount` 3. These are the conditions
    for direct-to-display (no compositor pass) on Apple silicon.
  - "Latest wins": only the newest decoded picture is drawn.
  - One present is in flight at a time; one unconfirmed after 250 ms counts
    as done, so a hidden window can't stall the stream.
  - The chooser's receive option, Normal or Lowest latency, sets
    `displaySyncEnabled = NO` for Lowest latency.
- **Receive has no page.** The Receive card starts receiving straight away, in
  the mode chosen under it, on
  UDP 47800 (`defaults write io.github.tcchen2026.llcast receivePort -int N`
  overrides it). If that port is taken, it uses the next free one of the
  following nine, and the waiting screen says so. The waiting screen shows
  exactly what to type on the sender: the address, plus ":port" only when it
  isn't 47800. Esc returns to the chooser. Android's Normal mode does the same.
- **Chooser:** Send and Receive cards. Under Send, **Settings…** (also ⌘,);
  under Receive, **Receive mode** (Normal or Lowest latency). Settings is a
  sheet with three groups:
  - **Sending:** Profile (Low, Medium, or a custom size and rate), Codec, Bandwidth
    (Auto shows its value), and Don't scale.
  - **Receiving:** which formats to accept, HEVC and H.264. Only those this
    Mac decodes in hardware can be on; the receiver announces the rest as
    unsupported.
  - **Network:** the default port, which receiving listens on (or the next
    free one) and sending uses when the address has none.

  The Send page holds the receiver's address and port, a summary of the send
  settings, and Start.
- **Sending to a port:** the Mac Send page has a Port box, which starts at
  the default port and remembers the last one used. Windows and Android have
  only the Receiver field. Every Receiver field also takes "address:port"
  ("[v6]:port"), which overrides the box.
- **Firewall.** Before listening, the receiver asks `socketfilterfw` (no
  administrator rights needed). It explains only a real block: "Block all
  incoming connections", or LLCast set to block. Otherwise macOS asks once
  itself. Sending needs no firewall change.
- **System.** While a session runs, the app holds an
  `NSActivityLatencyCritical | NSActivityUserInitiated` activity, so App Nap
  stays off. The games app category lets macOS enable Game Mode in full screen.

## Window and prechecks

This follows the Windows app: one small window with a **Send / Receive**
choice. Problems are explained in an alert with what to do.

- **Send:** address and port, then Start. It checks, in order:
  1. The address.
  2. Screen Recording permission. Ask for it, then explain how to grant it in
     System Settings.
  3. A receiver answering PING. A denied Local Network permission shows up as
     `EHOSTUNREACH` and is explained as such.
  4. A codec this Mac encodes in hardware and the receiver decodes.
- **Receive:** starts at once (see above). It checks hardware HEVC or H.264
  decoding, the firewall, Metal and a free port, then goes full screen with
  the waiting card. Esc stops.

## Packaging (adapted from VibeOffice)

- **Build:** CMake with `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` and
  deployment target 14.0, as a `MACOSX_BUNDLE`.
- **Info.plist:**
  - bundle ID `LLCAST_BUNDLE_ID`;
  - `NSLocalNetworkUsageDescription`;
  - the screen-capture usage text;
  - games category.
- **Signing:** `tools/macos/build.sh` signs ad hoc. With `--build-dmg` it
  signs with the Developer ID, hardened runtime and no entitlements (none are
  needed outside the sandbox). If codesign fails, the app is removed.
- **DMG:** dmgbuild from a hash-pinned venv, then `notarytool`, staple and
  `spctl`.
- **CI:** `.github/workflows/macos.yml` is manual/reusable, with one universal
  build on macos-15 in the `release` environment. Push/PR checks run only on
  Linux. With `MACOS_DEVELOPER_ID_P12` (base64) and
  `MACOS_DEVELOPER_ID_PASSWORD`, it imports a temporary signing keychain;
  `ASC_KEY_ID`, `ASC_KEY_P8` and `ASC_ISSUER_ID` supply notarization credentials.
  The signed DMG goes to the commit's release; without the certificate, only
  an ad-hoc zip artifact is kept. Signing material is removed afterwards.
  Versions use the commit's UTC date and minute, including on the first build.
- **Self-test:** every CI build runs synthetic frames through VideoToolbox
  encode, UDP loopback, decode and a pixel check. Exit 77 means the runner
  has no hardware HEVC and is recorded as a skip. No capture permissions are
  needed. Optional `smoke` opens the Send/Receive chooser, keeps a screenshot
  artifact and quits; it does not test desktop capture.

## Mac latency notes

The project-wide levers are in [PLAN.md](PLAN.md). Mac-specific ones:

- **Sender.** ScreenCaptureKit plus low-latency VideoToolbox avoids our own
  conversion pass. Reported capture plus encode is about 11 ms at 1440p60 on
  an M4
- **Wi-Fi.** AWDL channel hopping (AirDrop, Continuity) causes periodic
  spikes; `sudo ifconfig awdl0 down` helps until macOS re-enables it.
  The app could detect the spikes and suggest a wired connection or AWDL off.
  Changing the setting itself is a persistent system change, so it is left
  to the user.

## Research sources

- VideoToolbox and ScreenCaptureKit SDK headers.
- WWDC21 10158 (low-latency encoding); WWDC22 10155 and 10156 (ScreenCaptureKit).
- TN3179 (local network privacy).
- FFmpeg `videotoolboxenc.c` and `videotoolbox.c`.
- moonlight-qt `app/streaming/video/` (Metal renderer, AWDL issues #753 and
  #1427).
