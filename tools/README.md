# Receiver runners and analysis

## Receiver on this VM, without the box

```sh
python3 tools/run_receiver.py --local --dump --profile --seconds 120
python3 tools/run_receiver.py --local --profile --seconds 60
python3 tools/run_receiver.py --local --dump --seconds 120 --dry-run
```

`--local` builds/runs the Linux host receiver in `--no-decode` mode and uses no
ADB. It configures `build/host` if needed. Results go to
`build/results/<UTC stamp>-vmrx/`: `run.log`, `run.json`, and `status`, plus optional
`stream.hevc`. `--profile` starts the TCP collector on port 47801 and saves
`rx.csv`/`packets.csv`, plus `sender.csv`/`clock.csv` when PC profiles arrive. It waits for UDP readiness before
printing "run vmtest.bat" and a matching command for a single clean run:

```bat
vmtest.bat 192.168.185.100
bin\llstream_win.exe --host 192.168.185.100 --port 47800 --pattern-motion 8 --seconds 20 --bitrate-mbps 20 --profile-to 192.168.185.100:47801
```

`vmtest.bat` runs clean, 1% loss, and 1% loss with 20% FEC, for 20 seconds each
on port 47800. The single-run command uses the receiver's selected port. Both
send without pacing: budget-rate pacing spreads each frame across much of a
frame period. Local and box receivers run indefinitely by default. `--seconds N`
is available only when deliberately requesting a timed diagnostic run.

Start the PC command while the receiver is running. `--local-address` changes
the address printed in that command; it does not configure networking. The
native receiver binds its UDP port on all IPv4 interfaces. Ctrl-C asks it to
stop, drain pending AUs and flush logs. The runner enforces a shutdown deadline
and saves the process status on failure. This exercises the Windows encoder and
UDP/feedback/recovery path; the box's Wi-Fi, MPP and display are absent.

`--dump` requires `--seconds N` or `--frames N` in the runner and native
receiver. Plain streaming still has no duration limit.
Completed box runs retain the newest five directories; live runs and dumps
that have not been pulled successfully are preserved. Unlimited sessions check
device status every 30 seconds.

`--dump` enables native `llstream_rx --dump FILE.hevc`: create/truncate once,
then append complete AUs in receiver delivery order, including frames with
possibly damaged references after loss. `rx.csv` marks these as `damaged=1`.
Incomplete frames are omitted; only initial/reset key wait skips complete
non-key frames. Each new session resumes at a keyframe. Write/close errors fail
the run. Use a real HEVC sender for dumps:
`llstream_send --synthetic-mbps` payloads are not video. Dumping adds file I/O on
the receive path, so leave it off for latency baselines. The option is restricted
to `--no-decode` and is also available to the box runner.

The normal assembly deadline is 20 ms. `--loss-timeout-ms` can override it for
functional VM tests; the saved command records the value. The byte-equality test
uses 1 s to tolerate VM scheduling. `--binary`/`--no-build` select an existing
host receiver; `--local --display` is rejected.

## Checking a Windows vmtest run

Start the VM receiver with `--local --dump --profile --seconds 180`, then
run `vmtest.bat 192.168.185.100` on the PC. Let the receiver finish or stop it
with Ctrl-C so its dump and logs are flushed. Check the two completed folders:

```sh
cmake --build build/host --target test_color_math
python3 tools/check_vmtest.py build/results/<stamp>-vmrx /path/to/logs/<stamp>-vmtest \
  --json build/results/vmtest-check.json
```

This uses Python's standard library, FFmpeg/ffprobe and the existing C++ pattern
reference (`--reference PATH` selects another host build). Each available
`run-{clean,loss,fec}.log` identifies the sender session. The checker reads joined
`sender.csv` and `clock.csv` from PC_DIR, RX_DIR (the local runner's collector),
or per-case `clean/`, `loss/`, `fec/` profile folders. Old sidecars remain readable.
A folder with only the clean session works. RX_DIR supplies `rx.csv`,
`packets.csv`, `stream.hevc` and, when available, `run.log`. All dumped sessions
must have PC logs so their pictures can be identified.

Each session reports sent/delivered/lost/damaged/repaired frame counts, injected
versus other missing packets, recovery IDRs, pixels and stage timing. Damaged
counts mean delivered AUs with possibly broken references. Recovery IDRs are
successful stage rows with `key=1` after frame zero: the Windows sender uses an
infinite GOP and forces subsequent IDRs for recovery. For each unrepaired loss,
the checker finds the next clean delivered key in that session. Its interval
starts at the lost frame's first socket submission (encoded time if every packet
was injected away) and ends at the key AU's completion, mapped to the sender
clock with the analyzer's offset/uncertainty rules. A missing clock leaves the
interval unavailable; a run ending before recovery is reported explicitly.

The pattern offset uses the stage index (`convert_pattern` in
`sender/win/pipeline.cpp`), which matches the wire frame number from zero,
recovery IDRs included. The checker verifies consecutive stage and wire frame
numbers plus matching sizes/capture/encode timestamps, so frame F uses the
reference at `(F * 8) % 1920`.

Picture mapping is checked in bytes: the delivered CSV order and sender AU
sizes must match FFmpeg's parsed packet offsets, sizes and key flags exactly.
Each decoded picture's `pkt_pos`/`pkt_size` identifies its AU; decoder omissions
cannot shift later references. Reordered/B-frame streams and omitted clean AUs
fail the check. Omitted damaged AUs are counted separately. FFmpeg then compares
the decoded pictures against `test_color_math --reference` with those offsets,
using identical decoder settings and verifying the output count. There is no
scaling. Clean pictures report minimum Y PSNR; damaged pictures report Y/U/V
distributions. `inf` means pixel equality, including in JSON. Quality is reported
numerically, without an invented pass threshold. JSON also contains each
picture's mapping/PSNR and decoder diagnostics.

The checker streams references to FFmpeg and caches at most 240 NV12 images
(about 747 MB of temporary disk space for a full motion cycle); it does not save
a full raw decode. Temporary files are removed after the check. VMware NAT
measurements are **not Wi-Fi, MPP or display latency**.

The `vmtest` CTest builds eight lossless pattern AUs, sends three sessions through
the real host sender/receiver and a deterministic drop proxy, and checks a
damaged stretch, recovery and FEC repair. Only Windows pattern metadata/stage
sidecars are synthesized. The proxy suppresses RECOVER so replay follows the
fixed pattern indices until its pre-encoded key; its loss counts as "other"
missing packets because the sender did not inject it. This is a functional
fixture, not a capture/encode latency measurement.

## Running a receiver on the box

These commands run from the development VM when root ADB is available. They do
not launch anything on the PC or modify Wi-Fi/HDMI settings. The PC sends to the
box's printed `wlan0` address, not to the `127.0.0.1:5555` ADB tunnel.

```sh
python3 tools/run_receiver.py --no-decode --profile --seconds 60
python3 tools/run_receiver.py --display
```

The runner builds `build/android/llstream_rx`, pushes it into a unique directory
under `/data/local/tmp/llstream-rx/`, and starts a detached device-side session.
`--no-build` uses the existing binary; `--binary PATH` selects a prebuilt ARM64
ELF instead. The configured Android build must already have the box's vendor
libraries. `--serial`, `--port`, `--interface`, and `--results` override defaults.
The receiver has no duration or idle limit by default, including between sender
sessions. Use `--seconds N` explicitly for a timed diagnostic (0 means unlimited).

The receiver detects FEC from each frame's header; no runner setting is needed.
To compare recovery, add `--fec-percent 20 --fec-min 1` to the PC sender command,
keeping its nominal video/parity budget inside the link budget. For a deliberate
loss run use `--drop-rate 0.01`; damaged pictures continue instead of key-wait
freezes. Offline dry-run tests cannot validate vendor MPP concealment or display.

- `--no-decode` does not query or alter Android display services or DRM. It
  collects `run.log`, status files, and a local `run.json` manifest.
- `--profile` opens a loopback TCP service on the box, forwards it through ADB
  to a dynamically allocated local port, and saves `rx.csv` and `packets.csv`
  through the collector. It removes only its own forward on exit. Recording is
  fixed-memory and performs no file I/O on the receive thread; TCP uses another
  thread. Profile traffic still consumes CPU/ADB bandwidth, so compare runs
  with profiling off. See [profiling](../docs/PROFILING.md).
- `--display` also records DRM state before, during, and after the session;
  per-frame flip times are available in the optional `rx` profile table. It requires root, temporarily stops SurfaceFlinger/HWC,
  and restores only services that were previously running. The framework is
  restarted when needed to recreate its surfaces on this firmware. This closes
  Android apps. A device-side display lock prevents overlapping LLStream tests.
- Unlimited sessions have no lifetime timer. Explicit `--seconds` tests use a
  watchdog with 10 seconds for shutdown, then TERM and KILL 3 seconds later.
  The session's exit trap waits for that process before restoring Android and
  ignores further signals during cleanup. It works independently of host/ADB.
  Restoration is checked on the device for up to 60 seconds; active scanout is
  required only when HDMI reports connected. Otherwise the display check is
  recorded as unavailable in `restoration-display-check.txt`.

Results go to `build/results/<UTC stamp>-rx/`. The script and manifest are saved
before launch; failure logs are pulled too. Missing artifacts
are recorded explicitly in `run.json`. Device logs are retained; `stream.hevc`
is deleted only after its pulled size matches the device copy.
Ctrl-C stops a local VM receiver. For Android it only stops host monitoring; the
detached receiver keeps running. The device-side session restores Android when
the receiver exits or the session shell receives TERM. Display locks record the session PID and boot
ID; the next run logs and removes a lock from an earlier boot or a dead process.
Unknown lock metadata is left for inspection.

`show_hevc.py` uses the same session generator for its local HEVC playback test
and reads its `restoration-status` instead of checking services again from the host.
Its `--keep-60hz` option is opt-in; the receiver runner does not change
persistent HDMI settings.

For review without ADB or an installed binary:

```sh
python3 tools/run_receiver.py --display --seconds 30 --dry-run
python3 tools/run_receiver.py --no-decode --dry-run
python3 tests/test_receiver_runner.py
```

Dry-run prints the build/ADB commands, repeated poll/reconnect/collection paths,
and the complete generated device script. It executes no commands and creates no
results directory. The offline tests run `sh -n` and simulate Android service
commands to check success, failure, stubborn receivers, session signals, stale
locks, disconnected HDMI, and preservation of stopped services.

## Offline stream analysis

```sh
python3 tools/analyze_stream.py --sender sender.csv --receiver rx.csv \
  --receiver-log run.log --joined joined.csv --json stream.json
python3 tools/analyze_probe.py --sender sender.csv --receiver rx.csv \
  --receiver-log run.log --packets packets.csv --json probe.json
```

Both use only Python's standard library and read sibling `clock.csv` by
default (`--clock PATH` overrides; old `.clock.csv` companions remain supported). Sender and clock CSVs require a decimal
`session` column. `--session ID` selects a run from a combined collection. Frames are joined by session and frame, with wrap and
coalesced loss spans supported. Mismatched capture timestamps and overlapping
rows fail instead of silently producing latency numbers. Measurement definitions
are always in JSON; add `--verbose` to print them too.

The stream report gives median/p95/p99/max and sample count for each stage,
fates, and repeated vblanks between logged flips. `sent` means the first socket
submission, with first-to-last submission duration reported separately. Capture
means the content presentation time, so idle repeats retain their content age.
Flip means the DRM event, not light from the projector. A no-decode run has no
capture-to-flip or decode samples; missing stages remain unavailable.

Summary and joined rows also carry `damaged`, `frames_repaired`, and
`fragments_repaired`. Damage is independent of fate: a shown/delivered picture
may have missing references. Legacy logs lacking these fields report unknown
status, rather than claiming clean frames. The flags do not measure pixel quality.

Receiver-to-sender mapping uses the minimum-RTT sample within **±5 seconds of
transmission**, including future samples in this offline analysis. Change the
radius with `--clock-window`. Offset is receiver minus sender; subtract it to
map receiver times. Per-frame joined output records the chosen offset, sample
age/RTT, and uncertainty. The uncertainty estimate is RTT/2 + 1 µs timestamp
quantization + the receiver's logged clock-pair uncertainty + sample age times
`--drift-ppm` (50 by default). The drift allowance is an explicit assumption,
not a measured bound. No nearby sample means no cross-clock interval. Negative
intervals are retained. JSON records missing clock-pair evidence and timestamp
fallbacks/steps; the console labels clock domains. Quantiles use linear
interpolation over all available samples, including warmup.

## Probe interpretation

Use a **successful, complete sender run** and a receiver trace covering it.
`packets.csv` records every valid VIDEO datagram before assembly, with session,
sequence, fragment index/count, monotonic receive time, timestamp fallback and
cumulative socket-drop counter. It includes late/duplicate packets. The
analyzer deduplicates fragments and reconstructs expected packet order from the
sender CSV, so missing first/last packets, whole frames, and losses across frame
boundaries are counted. Bursts never cross sessions; arrival reordering is
reported separately. Unknown trace frames are counted as ignored. Without a
packet trace, exact loss/burst metrics stay unavailable: receiver frame counts
cannot distinguish missing packets from late arrivals rejected by assembly.

The default 20 ms AU deadline starts at the first observed packet. For FEC, an
AU is ready when all data arrives or every interleaved block has k shards;
optional parity arriving later does not make a clean frame late. The analysis
separates missing data/parity, actual repairs, trace-only reconstructibility,
and frames that met the deadline only through FEC. Packet readiness excludes
RS computation and retirement caused by a later completed frame; receiver fates
remain the actual pipeline outcomes. Whole-frame outages have no first arrival.
Delivery spread includes unique arrivals; partial-frame spreads are lower bounds.
Socket drops
are the last delivered `SO_RXQ_OVFL` counter, not a Wi-Fi loss measurement.
Deliberate `--drop-rate` omissions are separated from unexplained missing packets;
these runs produce no Wi-Fi/FEC decision.

The final sentence chooses a **next experiment**, without estimating a parity
percentage or budget. The user's fluency policy excludes retransmission.

- No observed loss supports no FEC. Socket overflow calls for fixing receive
  capacity first; a missing packet trace calls for more evidence.
- Data loss or a deadline saved by FEC supports comparing FEC on and off.
- Injected loss, whole-frame outages, a missing packet trace, or socket overflow
  produce no decision. None of these conclusions requests retransmission or
  waits on a lost frame.

Loopback runs validate the tools, not the PC-to-box Wi-Fi path. Tests:

```sh
ctest --test-dir build/host -R 'receiver_runner|analysis' --output-on-failure
```

`analysis_loopback` records real no-decode logs with zero and 12% injected loss,
plus 20% FEC with 1% loss,
under `build/results/h5-analysis-loopback/<stamp>/`. Its 1 s assembly timeout
tolerates VM scheduling; the analyzer still evaluates a 20 ms deadline, covered
by deterministic tests alongside clock selection, wrap, loss spans, reordering,
late packets and recovery decisions. Host tests also verify that packet tracing
does not fabricate decode/display timestamps.
