# Windows app and sender

`llstream_win.exe` captures an SDR output on its owning hardware adapter, scales the
whole desktop to the selected mode with centred black bars, converts to
BT.709 limited NV12, encodes with NVENC or a Media Foundation hardware encoder, and sends through the shared UDP
session. One pipeline thread/device and one encoder input are in use. No audio,
input forwarding, cursor composition, HDR or rotated-output handling in v1.

## LLCast.exe

The window for users (`llcast/`) first offers **Send** and **Receive**, as on
the Mac. Each role keeps its own taskbar entry while the chooser is hidden.
Send runs the same pipeline (`pipeline.cpp`) as the CLI, with no
per-frame CSVs. Before each send it checks, in order:

1. The address and port (host names are resolved).
2. A local, unlocked console desktop (not Remote Desktop).
3. The main display: on a hardware adapter, unrotated, SDR.
4. A receiver answering PING with its supported codecs.
5. A compatible hardware encoder on the display's adapter, using the chosen
   NVENC or Media Foundation backend. No software encoder is used.

Codec offers Auto / AV1 / HEVC / H.264. Auto tries HEVC, then H.264; it never
selects AV1. A forced codec stays forced across encoder attempts. A failure
names the side that cannot use it and the codecs the receiver supports.
Both NVENC and MF support all three codecs where their hardware does.

The first failure is shown in a message box, with what to do. Failures while
starting or streaming are mapped to a plain-language cause, and the details
show the log tail. Settings are kept in `HKCU\Software\LLCast` (Host, Profile
with CustomWidth/CustomHeight/CustomFps, BudgetAuto/BudgetKbps, NoScale, Encoder
and Codec); a legacy Mode value is read when Profile is absent. Codec stores
wire IDs: 0 Auto, 1 HEVC, 2 H.264, 3 AV1; an unknown saved value loads as Auto.
Send gives 10% of the budget to FEC parity; the encoder gets the rest.
There is one log per stream in `logs` beside the EXE, or in
`%LOCALAPPDATA%\LLCast\logs` when that isn't writable; the newest ten are kept. The icon comes from `tools/make_icons.py`.

## Receiving on Windows

Choose **Receive**. The full-screen waiting view shows one IPv4 address and
the listening port (47800, or the next free port through 47809). Prefer the
active default route with the lowest route-plus-interface metric; without a
usable default, prefer active Ethernet/Wi-Fi. Refresh from the local routing
table without sending traffic. Enter this address on the sending device. Allow LLCast through
Windows Firewall for your private LAN if Windows asks. Esc or Back stops and
returns to the chooser. There is no session or idle limit; the same receiver
waits for another sender after a disconnect. No display mode is changed.

The first implementation accepts 8-bit H.264 and HEVC with hardware decoding.
It checks the GPU's D3D11 decode profiles and usable, D3D11-aware Windows
Media Foundation decoders, then advertises only those codecs. It requires
decoder-owned NV12 GPU output and never retries with software decoding.
When the Windows HEVC decoder is missing or cannot be activated, a dialog
explains how to install [Microsoft's HEVC Video Extensions](https://apps.microsoft.com/detail/9nmzlz57r3t7)
and reopen LLCast. It offers a Store link and **Continue with H.264**; the
waiting screen retains the help button. Nothing opens or installs without a click.
H.264 can still receive, and a sender choosing Auto can select it. An installed
HEVC decoder with unusable GPU support gets a different dialog. A component
that loads but fails decoder setup also opens guidance, with the HRESULT in
Details; it does not claim the extension is missing. The warning is based on
usable HEVC capability, not just successful component activation. Windows N
without Media Foundation gets Media Feature Pack instructions; the app and
NVENC sender still start because MFPlat is loaded lazily.

`receiver/win/` uses the shared UDP reassembly/FEC session, a decode worker,
and a presentation worker with a single replaceable decoded-picture mailbox.
The D3D11 video processor scales NV12 into a BGRA flip-model swap chain,
using BT.709 limited input and full-range RGB output. Aspect fit and the
receiver's existing near-native size rule apply. **Normal** uses Present(1);
**Lowest latency** uses Present(0), allowing tearing when DXGI supports it.
The GPU device context is protected for both workers, and stop joins them
before closing the session. The UI keeps pumping messages while they stop.

Successful Present calls count as **DELIVERED** with a decode timestamp.
`flip_us` stays absent: Present returning does not measure scanout or light
from the display. Receiver profiling uses the usual `--profile` / `--profile-to`
flags and stays off by default. H.264 receiving works on the PC; HEVC
playback, colour accuracy and latency are still unverified there. Wine checks
cover navigation and failure UI.

The sender shows capture-to-display **minimum and median** for each feedback
period (normally five seconds), using fresh, undamaged SHOWN frames with mapped
flip timestamps. Lost/late, skipped, replaced and repeated frames are excluded
from latency, while reception failures still affect quality. No valid sample
means no latency number. The minimum covers every eligible frame; above 1,024
samples the bounded median is labelled estimated. Profiles and end-of-run
summaries retain original timings, including repeated content's age.

Implementation references: Microsoft's [D3D11 decoder integration](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation),
[H.264 MFT](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder),
[HEVC MFT](https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-decoder),
and [codec extension availability](https://support.microsoft.com/en-us/windows/apps/windowsmediaplayer/codecs-faq).

## Media Foundation fallback

The MF backend requires asynchronous, D3D11-aware hardware MFTs. It wraps the
converter's NV12 texture with `MFCreateDXGISurfaceBuffer`, sets the DXGI device
manager, CBR, low-latency mode and BT.709 limited colour attributes.
Low-latency mode is required and output timestamps must match the sole input.
The separate B-frame-count control is optional: NVIDIA's H.264 MFT rejected
it with `E_INVALIDARG` despite accepting low-latency mode. Microsoft documents
[B-picture control as optional](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder)
and [low-latency mode as forbidding frame reordering](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode).
Only one frame is submitted before its output is collected. Event callbacks wake
the pipeline; there is no encoder polling delay. Stop interrupts an event wait.
MFTEnum2 filters by the capture adapter LUID on Windows 10 1809+; older Windows
uses MFTEnumEx hardware enumeration and requires that the encoder accept that
device manager. Media Foundation is loaded from System32 at runtime; missing
MF disables this backend without preventing startup or NVENC. Windows N needs
the Media Feature Pack for MF encoding. Unsupported hardware/control settings are reported, not replaced by
a software encoder. Driver-dependent output and colour still need PC validation.

For the same-PC comparison, run the existing pattern with `--encoder nvenc`
and `--encoder mf`, both with `--codec hevc`, the same mode, and an explicit
`--seconds` or `--frames` test limit. `--codec h264` works with either backend. The file
keeps the requested name; choose `.h264` for H.264. `--profile` is still opt-in.
`encode_start_us` is immediately before NvEncEncodePicture/ProcessInput and
`encoded_us` is when the output bitstream buffer is locked, both on QPC. This
excludes input preparation and post-lock copies. It is encoder latency, not
glass-to-glass latency. No MF hardware latency has been measured yet.

API references: [hardware MFTs](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts),
[asynchronous events](https://learn.microsoft.com/en-us/windows/win32/medfound/asynchronous-mfts),
[D3D11 surface buffers](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatedxgisurfacebuffer),
[adapter-filtered enumeration](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mftenum2),
and [force keyframe](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avencvideoforcekeyframe).

AV1 is available with `--codec av1` on NVENC GPUs reporting AV1 support or an
MF AV1 hardware encoder. Choose `.obu` for a raw output file. Recovery includes
a sequence header and KEY_FRAME; the stream uses sized OBUs and temporal
delimiters. Auto uses HEVC then H.264; AV1 is explicit only.
See [format details and pending checks](../../docs/research/av1.md).

## Build on this Linux VM

```sh
bash tests/build_windows.sh        # build/windows-check; prints and checks each EXE's DLL imports
cmake -S . -B build/host
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure
```

The build check selects Release, GCC/G++ **-posix**, C++17; libgcc, libstdc++ and winpthread
are linked statically. Windows 10 1803+ is required for the high-resolution timer.
Windows provides D3D11, DXGI, d3dcompiler_47, winmm, Winsock and qWAVE. NVENC is
runtime-loaded from the NVIDIA driver. [Header provenance and driver minimum](../../third_party/nv-codec-headers/README.md):
**n13.0.19.0, API 13.0, driver >=570.0**, checked before encoder creation. The
shader plane views and NVENC NV12 registration are checked on the actual device;
an unsupported path exits with a diagnostic, with no silent format substitution.

## Run on the PC

The user runs the EXE or Lead's `.bat` launchers from the shared folder on
the logged-in console desktop, with HDR disabled. The VM reads the resulting
`logs\<stamp>-<test>\` under `/mnt/hgfs/deb13/LLCast`. No OpenSSH is needed.
Start with diagnostics, then a local recording, then streaming:

```bat
llstream_win.exe --info
llstream_win.exe --output capture.hevc --seconds 10 --profile-to 192.168.185.100:47801
llstream_win.exe --host 192.168.1.50 --seconds 60 --profile-to 192.168.185.100:47801
```

If `DuplicateOutput1` reports unsupported or unimplemented, capture tries standard
[`DuplicateOutput`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput).
Both paths retain the SDR/rotation checks and GPU pipeline. BGRA8 and FP16 SDR
surfaces are accepted: the PC's standard fallback actually returned FP16 despite
the older API's documented BGRA behavior. With SDR
[Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range),
FP16 is linear scRGB with white=1; the shader clips to [0,1] and applies the sRGB
transfer function before the usual BT.709 matrix. HDR remains unsupported.
The process uses per-monitor DPI awareness so crop coordinates are physical pixels.
Logs include the process/input desktop names and current display mode. If both
calls reject the desktop, that failure still needs diagnosis on the PC.

`--info` lists adapters, output rectangles/refresh rates, DDA availability,
NVENC max API and HEVC caps, and NV12 RTV support; it initializes the selected
pipeline without capturing or encoding any frames. A nonzero exit means that
selected pipeline is not ready. `--output` without `--host` sends no video packets. The local recording allows
FFmpeg validation before the box is online; optional profiling uses its own TCP
connection to the collector.

In the commands above, replace the receiver address. Defaults: UDP 47800, the Medium profile (1080p60)
with the Auto budget (20 Mbps for HEVC), **primary display** (desktop rectangle
starts at 0,0), best-effort qWAVE/DSCP 40, no FEC, no pacing. `--mode 720p`
selects Low (1280×720 at 30 fps); `--mode 1080p` selects Medium. `--size WxH`
and `--fps N` make a custom profile. `--bitrate-mbps` (0.5–200) replaces the
Auto budget for the profile and codec.
`--no-scale` copies the top-left mode-sized region, black outside a smaller
desktop. `--pace-mbps 100` is an experiment,
not a guarantee about Wi-Fi airtime. `--no-qos` disables QoS. `--frames N` bounds
a run; Ctrl-C stops cleanly. `--display N` selects an attached output by the index
printed by `--info` (`-1` selects primary). `--output FILE` also works alongside
`--host`; `--hevc FILE` is an alias. Recording requires `--seconds` or `--frames`.
Recording saves encoded AUs after sending and
can affect timing. Keep the total measured bitrate within 30 Mbps, including
headers, feedback and keyframes.

Updates can encode once `period-2 ms` has elapsed since the previous encode;
otherwise DDA retains the latest image. With no update, the first repeat is due
after `period+1 ms`; subsequent repeats are due one nominal period apart.
RECOVER bypasses that wait and forces IDR+VPS/SPS/PPS. Repeats
retain the content's original DDA LastPresentTime. A duplication loss retries
every 250 ms while repeating the last valid image; device removal is fatal.
The pipeline waits on capture completion, RECOVER and a one-shot cadence timer.
A worker isolates the [uncancellable DXGI acquisition wait](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe);
GPU conversion and encoding stay on the pipeline thread. Recovery can encode
while that worker waits. Capture age still comes from DDA present time, and
`acquire_return_us` records the worker's actual completion QPC.

## Profiling

Diagnostics are off by default. `--profile 47801` listens on loopback;
`--profile-to 192.168.185.100:47801` connects to the VM collector without an inbound
PC firewall rule. Start `python3 tools/profile.py --listen 47801 --save DIR` on
the VM first. The program keeps fixed rings; the collector writes `sender.csv`
and `clock.csv`. `--profile-capacity N` overrides each ring's row capacity.

The sender table joins transport, receiver report and pipeline stages in memory:
raw DDA present QPC, present/acquire/conversion-submit/encode-start/bitstream-lock,
`last_send_submit_us`, `send_return_us`, and `file_write_return_us`. All `_us`
columns use sender QPC microseconds. Socket submission/return is not a NIC
transmission timestamp; conversion submit is not GPU completion; file-write
return is not a disk flush. Local recordings have session and send times zero.
There are no stage sidecars or shutdown file joins. See
[profiling](../../docs/PROFILING.md) for retention, reconnects and clock domains.

The shared sender retains at most 512 rows for feedback, waiting up to five
seconds from submission before retiring an unreported row (so old content
timestamps on repeats do not expire it early). Profile order is frame order.
Feedback arriving after retirement is counted as `retired_feedback` and ignored.
`history_evicted` and `feedback_expired` count capacity and timeout retirements.

Repeated frames have 0 for acquire/conversion events (absent), and retain the
original present time. `--pattern` has a synthetic content-creation timestamp,
not a DDA present event. Summaries split the first 60 frame slots from steady
state. Counts and maxima are exact; median/p95/p99 use at most 1024 reservoir
samples per interval/range and are labelled as estimates when sampled. Collected profiles support exact percentiles over retained records; check for
`#skipped` messages before treating them as a complete run. These are stage timings, not
measured glass-to-glass latency.

`--pattern-motion N` implies `--pattern` and scrolls it **right by N pixels per
encoded frame**, wrapping within the full source width before crop/padding.
N is an integer from 0 to 4294967295; zero retains the static pattern behavior.
Frame zero is unshifted; frame F has offset `(F * N) % source_width`. Two GPU
`CopySubresourceRegion` calls build the shifted BGRA texture; no per-frame CPU
image generation or upload is used. The converter fits the full shifted source
to the selected mode, or copies the top-left region with `--no-scale`, before NV12.
Motion uses the fixed 60 fps grid and gets a new synthetic content timestamp
each frame. Recovery IDRs also advance the frame number/motion step. The stage
profile's `repeated` flag is false for these newly converted frames.

For a moving-pattern pixel check (frame 59, N=8, source width 1920):

```bat
llstream_win.exe --pattern-motion 8 --no-scale --frames 120 --output motion.hevc --profile-to 192.168.185.100:47801
```

On the VM, after obtaining that HEVC or a loss-free receiver dump:

```sh
ffmpeg -v error -i motion.hevc -vf 'select=eq(n\,59)' -frames:v 1 -pix_fmt nv12 -f rawvideo decoded-59.nv12
build/host/test_color_math --reference expected-59.nv12 1920 1080 --offset 472
python3 tests/compare_nv12.py expected-59.nv12 decoded-59.nv12
```

After network loss, decoded-file frame indices can differ from sender frame
numbers. Match each dumped AU to the ordered `delivered` rows in `rx.csv`, then
use that row's frame number and the sender's source width/motion setting. With
multiple sender sessions, match the session too. GPU motion and NVENC pixel
checks still require the PC; the host test checks copy geometry against an
independent CPU reference, including odd widths, wrapping, crop and padding.

## GPU scaling

The GPU scale pass uses `clamp(ceil(source/fitted/2), 1, 4)` bilinear taps per
axis (at most 16 samples). A 2:1 reduction averages exactly 2×2 pixels. FP16
stays linear through scaling; `_SRGB` input uses the view's decode and explicitly
re-encodes the fitted image before NV12 conversion. See Microsoft's
[sRGB view rules](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/converting-data-color-space)
and [SampleLevel](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplelevel).
Equal sizes use the exact copy path, preserving pixel-test behavior.

## Optional FEC and damaged delivery

Streaming accepts `--fec-percent 20 --fec-min 1` (default FEC off), plus
`--drop-rate 0.01` for deliberate test loss. `--bitrate-mbps` is the nominal
video plus parity budget; at 24 Mbps with 20% FEC, NVENC gets 20 Mbps. Both
values are logged. Headers and integer shard rounding add overhead. File-only
capture has no FEC and uses the full bitrate. Replay/synthetic sources add
parity to their existing source rate. See [coding and tests](../../docs/research/fec.md).

Unrepaired loss requests an IDR and keeps complete following frames flowing
with `damaged=1`. A matching dumped AU is not necessarily a correct reference
picture; use clean/repaired frames after a key for pixel comparisons.

## PC pixel check

For exact crop/pad checks, select `--no-scale`: first use 1920x1080, then
1280x720 for padding and 2560x1440 for cropping. These tests keep the default
1920x1080 output; `pattern.bat` and `vmtest.bat` set `--no-scale` explicitly.

```bat
llstream_win.exe --pattern --no-scale --pattern-width 1280 --pattern-height 720 --frames 120 --output pattern.hevc --profile-to 192.168.185.100:47801
ffprobe -v error -show_entries stream=width,height,pix_fmt,color_range,color_space,color_transfer,color_primaries,chroma_location -of default=nw=1 pattern.hevc
ffmpeg -v error -i pattern.hevc -frames:v 1 -pix_fmt nv12 -f rawvideo decoded.nv12
```

Bring `decoded.nv12`, the HEVC and logs to the VM, then:

```sh
build/host/test_color_math --reference expected.nv12 1280 720
python3 tests/compare_nv12.py expected.nv12 decoded.nv12
```

Expect 8-bit 1080p, limited range, BT.709 and left chroma. The comparison reports
per-plane error, bias and PSNR; its default per-plane MAE limit is 4 code values
for lossy HEVC. Inspect padding (Y=16, U/V=128), full bars and single-pixel edges
as well. This is a diagnostic gate, not a promise of a specific lossy error.
`test_hlsl.exe` compiles all three shaders without needing a D3D device. Host
math tests alone do not validate GPU plane writes, NVENC colour or display CSC.
