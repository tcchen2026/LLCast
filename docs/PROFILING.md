# Profiling

Streaming diagnostics are **off by default**. Native streaming programs never
write per-frame diagnostic files. The timed `hevc_decode_test` retains its
explicit CSV output; `--dump`/`--output` recordings still require a frame or
duration limit.

```sh
# VM collector, reachable from the PC; leave running across sender restarts.
python3 tools/profile.py --listen 192.168.185.100:47801 --save build/results/pc-profile
# On the PC (the ordinary stream.bat enables no profiling):
bin\llstream_win.exe --host BOX_IP --profile-to 192.168.185.100:47801
# Windows window (both roles); Send profiles from Start, Receive while waiting:
LLCast.exe --profile-to 192.168.185.100:47801
# Mac window (both Send and Receive), or Terminal sender:
open -a LLCast --args --profile 47801
/Applications/LLCast.app/Contents/MacOS/LLCast --send BOX_IP --profile-to VM_IP:47801

# Local receiver and collector together; also accepts the vmtest PC profiles.
python3 tools/run_receiver.py --local --profile --dump --seconds 120
```

The collector also binds loopback by default. Specify the VM address in
`--listen ADDR:PORT` for PC connections. A malformed connection is discarded
without stopping other collectors.

Each native program accepts one of `--profile [ADDR:]PORT` (loopback by default)
or `--profile-to HOST:PORT` (outgoing, retries every second), plus optional
`--profile-capacity N`. Listeners support four readers; anything sent by readers
is discarded. Profiling is plain TCP; use a trusted interface or tunnel.

The collector prints service summaries and, with `--save DIR`, writes `rx.csv`,
`packets.csv`, `sender.csv`, and `clock.csv` for the tables present. It appends to
matching schemas and records stream headers, summaries and skips in
`profile.jsonl`. Use a fresh directory for a new measurement. Receiver fate
numbers are saved as their existing names (`shown`, `lost`, etc.). The sender
table includes transport, receiver feedback and pipeline stages in one row;
there are no stage/clock sidecars to join after shutdown. A file-only encode
uses session zero and has no receive/send timestamps.

## Box through ADB

When the ADB tunnel is available:

```sh
python3 tools/run_receiver.py --display --profile
```

The runner starts the service on box loopback port 47801, allocates an ADB
forward with `adb -s 127.0.0.1:5555 forward tcp:0 tcp:47801`, then runs the collector
against that forwarded port. CSVs are saved on the VM. It reinstalls that forward after reconnecting ADB and removes only that
forward on exit; the detached display session still owns restoration. Use
`--profile-port` to select another TCP port. `--dry-run` prints these steps without
executing them. This does not enable profiling in an already running APK session.

For a receiver already launched with `--profile 47801`, forward that port and run
`python3 tools/profile.py 127.0.0.1:FORWARDED_PORT --save DIR`. Profiling over ADB
still uses the box's Wi-Fi; compare timings with profiling disabled.

## Records, retention and clocks

Recording copies fixed int64 columns under a short mutex into preallocated rings;
no allocation, file I/O or blocking network I/O occurs there. A separate thread
waits on sockets and a coalesced wakeup. Default capacities are 3,600 frame rows,
180,000 packet rows and 600 clock samples (roughly 60 s at 60 fps, 3,000 packets/s,
and ten pings/s). An explicit capacity overrides every table; startup rejects
allocations above 256 MiB. With profiling off these rings do not exist.

Readers receive retained history then live records. Slow readers never block
the producer; `#skipped TABLE N` reports overwritten rows. Shutdown gives connected
readers at most 250 ms to drain. An absent collector or abrupt termination may
lose the tail. Treat incomplete collections as incomplete measurements. The analyzers warn
when `profile.jsonl` reports skipped or truncated records; the probe tool then
withholds a FEC recommendation rather than interpreting collector loss as Wi-Fi loss.

The version-1 text stream starts with `#llcast-profile 1 program=... clock=...`
and an `instance` identifier, followed by `#table NAME col1,col2,...`,
`#cursor NAME SEQUENCE`, then `NAME,v1,v2,...`. `#summary` appears every ten
seconds. Instance/cursor metadata lets the collector suppress repeated history
on reconnect (up to 128 recent eight-table instances). Cursors are saved in
`profile.jsonl`; a late start or reconnect beyond the last collected sequence
creates a `#skipped` event, including history lost before the first connection.
Half-closed readers (for example `nc -N` with empty stdin) still receive output.
It never sends commands.

Receiver stage times are nanoseconds of `CLOCK_MONOTONIC` on Linux/Android, QPC
on Windows and UPTIME_RAW on macOS. Linux kernel receive
timestamps start in REALTIME and are converted using paired clock samples;
fallbacks, clock steps and conversion uncertainty appear in `run.log`. Sender
times are QPC microseconds on Windows, MONOTONIC microseconds on Linux, and
UPTIME_RAW microseconds on macOS.
The Mac sender's acquire time is entry into its ScreenCaptureKit image callback;
conversion occurs inside ScreenCaptureKit, so `conversion_submit_us`,
`accumulated` and `dda_present_qpc` are zero/unavailable there. Repeats keep
their content's present/acquire times; `encode_start_us` belongs to each encode.
Raw Windows `dda_present_qpc` is in QPC ticks. PING/PONG
clock rows supply offset/RTT; the tools carry mapping uncertainty. A DRM flip
timestamp is not projector light output, and no-decode probes cannot measure
capture-to-display latency.

`analyze_stream.py` and `analyze_probe.py` read the collected tables and sibling
`clock.csv`; `--session ID` selects one sender in a combined collection.
`check_vmtest.py` matches each `run-{clean,loss,fec}.log` session to joined sender
records in the PC or receiver collection. Keep these console logs with the
profiles because they identify the pattern and any kernel drops.
