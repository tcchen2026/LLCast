# LLCast design

How the streaming code is put together. The plan is in `PLAN.md`; the wire format
is defined in `src/common/llstream_proto.h`, and that header is authoritative.

## Source layout

| Path | Contents | Builds for |
| --- | --- | --- |
| `src/common/` | Portable C11: wire format, reassembly with FEC, the shared receiver session, HEVC/H.264/AV1 access-unit splitting, frame policy, profiles, clock and socket shims | all |
| `src/` | Box/rooted-Android modules (`video_decoder` over the MPP plugin or dav1d, `drm_display`, `scanout_buffer`, `waiting_frame`, `drm_precheck`) and programs (`llstream_rx`, `hevc_decode_test`) | Android arm64 |
| `sender/` | `lls_sender`: the protocol session every sender shares; `llstream_send`: replay and probe sender | host, mingw, box |
| `sender/win/` | C++17 Windows sender: DDA capture, NV12 conversion, NVENC or Media Foundation, `llstream_win`; `llcast/`: the `LLCast.exe` window | mingw |
| `receiver/win/` | Windows receiver: Media Foundation D3D11 decoding and presentation | mingw |
| `mac/` | macOS app, both roles: ScreenCaptureKit, VideoToolbox, Metal | macOS |
| `android/` | The APK: Java UI, JNI sender and Normal receiver, root session helper | Android |
| `tests/` | Host unit tests, Python runner tests and Windows tests under Wine | host |

## Receiver threads and ownership

```text
shared RX:      poll(socket + wake, assembler deadline) -> recvmmsg -> lls_read_* -> assembler
                  PING -> PONG straight away; RECOVER and loss reports sent from here
                  FRAME outputs -> decode queue (bounded FIFO, ownership moves)
decode thread:  lls_rx_next -> video_decoder_put -> video_decoder_get -> drm_display_submit
                  frees each AU once its picture (same pts) is returned
display thread: owned by drm_display: packs CPU pictures while a flip is pending,
                  non-blocking commits, flip events
                  on_frame callback -> lls_rx_finish -> shared RX report
```

`lls_rx_session` owns the socket, assembly, bounded AU queue, recovery, frame
reports and optional profile tables on every receiver: `llstream_rx`, the
APK's Normal receiver, Windows and Mac. Linux/Android use batched kernel
timestamps; other platforms use the portable socket path. It receives and
parses outside its lock, and emits reports and profile rows after unlocking.
No-decode probes consume complete AUs on the RX thread, avoiding an artificial
decoder queue. A clean drain-stop delivers the remaining decode queue; abort
cancels it. Windows reports a completed present as DELIVERED; Android Normal
reports SHOWN on release to its surface. Neither has a flip timestamp.

`video_decoder` loads the optional Rockchip plugin `libllcast_mpp.so` (HEVC)
only when `/dev/mpp_service` exists, so tablets can run with dav1d AV1 alone.
MPP pictures are scanned out without a copy; dav1d's I420 pictures are packed
(YUYV and rotation where the device needs them) by the display worker.

- **Ownership moves, nothing is shared.** AU buffers pass from the assembler to the
  queue to the decoder. `DisplayFrame`s pass from the decoder to the display. The only
  shared state is the queue, the display's ready slot, and the peer address used
  for sending feedback.
- **Keep moving, never drop silently.** Frames after an unrepaired loss are
  decoded and shown, flagged damaged, while RECOVER repeats until a keyframe
  arrives. If the decode queue is full, the oldest AU is dropped as LOST and
  recovery is requested. Only a decoder failure (error, 100 ms without output,
  or a picture for the wrong AU) resets the decoder; the rx thread then calls
  `lls_assembler_resync()` after flushing the entire backlog, including queued
  keys, and requests a fresh key beyond the discarded range.
- **No sleeps on the data path.** Receive waits in `poll`, decode in the
  decoder's output timeout, display in `poll` on the DRM fd.
- **Session change.** On RESET the decode thread calls `video_decoder_reset()` before
  the next AU. Local ordinals identify display callbacks across sender restarts;
  reset never reopens the display. Decoded pictures remain alive until their replacing
  flip. Shared completion reclamation is lock-free for the display callback.

## Clock domains

- **Receiver:** nanoseconds of `CLOCK_MONOTONIC` on Linux/Android, QPC on
  Windows, `CLOCK_UPTIME_RAW` on macOS. Kernel receive timestamps
  (`SO_TIMESTAMPNS`, which are `CLOCK_REALTIME`) are converted using
  `realtime - monotonic`, sampled next to each `recvmmsg`. DRM flip events are
  monotonic when `DRM_CAP_TIMESTAMP_MONOTONIC` is 1; check it and log it.
- **Sender:** QPC on Windows, `CLOCK_MONOTONIC` on Linux, `CLOCK_UPTIME_RAW` on
  macOS, in microseconds.
  On Windows `capture_us` is the content's presentation time (DDA
  `LastPresentTime`), not the time it was acquired.
- **Mapping:** the sender maps receiver times with PING/PONG offset estimates. It
  logs each estimate with its RTT, so that end-to-end figures carry their
  uncertainty.

## Logs

Streaming programs write no diagnostic files. `--profile [ADDR:]PORT` (loopback
by default) or `--profile-to HOST:PORT` enables fixed rings and a separate TCP
service thread. `tools/profile.py --save DIR` writes the CSV tables. With neither
flag, no rings or profile sockets are allocated. Exit summaries use fixed storage.
See `docs/PROFILING.md` for capacities, wire format and ADB forwarding.

- **Receivers (`rx`, `packets`):** per frame: fate, packets, `first_rx_ns`,
  `complete_ns`, `queued_ns`, `decode_put_ns`, `decoded_ns`, `ready_ns`,
  `flip_ns`, `flip_sequence`, `capture_us`, `encoded_us`, damage, FEC repairs,
  codec; per packet: receive time, kernel drops, FEC fields.
- **Senders (`sender`, `clock`):** frame, bytes, packets, `capture_us`,
  `encoded_us`, first and last send time, pipeline stages, then the receiver
  report fields mapped to the sender clock; PING/PONG samples.
- **Summaries:** separate warmup from steady state, and give median, p95, p99 and
  max for every stage interval. Count losses, skips, replacements and repeated
  vblanks.

## Sender session (`sender/lls_sender.h`)

The session handles everything a sender does apart from producing frames:
fragmenting, FEC parity, pacing, PING/PONG, RECOVER handling, live reception
status, joined profile records and the summary. Each sender program only
produces `LlsSenderFrame`s and asks `lls_sender_need_keyframe()` before
encoding. The apps send 10% FEC parity, paid from the bandwidth budget.

## Replay and probe sender (`llstream_send`)

- **Replay:** splits an Annex-B HEVC/H.264 or AV1 OBU file (`--codec`) into AUs
  with `lls_split_access_units()`, and marks keyframes as `LLS_FRAME_KEY`.
  Paces them at `--fps` from a fixed schedule, with
  `capture_us` set to the scheduled time. On RECOVER it jumps to the next keyframe
  in the file.
- **Synthetic (`--synthetic-mbps`):** produces frame-sized bursts of random bytes,
  plus key-sized bursts on request. Use it with `llstream_rx --no-decode` for
  network characterisation.
- **Common behaviour:** fragments are sent back-to-back by default, or spread with
  `--pace-mbps`. Sends PING every 100 ms. Publishes the tables above only when profiling is enabled.
- **Portability:** POSIX sockets and Winsock are behind `src/common/lls_net.h`; one
  source file builds for host, mingw and the box.

## Windows sender (`sender/win`)

There is one GPU pipeline thread and one D3D11 device, on the hardware adapter
that owns the captured output. A capture-wait worker only calls DXGI acquisition;
conversion and encoding remain on the pipeline thread:

```text
DdaCapture.acquire -> Nv12Converter.convert -> DdaCapture.release -> VideoEncoder.encode -> lls_sender_send
```

NVENC is preferred, with a D3D11 Media Foundation hardware fallback. Both keep
one frame in flight. MF consumes NeedInput/HaveOutput events; callbacks only
wake the pipeline, which owns the texture, codec controls and output bytes.
Codec selection precedes the sender session and remains fixed until it ends.
See `sender/win/README.md` for driver requirements and the backend comparison.

**Frame policy.** The stream runs at the profile's steady frame rate, driven by
desktop updates:

- **Encode on update:** encode as soon as a desktop update arrives, provided at
  least `period - 2 ms` has passed since the previous encode. An update that
  comes too early is not lost: DDA folds it into the next image.
- **Repeat frame:** if no update arrives within `period + 1 ms`, re-encode the
  last NV12 image. Static content then costs tiny P-frames but keeps the cadence
  steady, which keeps loss detection quick and the box's Wi-Fi out of
  power-save.
- **Recovery:** when `lls_sender_need_keyframe()` is true, the next encode is an
  IDR. It happens immediately, re-encoding the last image if necessary.
- **Capture timestamp:** `capture_us` is DDA's `LastPresentTime`. A repeated
  frame keeps its content's original time, so frame-age statistics stay honest.

**Geometry and colour:**

- The encoded size and rate are the profile's: Low (1280x720 at 30 fps),
  Medium (1920x1080 at 60 fps) or custom (`src/common/lls_stream_profile.h`).
  By default the whole output is scaled to it, aspect kept. "No scale" takes
  the top-left region at full size, black where the desktop is smaller.
- The conversion is BT.709 limited range, and the VUI says so. The box plane
  uses `COLOR_SPACE=3`.
- The desktop must be SDR and unrotated; HDR and the cursor are out of scope.

**Timing and priority:**

- Uses `timeBeginPeriod(1)`, a high-resolution one-shot deadline timer, and
  `THREAD_PRIORITY_HIGHEST` for the pipeline and acquisition threads. It waits on the timer,
  capture completion and RECOVER notification together, without 1 ms polling.
- `src/common/lls_frame_policy.h` owns the C cadence policy shared with other
  senders. Fresh updates use the acquire gate; repeats use the fixed grid.
  The Mac uses the same policy with a one-shot dispatch timer.
- DXGI acquisition cannot be cancelled, so the worker's wait ends at the repeat
  deadline, rounded down with a 1 ms margin (at most 100 ms before the first
  image). A receiver RECOVER can encode the retained private image while
  acquisition is pending. Any subsequently acquired update is converted and
  kept until the next encode gate; DDA need not report it again. Transport
  failures request an IDR at the ordinary cadence, never an immediate repeat.
  GPU context calls never run on the
  worker. Idle waits are capped at 100 ms to observe the UI/console stop flag.
- GPU scheduling priority, `SetGPUThreadPriority`, `HIGH_PRIORITY_CLASS` and
  NvAPI power state are untried phase-5 experiments.

**Joined sender profile:** DDA present time, acquire return, conversion submit,
bitstream locked, last datagram submitted, send return, and the receiver report.
The bounded sender history waits for both the stage update and feedback (or
retirement); it records one row, with no file join. `_us` values use QPC converted
to sender microseconds; `dda_present_qpc` retains raw QPC ticks.

**Launch:**

- The user runs the programs on the PC's logged-in desktop, using `.bat`
  scripts in the shared folder (`/mnt/hgfs/deb13/LLCast` on the VM).
- Each script writes `logs\<stamp>-<test>\` there; we read results from the
  share.
- Never capture or measure through RDP.
