# Tablet software AV1 experiment — Intern, 2026-10-28

**Result:** dav1d reached **103.37 fps average** decoding a short synthetic
1920×1080 AV1 stream at 19.57 Mbps on the ADVAN_TAB_V8. Changing the encoded
tile layout from two columns/two rows to four columns/one row improved
throughput. This is a CPU result; the tested Vulkan CDEF offload was slower.
The APK follow-up below adds this CPU decoder; energy/heat improvement remains
unproven.

## Device and method

- MT8781V/CA, Android 14; two Cortex-A76 cores plus six Cortex-A55 cores;
  Mali-G57 MC2, Vulkan 1.1.177. Codec enumeration exposes software AV1
  (`c2.android.av1.decoder`, libgav1), hardware H.264/HEVC, and no hardware AV1.
  The device does not advertise the Vulkan video decode extensions.
- dav1d 1.5.4, commit `54706fc6bc0cdecab7e9593974a4039cc038fca7`, built with
  NDK r27d, ARM64 NEON, 8-bit only, Cortex-A76 target and ThinLTO. Runtime:
  `taskset c0`, `--threads 2 --framedelay 1 --muxer null`. Only the test process
  is pinned to the two big cores; clocks and persistent settings are unchanged.
- FFmpeg `testsrc2`, 120 frames at 1920×1080/60, libaom realtime/cpu-used 8,
  no lookahead, 20 Mbps target, one-frame VBV. Repeat that clip four times
  as 480 complete frames. The tile comparison changes only `-tiles 2x2`
  to `-tiles 4x1`. Both use all filters required by their bitstreams;
  the 4×1 sequence enables CDEF and has no restoration or film grain.
- Output intervals: device `CLOCK_MONOTONIC` after consecutive null-muxer
  outputs, including IVF input and logging; first interval includes startup.
  These are not queue-to-output or glass-to-glass latencies. CPU time is
  simpleperf `task-clock` summed across threads; cycles count userspace work.

## CPU result

| Measurement | 2×2 tiles | 4×1 tiles |
| --- | ---: | ---: |
| Average unpaced decode throughput | 77.91 fps | 103.37 fps |
| Mean output interval | 12.83 ms | 9.67 ms |
| p95 / p99 output interval | 14.03 / 15.12 ms | 10.52 / 12.46 ms |
| Unpaced CPU time per frame, summed threads | 21.12 ms | 18.07 ms |
| CPU time per frame at fixed 60 fps | 22.61 ms | 22.47 ms |
| User cycles per frame at fixed 60 fps | 35.81 million | 36.23 million |
| PSNR against the original test pattern, all planes | 46.0666 dB | 45.9958 dB |

At maximum speed, throughput rose 32.7% and CPU time/frame fell 14.4%.
At equal 60 fps, CPU time differed by only −0.62% and cycles rose 1.15%:
**there is no demonstrated energy gain.** Automatic clock scaling was active;
these are single short runs, not controlled power or thermal measurements.
USB-powered battery current measures net battery flow, and PowerStats returned
no usable energy counter. Neither provides whole-device joules/frame here.

Fifteen of the 480 intervals in the 4×1 run exceeded 11.11 ms; none exceeded
16.67 ms. Thus 103 fps is average capacity, not proof of every 90 fps deadline.
The PSNR decrease was 0.071 dB on this pattern, not an assessment of games.
An earlier diagnostic that disabled in-loop filters is invalid for playback
and is not included in these results.

## Vulkan CDEF experiment

Captured the inputs and correct ARM outputs for 9,572 CDEF blocks in frame 60
(277,184 filtered pixels). The compute shader matched every captured output
byte; this checks one captured workload, not full AV1 conformance. Direction
search stays on the CPU. Five warmups precede 30 measured iterations.

| Same captured CDEF jobs | Mean |
| --- | ---: |
| ARM NEON, one thread, input reset and filtering, monotonic wall time | 1.114 ms |
| Vulkan shader, GPU timestamps | 4.672 ms |
| Vulkan copy/flush/submit/fence wait/invalidate, monotonic wall time | 7.065 ms |
| CPU process time during that Vulkan region | 2.027 ms |
| CPU process time during the ARM NEON region | 1.105 ms |

Do not integrate this CDEF offload: it is slower and uses more CPU time while
also running the GPU. It uses packed captured jobs and a blocking fence;
this result does not rule out a different full-frame GPU reconstruction design.
There is no working GPU-assisted whole-frame AV1 decoder in LLCast.

## Product implication and evidence

At benchmark time the APK required hardware AV1. The measured build is a
device-specific benchmark, not a portable APK library configuration. The
subsequent integration below needs separate presentation and power measurements.

The pinned NVENC header exposes uniform tile counts through
`NV_ENC_CONFIG_AV1.numTileColumns=4`, `numTileRows=1`,
`enableCustomTileConfig=0`. This is the next sender setting to validate;
the result above used libaom, not NVENC. The tile setting is not yet changed
in the Windows sender.

Raw logs, sources, shader, captured jobs and licenses are under
`build/results/tablet-av1/`. `optimization-results.json` contains exact values
and hashes; `analyze_optimization.py` regenerates it without device access.
The standalone
GPU prototype retains dav1d's BSD-2-Clause notice in `gpu/COPYING.dav1d`.

Sources: [dav1d settings](https://github.com/videolan/dav1d/blob/1.5.4/include/dav1d/dav1d.h),
[dav1d CDEF](https://github.com/videolan/dav1d/blob/1.5.4/src/cdef_tmpl.c),
[AV1 syntax](https://aomediacodec.github.io/av1-spec/),
[Vulkan AV1 decode extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_video_decode_av1.html),
[battery-current semantics](https://developer.android.com/reference/android/os/BatteryManager#BATTERY_PROPERTY_CURRENT_NOW),
and `third_party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h`.

## APK follow-up requested by the user

- Added pinned dav1d 1.5.4, 8-bit ARM64/ARM32 builds, statically linked into
  `libllcast.so`. Normal mode prefers hardware AV1 when present, otherwise
  advertises/uses dav1d. Two workers, one-frame delay, all in-loop filters.
- A separate GLES 3 renderer uploads Y/U/V planes, applies range/matrix and
  scales to the existing SurfaceView. One replaceable waiting picture bounds
  display backlog; dav1d references keep predictor/display memory alive.
- Built and installed APK SHA-256 `2719982250fd19b91ec161f60e7980d767c015d71f5ce2c36587fec007de694d`.
  The prior APK is backed up locally. Both ABIs link; signing and packaging
  verify. The user requested manual playback instead of the planned local test.
- The Windows AV1 precheck then failed: zeroing AV1 configuration selected
  level 2.0, unlike the automatic zero value in other codecs. Set
  `NV_ENC_LEVEL_AV1_AUTOSELECT`. Start logging before prechecks and save logs
  beside the EXE, falling back to LOCALAPPDATA on a read-only installation.
- The user confirmed AV1 now works. The saved Windows log names RTX 5060 Ti,
  NVENC API 13.1, AV1 1920×1080/60 at **30 Mbps**, targeting the tablet at
  `192.168.5.121:47800`. This is a functioning stream, not a measured 90 fps
  playback or power result. Current builds are in `/mnt/hgfs/deb13/LLCast`.

## Unified Extreme mode — tablet first

The user confirmed the guarded tablet test showed neutral grayscale bars.
The tablet's MediaTek KMS primary plane accepts YUYV, not NV12/I420, and has
no rotation property. Cached `mtk_mm` DMA allocation, PRIME import and a
plane-only atomic commit succeeded on the existing 1200×1920 mode. The last
probe showed grey for four seconds, black for two, then restored Android
(`extreme/plane-result3.txt`: status 0, restoration-status 1). This proves a
static scanout, not sustained streaming or a power improvement.

One Extreme UI now selects decoder by codec: dav1d for AV1, Rockchip MPP for
HEVC only. MPP is a runtime plugin; its missing vendor library no longer
prevents the receiver from starting. CPU frames are packed on the display
worker into four reusable DMA buffers. The decoded mailbox holds one waiting
picture; replacing it never drops an encoded reference frame. Buffer release
waits for the replacing flip. The direct path uses no GLES/Vulkan.

Rotate clockwise 90° when `(device_w > device_h) != (sender_w > sender_h)`;
otherwise preserve orientation. Aspect fit keeps black borders. The tablet
uses YUYV; an NV12 plane can use the same software path. Compatible MPP DMA
frames remain direct, with packing only when format/orientation differs.
All streams here are SDR, 8-bit BT.709 limited. Software AV1 is limited to
1920×1080 pixels, allowing portrait geometry. Other drivers remain unverified.

The root guard discovers the running HWC service name (this tablet uses
`vendor.hwcomposer-2-3`, the box uses `2-1`). Back/Esc, Power and either Volume
key exit. Existing guard tests pass 22/22. Pure rotation checks cover both
orientation mismatches, unchanged orientation, chroma order and NV12 output;
receiver lifetime and framebuffer cache checks pass. The APK builds for both
ABIs. APK `2026.10.28-e0f045` is installed and hash-verified on the tablet;
native decoder and read-only display probes pass (1200×1920@90 Hz). The user
confirmed the new Extreme mode works. No persistent display, power or network
setting changed; the 90 Hz screen mode is not a claim of 90 fps decoding.

## Why the first Extreme build showed about 43 fps

The completed Windows 60 fps session `5e886552` sent 6,507 frames. Receiver
reports show 4,682 displayed and 1,732 already-decoded pictures replaced before
display: **6,414 decoded (98.6%)**, with just 21 lost, 59 skipped after resets
and 13 decode errors. The final quality window reports 43.2 shown fps and
99.97% packets received. The main throughput loss is after decode.

Receiver-clock median complete-AU-to-decoded is 11.245 ms (includes queueing),
versus 29.713 ms decoded-to-flip (includes waiting, conversion and scanout).
The display counts 4,752 repeated vblanks over 4,682 displayed pictures on the
90 Hz panel, consistent with mostly every second refresh, near 45 fps.
That build's display worker started packing only after the previous flip.
This motivated optimizing packing/rotation and overlapping preparation with
the pending flip; it did not show that dav1d itself was limited to 43 fps.

After the sender switched to 30 fps (`77d42ece`), a three-second simpleperf
sample attributed 46.70% of sampled user CPU cycles to `scanout_pixels`.
A separate four-second passive observation of LLCast's outgoing reports saw
123 shown and one replaced, about 31 reported fps. These later observations
are not measurements of the earlier 60 fps session. No stream was stopped or
settings changed. Raw: `extreme/windows-60fps.log`, `live-profile.txt`,
`live-threads.json`, `live-reports.csv`, `rate-diagnosis.json`.

## Display optimization and local replay

Native-size I420-to-YUYV now fuses 90-degree rotation and packing with ARM64
NEON in cache-sized tiles, clearing only the black borders. The general scaler
remains available. The display worker prepares one decoded picture while the
preceding flip is pending and commits it as soon as that flip retires. The
four-buffer pool still retains scanning/pending ownership until retirement.

On the tablet, bit-exact comparisons with the scalar reference passed for
orientation, tails, padded pitches, chroma, borders and a full 1080p picture.
A conversion-only microbenchmark used 20 paired samples on CPU 3 (A55), without
changing its governor: generic 42.159 ms wall / 41.503 ms thread CPU versus
NEON 12.376 ms wall / 12.030 ms thread CPU. This is not whole-device energy,
heat, sustained playback throughput or a comparison with the live CPU clocks.

A guarded local replay sent 240 synthetic 1080p60 AV1 frames to the tablet's
loopback receiver: **240 displayed, zero replaced/lost/decode errors**, with
the existing 1200×1920@90 mode. Mean preparation including DMA synchronization
was 4.421 ms (241 samples including the waiting card). In the 230-frame steady
section, receiver CLOCK_MONOTONIC medians were 9.487 ms decoder put-to-picture
and 9.308 ms ready-to-DRM-flip. These exclude PC capture, encoding and Wi-Fi;
the flip timestamp is not the end of panel scanout or glass-to-glass latency.
The guard exited 0 and restoration-status was 1. A new Windows stream and
sustained power measurement are still needed.

The local replay initially failed with EINVAL on its first nonblocking flip,
including when using the previously working APK's binary. Property-only tests
passed. Explicit CRTC activation fixed it: MediaTek can retain mode_valid after
HWC deactivates the controller. Mode selection now reads ACTIVE, preserving
the retained mode while reactivating when needed; every commit includes ACTIVE.
Existing host fixtures cover active/inactive controllers retaining the same
mode. No persistent device settings changed.

Raw evidence under `build/results/tablet-av1/extreme/`: `pack-bench-neon.txt`,
`opt-old-baseline-receiver.log`, `opt-diag-receiver.log`,
`opt-explicit-crtc-receiver.log` and `opt-explicit-crtc-restoration.txt`.

## Extreme reboot after several minutes (2026-10-29)

The current tablet kernel is `5.10.218-android12-9-gc39cd0c7fc35-dirty`.
Its live log shows `HANG_KICK (300)` from Android every 30 seconds and
`hang_detect thread counts down 10:10, status 1`. Extreme deliberately stops
`system_server`, including the thread producing those heartbeats. The old
guard had no replacement heartbeat. A roughly five-minute reboot is consistent
with this conflict; the saved crash trace cannot prove the last reboot's cause.

The saved `console-ramoops-0` instead names **5.10.209**, with September 2025
timestamps and a USB-network suspend panic. Both October 28 and October 29
DropBox last-kmsg files have the same size/content. These are stale records,
despite the current boot properties saying `kernel_panic,bug`. Do not attribute
this session's reboot to that USB trace or assume overheating.

[MediaTek's published 5.10 hang detector](https://github.com/MotorolaMobilityLLC/kernel-mtk/blob/android-13-release-ttt/drivers/misc/mediatek/monitor_hang/hang_detect.c)
counts 30-second ticks after its last heartbeat and triggers a dump/panic when
the countdown expires. Its `HANG_KICK` ioctl takes the timeout as a scalar;
zero disables detection. The handler restricts caller command lines to platform
prefixes. This is a related vendor source, not the tablet's exact kernel tree.

The APK root input listener now opens `/dev/RT_Monitor` when present and renews
300 seconds every 30 seconds. No extra thread, decode-path wait, model check,
hardware watchdog change or thermal-control change. The verified installed
helper re-execs with the process label `/system/bin/llcast-guard` for the vendor
command-line check; it still executes from its APK path and writes no system
file. Startup logs once. An ioctl failure logs once and stops heartbeat attempts
without ending playback, as requested by the user. Other devices skip this
optional interface. Normal Back/Power/Volume restoration remains unchanged.

Raw evidence and the timed validation are in `build/results/tablet-reboot/`.
The native APK guard is covered by this change; standalone ADB runner sessions
do not launch its input listener and have not gained this heartbeat.

The rebuilt APK (`2026.10.28-83b70c`, SHA-256
`38d92739ee6663ac1d0303e61c443d7f7a833142c226d30866856fd27f8e355f`)
is installed and hash-matched to `/mnt/hgfs/deb13/LLCast/LLCast.apk`. A single
six-minute local 1080p30 AV1 replay displayed all 10,800 frames, with zero
loss, replacement or decode errors. The kernel accepted the guard heartbeats
while `system_server` stayed stopped; boot ID remained unchanged for 364 s.
Exactly one heartbeat setup log, no failures. Explicit test stop restored
HWC/SF/framework (`restoration-status=1`). The 1200×1920@90 mode was retained.
This verifies one run across the timeout boundary, not indefinite operation,
PC/Wi-Fi performance or whole-device energy consumption.
