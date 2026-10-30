# LLCast for Android

One APK for Android 9 or later. Like the Mac app, it opens on a Send/Receive
choice:
- **Send:** no root. Android's screen capture (MediaProjection) mirrors the
  screen into the hardware encoder's input surface (HEVC, H.264 or AV1; Auto
  picks HEVC, then H.264, from what both ends share, and AV1 only when chosen). `llcast_send.c` hands each AU to the C sender session;
  a receiver's RECOVER requests a sync frame. A foreground service with a Stop
  notification keeps it running in the background. The whole screen is fitted
  to the mode; there is no "Don't scale" on Android. The selected fps stays
  fixed while the image size is reduced if needed to fit the hardware encoder's
  limits; status shows the actual encoded size. Missing hardware and an
  unsupported profile have separate error messages. Codec callbacks only queue
  output indices; a bounded worker handles buffer release, sending and sync-frame
  requests, avoiding synchronous calls on the codec's own callback thread.
- **Receive** has two modes:
  - **Normal:** no root. Hardware HEVC/H.264/AV1 uses MediaCodec straight to a
    SurfaceView. Without hardware AV1, bundled dav1d decodes 8-bit AV1 on the CPU
    (up to 1920×1080, two workers, one-frame delay); OpenGL ES 3 uploads planar
    YUV and converts/scales it for display. AV1 must be selected on the sender;
    Auto continues to prefer HEVC, then H.264.
  - **Extreme low latency:** one mode for supported rooted ARM64 devices.
    AV1 uses dav1d on the CPU; HEVC uses the optional Rockchip MPP backend.
    Both use DRM scanout under the restoration guard. On the MT8781 tablet,
    the display worker packs I420 into YUYV and rotates clockwise 90° when
    `(device_w > device_h) != (sender_w > sender_h)`, then fits the picture
    with black borders. Android's current display mode stays unchanged.
    Root alone does not guarantee a usable KMS display or DMA heap.
    Select AV1 explicitly for the tablet; Auto never selects AV1.

Build from Linux with JDK 21, CMake, a C/C++ compiler, Python 3.12+, Meson,
Ninja and `zip`:

```sh
python3 tools/build_apk.py
# Or use an existing SDK (build-tools 35.0.0, platform 33) and NDK r27d:
python3 tools/build_apk.py --sdk /path/to/sdk --ndk /path/to/android-ndk-r27d
```

The output is `build/apk/LLCast.apk`, application ID `io.github.tcchen2026.llcast`
(Java package `org.llcast`), versioned from the commit (`tools/current-tree-version.sh`).
Development builds before this ID were `org.llcast.box`: uninstall that once
(`adb uninstall org.llcast.box`).
`build/apk/build.json` records the APK hash, toolchain paths and signing type.
There is no Gradle or AndroidX. `build_dav1d.py` fetches dav1d 1.5.4 from
VideoLAN, verifies its pinned SHA-256, and builds static ARM64/ARM32 NEON libraries.
Sources and build products stay under `build/`; the APK includes the BSD license
at `assets/dav1d-COPYING.txt`. `--meson /path/to/meson` selects a local Meson.
SDK packages
come from Google's repository; archive sizes and checksums are verified against
its HTTPS XML. NDK r27d is reused if available, or fetched the same way.

One APK for arm64 and 32-bit ARM. It contains `libllcast.so` (JNI; both ABIs)
and, for Extreme mode, two arm64 PIE executables named `.so` plus the optional
`libllcast_mpp.so` backend, extracted into Android's protected `nativeLibraryDir`.
It includes no vendor libraries. CI links
against trap-only symbol interfaces generated from `android/native/*-symbols.txt`;
those interfaces are **never packaged or installed**. On the box the receiver
uses its existing `libdrm.so`. Only the optional Rockchip backend loads
`/vendor/lib64/libmpp.so`; its absence does not prevent tablet AV1 startup.

Normal mode's dav1d renderer has one replaceable decoded picture waiting for display and
one in progress. Decode never waits for vsync. Pictures retain their dav1d
references through upload; frames get one completion report. Display reports
mean EGL submission, with no fabricated flip timestamp. On heterogeneous CPUs,
the decoder uses the faster cluster only when sysfs and the process's allowed
CPU mask identify it; otherwise Android schedules the two workers. No governor
or persistent setting is changed. Short tablet measurements and their limits
are in `docs/research/tablet-av1.md`.

Extreme mode has the same one-frame decoded mailbox. Packing/scaling runs on
the display thread, independently of decode, while the preceding flip is pending.
The tablet's native-size I420-to-YUYV path fuses rotation and packing with ARM64
NEON; other sizes retain the scalar scaler. Its four-buffer DMA pool and the
framebuffer cache are bounded; a buffer stays owned until the replacing flip.
No Vulkan or GPU decoding is involved. Compatible MPP NV12 frames still scan
out without copying. CPU frames use cached DMA heaps and explicit DMA-BUF
synchronization. NV12/YUYV format support is checked before takeover; arbitrary
rooted Android display drivers are not claimed to work. The software path is
8-bit I420, BT.709 limited, at most 1920×1080 pixels (portrait is allowed).
An inactive display controller is reactivated using its retained mode; having
valid mode information alone does not prove that it is still scanning out.

Signing defaults to `build/signing/llcast-development.p12`, kept locally across
builds. For release signing set `LLCAST_KEYSTORE` (a file path) or
`LLCAST_KEYSTORE_BASE64`, plus `LLCAST_STORE_PASSWORD` and `LLCAST_KEY_ALIAS`.
`LLCAST_KEY_PASSWORD` defaults to the store password. Passwords are passed through
environment references, never command arguments. CI uses the correspondingly
named repository secrets. Without secrets, its artifacts use a new development
key on each fresh runner; use a stable release key for installable updates.

## Operation

Open LLCast from either Android launcher. Extreme mode first checks app-level
`su -c id` and the native decoder; the display and an exit key are checked when
the receiver starts, and a failed start says what is missing. Root approval gets
90 seconds; a denial or timeout leaves Android running and shows a reason.
Checks stop at the first failure. All user-facing
strings, including the waiting card, live in `res/values/strings.xml` for Lead
review.

The launcher PNG (192×192) and TV banner (320×180) use the shared screen/bolt
icon from `tools/make_icons.py`. To regenerate them, run
`python3 android/generate_icons.py` with Pillow and DejaVu Sans Bold installed.
The banner title comes from `app_name` in the same strings file. APK and CI
builds use the checked-in PNGs and need no image-generation dependencies.

Start invokes only the installed native launcher with a fixed operation. The
launcher verifies root and its installed executable paths, copies the receiver
into root-only `/data/local/tmp/llcast/` (only programs there may load the
box's `/vendor` libraries), rechecks hardware,
double-forks, calls `setsid()`, and moves into the roots of the mounted cgroup
hierarchies. If it cannot verify those memberships it refuses to take over the
display. **Actual survival after force-stop is still a device acceptance test.**

`tools/device_session.py` is the one restoration implementation. The APK build
embeds its generated script in the native launcher and invokes `/system/bin/sh
-c` with that immutable string. No script is extracted to a writable directory
or read back for execution. The same display lock is used by the ADB runners.
Android service states are saved before stopping SurfaceFlinger/HWC. The guard
first pauses `system_server`, so pending UI work cannot crash it and take Wi-Fi
down, and resumes it after SurfaceFlinger restarts. It stops and reaps the
receiver before restoring those services and zygote, including when the
receiver crashes or the guard receives TERM/INT. No streaming or idle
duration is imposed. The three-second shutdown escalation applies only after a
stop, never to the lifetime of a running stream.

A separate root child polls `/dev/input/event*` and watches input-device hotplug.
Linux Back, Esc, Power, Volume Up or Volume Down, on key-down, signals the guard
to restore Android. It does not inject or exclusively grab input. The listener must be ready
before any display service is stopped. Losing all usable input devices also
ends the session. ADB key injection is not a substitute for the physical remote:
Android's framework is unavailable during direct scanout.

When `/dev/RT_Monitor` exists, the APK's root input listener also supplies the
MediaTek framework watchdog heartbeat every 30 seconds while `system_server`
is paused. It leaves the watchdog armed at Android's normal 300-second timeout.
This is detected by interface, not tablet model; devices without it keep the
existing path. One startup log, no periodic app logs. An ioctl failure logs
once and stops heartbeat attempts, without ending playback. No hardware
watchdog, thermal control or persistent setting is changed.

Logs go to root-owned `/data/local/tmp/llcast/session-*/`: startup, service state,
DRM snapshots, status and restoration result. Keep five completed/failed sessions
including the next session; never prune a live guard. App sessions request no
per-frame CSVs. The launcher never changes Wi-Fi, firmware or system libraries.

## Acceptance on the box

On 2026-10-28 the APK's Extreme mode streamed HEVC on the box, kept Wi-Fi up,
and restored Android on physical Back; restoration still drops the network for
about 30 s. Force-stop survival is not yet proven. Root ADB alone proves nothing
about app-level `su`, input key mapping or cgroup permissions. Install only this package and record its APK hash and package
version in `build/results/`; uninstall no other packages.

```sh
adb -s 127.0.0.1:5555 root
adb -s 127.0.0.1:5555 install -r build/apk/LLCast.apk
adb -s 127.0.0.1:5555 shell am start -n io.github.tcchen2026.llcast/org.llcast.MainActivity
# Extreme mode's checks directly, forcing a failed check to display its reason:
adb -s 127.0.0.1:5555 shell am force-stop io.github.tcchen2026.llcast
adb -s 127.0.0.1:5555 shell am start -n io.github.tcchen2026.llcast/org.llcast.BoxActivity --es llcast.fail decoder
```

Repeat the forced failure with `root` and `root_timeout`. The display and the
Back/Esc key are checked when the receiver starts, and a failed start names what
is missing. This switch can only fail
a check, never bypass one or alter a command. Relaunch without the extra for normal
checks. Verify each reason and the focused Quit button. Then:

1. End any existing ADB display session through its guard before pressing Start.
2. Start LLCast and check the waiting card, live address and Back/Esc instruction.
3. Have the user start the PC sender, after the Lead has deployed its current build.
4. Force-stop **only** `io.github.tcchen2026.llcast` mid-session. Confirm the receiver/guard PIDs,
   `/proc/<pid>/cgroup`, UDP reception and HDMI picture continue.
5. Press physical Back, and repeat with Esc if a keyboard is available. Confirm
   Android returns and the session records `restoration-status=1`.

Offline validation: `python3 tests/test_apk.py`, the KMS fixture
`tests/test_drm_precheck.c`, and the existing `receiver_runner`, `rx_decode` and
`rx_lifetime` tests. Android CI runs the full host suite, builds/signs/verifies the
APK and uploads only the APK and build metadata. SDK 35.0.0/platform 33 and NDK
27.3.13750724 are reused from the runner only when the versions match.

Sources: [AAPT2](https://developer.android.com/tools/aapt2),
[d8](https://developer.android.com/tools/d8),
[APK signing](https://developer.android.com/tools/apksigner),
[TV launcher requirements](https://developer.android.com/training/tv/get-started/create),
[Android cgroups](https://source.android.com/docs/core/perf/cgroups).
