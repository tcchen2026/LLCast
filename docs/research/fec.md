# Scheme 1: Reed–Solomon and validation

Status (2026-10-28): the LLCast apps send 10% parity, paid from the bandwidth
budget (user decision). CLI defaults below are unchanged;
the box repair cost is still unmeasured.

Original implementation: `src/common/lls_fec.c`; no third-party code or runtime
dependency. GF(256) uses polynomial `0x11d`, primitive element 2, and systematic
generator `[I; C]`, where `C[r,c] = inverse(r XOR (m+c))`. Data columns are
0..k-1, parity rows 0..m-1. These choices are wire format, not tuning knobs.
Decode solves the erased-data submatrix and leaves parity unchanged. Encode
allocates no memory; decode allocates at most 254² matrix bytes. Field tables
are local, so simultaneous calls do not share mutable state.

Frames use the header's interleaving: B=ceil(count/128), data i in block i%B,
parity j in j%B. Parity per block is ceil(largest_block_k*percent/100), raised
to `fec_min` and capped at 255−largest_k and the uint16 packet-count limit.
`count+fec_count <= 65535` is required because indices and reports are uint16.
If that limit leaves no room, the sender uses scheme 0 for that frame.

Both senders accept `--fec-percent 0..100` (default 0) and `--fec-min 0..254`
(default 1). Data is sent first; parity is calculated afterwards and follows
under the same pacing rule. No retransmission or extra assembly deadline.
Already-complete frames do not wait for parity. Late parity counts as late.
Repairing a frame costs parity arrival time plus CPU time; it is not zero latency.

Windows `--bitrate-mbps` is the nominal combined video/parity budget: NVENC gets
`budget*100/(100+percent)`. Both rates are logged. Local HEVC-file mode has no
FEC and uses the whole budget. Headers, parity rounding/minimums, and CBR bursts
add overhead; this is not an exact wire-rate limiter. Use 20–24 Mbps nominal
within the 30 Mbps link budget and measure the actual stream. Replay/synthetic
rates describe source bytes and gain parity overhead without being reduced.

Receiver CSV appends `fec_packets,frames_repaired,fragments_repaired` (after
`damaged`); sender CSV appends `fec_packets`. `packets`/`packets_expected` include
data and parity. Received counts describe unique arrivals **at completion**,
not final network loss: missing optional parity can arrive after delivery.
Use the packet trace, which appends `fec_scheme,fec_count`, for network loss.
Wire report size is unchanged; repairs are receiver-side telemetry.

Validation: 15,810 exhaustive erasure patterns (k=1..8, m=0..4), 802 random
blocks through k=128/m=32 plus k=1/m=254, and a fixed vector independently
calculated by polynomial multiplication. Packet tests include last-shard loss,
five consecutive losses, an entire interleaved block, parity-first reorder,
late parity and the subsequent damaged/key-healing path. Replay at 20% FEC and
1% deterministic random injection delivers all 180 AUs byte-for-byte: 63 frames,
79 data fragments repaired, zero lost/skipped/recovery and zero socket drops.
The full 26 tests pass normally and with ASan/UBSan; Android and MinGW link.
Logs and Windows imports: `build/results/h7-fec/`.

## CPU cost (not stream latency)

Single-thread GCC 14.2 `-O3`, VMware guest reporting Ryzen 7 9850X3D; 200
iterations/case, monotonic microsecond clock. Calls only: no sockets, GPU, MPP,
display or allocation of frame storage. Raw output: `benchmark.txt` above.

| Data/parity shards | Missing data | Encode mean, µs | Repair mean, µs |
| --- | ---: | ---: | ---: |
| 37/8 (50,172 data bytes) | 1 | 94.0 | 12.0 |
| 37/8 | 5 | 94.6 | 64.5 |
| 128/26 (173,568 bytes) | 5 | 1082.3 | 210.6 |
| 128/26 | 26 | 1072.5 | 1098.7 |

**Box planning estimate only:** assuming the A53 takes 5–10× this host's repair
time gives 0.06–0.12 ms for one missing fragment in a typical ~50 KB frame,
0.32–0.65 ms for five, and 1.05–2.11 ms for five in a full block. A near-limit
26-erasure block could take 5.5–11 ms under that assumption. This multiplier is
unmeasured, not a prediction or bound; multiblock frames add block work. Measure
on the box before selecting a default. Clean frames bypass the RS decoder.

Reproduce CPU measurements with a Release build of `test_fec --benchmark`.
