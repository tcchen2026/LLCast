# Box decoder and display tests

The phase-0 tools `hevc_decode_test`, `tools/run_hevc_test.py` and
`tools/show_hevc.py`, and what they measured on this box.

A small ARM64 Android executable that calls Rockchip's existing `libmpp.so`
directly. It submits HEVC access units to the hardware decoder, receives decoded
DMA buffers, and records per-frame submission-to-output time. It does not use
MediaCodec, Java, or a software decoder on the TV box.

It can also scan those decoded NV12 buffers directly out to HDMI through DRM,
without CPU pixel copies or GPU rendering.

The connected HK1 RBOX K8 identifies as **RK3528A**, Android 13, kernel 5.10.157.
The relevant interfaces are `/vendor/lib64/libmpp.so` and `/dev/mpp_service`.

## Run the complete test

Requirements on the build machine: Python 3, CMake, Android NDK, ADB, and FFmpeg
with `libx265`. Authorize ADB on the box; root access is available on the tested
firmware. ADB must be connected before invoking the runner.

```sh
python3 tools/run_hevc_test.py \
  --ndk /path/to/android-ndk-r27d \
  --serial 127.0.0.1:5555
```

In this workspace, NDK r27d has been downloaded and checked against Google's
repository checksum. It is available at `build/toolchains/android-ndk-r27d`:

```sh
python3 tools/run_hevc_test.py --ndk build/toolchains/android-ndk-r27d
```

The default test:

1. Pulls the box's own MPP and DRM libraries for linking, then cross-compiles the probe.
2. Generates 180 frames of moving 1080p60 HEVC Main / 8-bit 4:2:0 video, with
   no B-frames or lookahead. FFprobe checks the frame count and picture types.
3. Uploads the executable and clip beneath `/data/local/tmp/llstream-hevc/`.
4. Submits one complete picture at a time, paced at 60 fps, with MPP immediate
   output enabled. No additional input picture is queued until its predecessor
   has been delivered. MPP still retains the reference buffers the codec needs.
5. Runs a separate pixel-verification pass. CRC32 values covering every active
   NV12 pixel must match FFmpeg's software decoder on the build machine.

Results go to `build/results/<timestamp>/`, including `timings.csv`, logs,
pixel checksums, and the library/input SHA-256 hashes. Test artifacts remain in
the timestamped device directory so the binary can be run again. The runner
does not change device settings or replace system libraries.

Useful variations:

```sh
# Substitute your NDK path, or set ANDROID_NDK_HOME.
python3 tools/run_hevc_test.py --ndk /path/to/ndk --size 1280x720
python3 tools/run_hevc_test.py --ndk /path/to/ndk --unpaced --skip-verify
```

## Run the executable directly

From an ADB shell, in the timestamped test directory:

```sh
./hevc_decode_test test.hevc --fps 60 --csv timings.csv
./hevc_decode_test test.hevc --fps 0 --hashes hashes.csv
```

Input is a single-layer Annex-B HEVC elementary stream, at most 512 MiB. The
probe recognizes picture boundaries, including multi-slice pictures and streams
without AUD NAL units. Immediate output assumes no presentation reordering.
For an ordinary reordered clip, use `--no-immediate --window 16` (the required
window depends on the stream); that mode measures buffering as well as decode.
Pixel verification supports linear, even-sized 8-bit NV12, with stride/crop
handling and DMA buffer cache synchronization. Other formats can be timed but
are not supported by the checksum pass.

Invalid input, missing pictures, decoder errors/discards, duplicate PTS values,
and a stalled decoder produce a nonzero exit status. The runner also applies
a process timeout.

## Interpret the measurements

`timings.csv` records each picture's monotonic submission and delivery times.
The summary excludes the first ten frames from steady-state statistics and
reports the first-frame time separately. These numbers include MPP parsing,
driver work, scheduling, and output delivery. They are **not isolated hardware
execution time** or PC-to-TV latency. Pixel verification adds CPU work and is
therefore performed separately from the latency pass.

With `--unpaced`, the reported output rate is for the selected submission
window (one picture by default), not maximum deeply pipelined throughput.

Wi-Fi transport and PC capture/NVENC encoding are not part of this test.
HDMI presentation is included only with `--display`, and the per-frame CSV
continues to measure submission through decoded-frame delivery before display.
The box initially used 1080p24; the screen test has now set it to 1080p60.

## Show the video on HDMI

After building the decoder test, run:

```sh
python3 tools/show_hevc.py --keep-60hz
```

This plays the three-second 1080p60 clip five times. `--repeat 1` makes a short
three-second test. `--keep-60hz` saves the supported 1080p60 timing in this
firmware's `persist.vendor.resolution.HDMI-A-0` setting, so Android also returns
at 60 Hz. The previous value is recorded in the result directory.

The program imports the MPP DMA-BUF file descriptors with
`drmPrimeFDToHandle()`, creates NV12 framebuffers with `drmModeAddFB2()`, and
updates the HDMI plane with non-blocking atomic DRM commits (`drm_display`).
It retains each displayed MPP frame until the flip that replaces it completes.
The display hardware performs YUV-to-RGB conversion and scaling.

Root is required. This full-screen test temporarily stops SurfaceFlinger and
the hardware composer to acquire DRM master. It restarts those services and
the Android framework afterwards; Android apps restart as part of recovery.
A device-side watchdog and cleanup trap run independently of ADB, because
restarting the framework can interrupt an ADB connection. Logs, the active
NV12 scanout state, and the restored Android display state are saved under
`build/results/display-<timestamp>/`.

For a manually managed display session, the native options are:

```sh
./hevc_decode_test --display-info
./hevc_decode_test test.hevc --display --fps 60 --repeat 5
```

The native executable requires DRM master for `--display`; use the Python
launcher to manage Android's display ownership. The display keeps the CRTC's
current mode, else uses the preferred one, then the first progressive one.
The renderer supports linear 8-bit NV12 and preserves the video's aspect ratio.

The first successful screen run presented all 900 frames in about 15 seconds
(60.04 fps), reusing three decoder DMA buffers. Kernel state confirmed NV12
scanout at 1920×1080p60. Decoder delivery remained about 4.46 ms median; blocking
display commits averaged 12.03 ms including refresh waits. These are separate
stages, not a measurement of PC-to-TV latency.

## Results on this box (2026-10-26)

Both runs used 180 frames, 60 fps submission, one picture in flight, immediate
output, and the firmware's unmodified MPP build `8a4fa843` (2023-07-31).
Statistics below use the 170 frames after warmup. Each timing run increased
the `rkvdec0` hardware task counter by exactly 180; each separate verification
run matched all 180 software-decoded frame checksums.

| Input | Median | p95 | Maximum | First frame |
| --- | ---: | ---: | ---: | ---: |
| 1920×1080 at 60 fps | 4.571 ms | 4.845 ms | 5.314 ms | 5.554 ms |
| 1280×720 at 60 fps | 2.982 ms | 3.212 ms | 3.889 ms | 3.918 ms |

Local result directories:

- `build/results/20261026T022614.574571Z/`: 1080p60 timing and verification.
- `build/results/20261026T022645.937366Z/`: 720p60 timing and verification,
  plus a 12-frame multi-slice stream without AUDs and negative-input checks.

The multi-slice stream also passed pixel verification. Empty input, non-HEVC
input, and a truncated NAL header returned clean nonzero exits. The shutdown
sequence resets MPP before destroying it, releasing HEVC reference frames
while the vendor library's worker threads still exist.

These are short decoder measurements on synthetic content, with stock clock
and display settings. They do not establish sustained Wi-Fi streaming latency.
