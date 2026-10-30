#!/usr/bin/env python3
"""Measure packet loss, bursts, FEC repairs and deadlines; never recommend retransmission."""
import bisect
from collections import Counter, defaultdict
import sys

from analysis_common import (MOD, arguments, csv_rows, emit_json, fate_counts, integer, load,
                             print_stats, quality_counts, session_of, stats)


def gaps(positions, total):
    """Half-open missing ranges; memory/work proportional to observed packets."""
    cursor = 0
    for position in positions:
        if position > cursor:
            yield cursor, position
        cursor = position + 1
    if cursor < total:
        yield cursor, total


def decision(result):
    if result.get("receiver_counters", {}).get("profile_records_missing"):
        return "no decision", "No FEC decision from an incomplete profile collection; reconnect sooner or increase ring capacity."
    if not result["expected_packets"]:
        return "no decision", "No FEC decision: there are no sender packets in this trace."
    if result["injected_drops"]:
        return "no decision", "No Wi-Fi/FEC decision from a run with deliberate packet drops; repeat without --drop-rate."
    if result["missing_packets"] is None:
        return "no decision", "No FEC decision without a packet trace; collect --profile with profile.py --save."
    if result["kernel_socket_drops"]:
        return "no decision", "No FEC decision while the receive socket overflows; fix receive capacity and repeat."
    if not result["missing_data_packets"] and not result["frames_timely_only_with_fec"]:
        return "no FEC", "This trace shows no data loss or deadline saved by FEC, so it does not support adding parity."
    if result["frames_wholly_missing"]:
        return "no decision", "No FEC setting established: whole-frame outages leave parity survival unknown."
    return "FEC", "Observed data loss or timely repair supports comparing FEC on and off on this path."


def ready_time(arrivals, data, parity):
    """Earliest packet arrival permitting completion, excluding RS CPU time."""
    ready = max(arrivals[i] for i in range(data)) if all(i in arrivals for i in range(data)) else None
    if parity:
        blocks = (data + 127) // 128
        block_times = []
        for b in range(blocks):
            k = (data - b + blocks - 1) // blocks
            times = sorted(t for i, t in arrivals.items() if (i if i < data else i - data) % blocks == b)
            if len(times) < k:
                return ready
            block_times.append(times[k - 1])
        fec_ready = max(block_times)
        ready = min(ready, fec_ready) if ready is not None else fec_ready
    return ready


def analyze(args):
    senders, receiver, _, counters, warnings = load(args)
    fates, _ = fate_counts(senders, receiver)
    totals, frames = defaultdict(int), {}
    previous = {}
    injected = expected_data = expected_parity = 0
    for row in senders:
        session, frame, count = row["session"], integer(row, "frame"), integer(row, "packets")
        parity = integer(row, "fec_packets", 0)
        data = count - parity
        blocks = (data + 127) // 128 if data > 0 else 0
        if (not 1 <= count <= 65535 or not 0 <= parity < count or
                (parity and (parity % blocks or (data + blocks - 1) // blocks + parity // blocks > 255))):
            raise ValueError("Invalid sender data/parity packet count")
        if session in previous and (frame - previous[session]) % MOD != 1:
            raise ValueError("Packet analysis requires consecutive sender frames per session")
        previous[session] = frame
        frames[session, frame] = (totals[session], data, parity, row)
        totals[session] += count
        expected_data += data
        expected_parity += parity
        dropped = integer(row, "dropped_packets", 0)
        if not 0 <= dropped <= count:
            raise ValueError("Invalid injected drop count")
        injected += dropped
    received = defaultdict(dict)
    sequence_base, highest = {}, {}
    duplicates = reordered = ignored = fallbacks = trace_drops = 0
    if args.packets:
        for packet in csv_rows(args.packets, ("session", "frame", "sequence", "index", "count", "rx_ns")):
            session = session_of(packet)
            frame = integer(packet, "frame")
            entry = frames.get((session, frame))
            if entry is None:
                ignored += 1
                continue
            start, data, parity, sender = entry
            count = data + parity
            index, sequence, rx = (integer(packet, name) for name in ("index", "sequence", "rx_ns"))
            if (not 0 <= index < count or data != integer(packet, "count") or
                    parity != integer(packet, "fec_count", 0) or
                    integer(packet, "fec_scheme", 0) not in ((1,) if parity else (0, 1)) or
                    not 0 <= sequence < MOD or rx <= 0 or
                    integer(packet, "capture_us", integer(sender, "capture_us")) != integer(sender, "capture_us") or
                    ("frame_bytes" in packet and "bytes" in sender and integer(packet, "frame_bytes") != integer(sender, "bytes"))):
                raise ValueError("Packet trace disagrees with sender (frame/index/data/parity/bytes/capture)")
            position = start + index
            base = (sequence - position) % MOD
            if sequence_base.setdefault(session, base) != base:
                raise ValueError("Packet sequence discontinuity: sender/trace may be truncated or mismatched")
            if position in received[session]:
                duplicates += 1
                received[session][position] = min(rx, received[session][position])
            else:
                reordered += position < highest.get(session, -1)
                highest[session] = max(position, highest.get(session, -1))
                received[session][position] = rx
            fallbacks += integer(packet, "timestamp_fallback", 0)
            trace_drops = max(trace_drops, integer(packet, "kernel_drops", 0))
    expected = sum(totals.values())
    seen = sum(map(len, received.values()))
    missing = expected - seen if args.packets else None
    if missing is not None and missing < injected:
        raise ValueError("Fewer missing packets than injected drops: mismatched or incomplete logs")
    histogram = Counter()
    positions = {}
    for session, total in totals.items():
        positions[session] = sorted(received[session])
        if args.packets:
            histogram.update(end - start for start, end in gaps(positions[session], total))
    spreads, completed_spreads = [], []
    late = incomplete = wholly_missing = missing_data = missing_parity = unassembled = 0
    trace_repairable = timely_fec = 0
    for (session, frame), (start, data, parity, sender) in frames.items():
        count = data + parity
        out = receiver.get(session, frame)
        unassembled += count - integer(out, "packets", 0) if out else count
        if not args.packets:
            first = integer(out, "first_rx_ns", 0) if out else 0
            complete = integer(out, "complete_ns", 0) if out else 0
            if complete and first:
                spread = (complete - first) / 1e6
                completed_spreads.append(spread)
                late += spread > args.deadline_ms
            else:
                incomplete += 1
            continue
        ordered = positions[session]
        subset = ordered[bisect.bisect_left(ordered, start):bisect.bisect_left(ordered, start + count)]
        arrivals = {pos - start: received[session][pos] for pos in subset}
        data_lost = data - sum(i < data for i in arrivals)
        parity_lost = parity - sum(i >= data for i in arrivals)
        missing_data += data_lost
        missing_parity += parity_lost
        times = list(arrivals.values())
        first = min(times) if times else None
        if times:
            spreads.append((max(times) - first) / 1e6)
        ready = ready_time(arrivals, data, parity)
        data_ready = ready_time({i: t for i, t in arrivals.items() if i < data}, data, 0)
        deadline = first + args.deadline_ms * 1e6 if first is not None else None
        timely_fec += ready is not None and ready <= deadline and (data_ready is None or data_ready > deadline)
        if ready is None:
            incomplete += 1
        else:
            spread = (ready - first) / 1e6
            completed_spreads.append(spread)
            late += spread > args.deadline_ms
            trace_repairable += data_lost > 0
        wholly_missing += not times
    if not args.packets:
        warnings.append("Without a packet trace, packets absent from assembled AUs are NOT an exact packet-loss count (late optional parity is normal).")
    if ignored:
        warnings.append(f"Ignored {ignored} packet trace rows outside the sender's logged sessions/frames.")
    if injected:
        warnings.append("Loss includes deliberate --drop-rate omissions; this run tests tools/recovery, not Wi-Fi quality.")
    if "kernel_drops" not in counters:
        warnings.append("No receiver socket-drop summary; trace counters are only the last observed SO_RXQ_OVFL value.")
    result = {
        "sessions": sorted(totals), "frames": len(senders), "fates": fates,
        **quality_counts(senders, receiver),
        "expected_packets": expected, "expected_data_packets": expected_data, "expected_parity_packets": expected_parity,
        "observed_unique_packets": seen if args.packets else None,
        "missing_packets": missing, "missing_data_packets": missing_data if args.packets else None,
        "missing_parity_packets": missing_parity if args.packets else None,
        "packet_loss_rate": missing / expected if missing is not None and expected else None,
        "injected_drops": injected, "unexplained_missing_packets": missing - injected if missing is not None else None,
        "packets_not_in_assembled_rows": unassembled,
        "burst_length_histogram_packets": dict(sorted(histogram.items())) if args.packets else None,
        "duplicate_packets": duplicates if args.packets else None, "out_of_order_packets": reordered if args.packets else None,
        "frames_late": late, "frames_incomplete": incomplete, "frames_wholly_missing": wholly_missing if args.packets else None,
        "frames_missing_deadline": late + incomplete, "deadline_ms": args.deadline_ms,
        "frames_repairable_from_trace": trace_repairable if args.packets else None,
        "frames_timely_only_with_fec": timely_fec if args.packets else None,
        "delivery_spread_all_observed_frames": stats(spreads, domain="receiver CLOCK_MONOTONIC, first -> last unique packet, including optional parity"),
        "delivery_spread_complete_frames": stats(completed_spreads, domain="receiver CLOCK_MONOTONIC, first packet -> enough data/FEC shards; excludes repair CPU"),
        "kernel_socket_drops": counters.get("kernel_drops", trace_drops if args.packets else None),
        "packet_timestamp_fallbacks": fallbacks if args.packets else None,
        "receiver_counters": counters, "warnings": warnings,
        "definitions": ["Packet loss = absent unique data/parity fragments / expected fragments in this recorded run, including injected omissions.",
                        "Bursts count consecutive missing VIDEO packets across frames, never across sessions; reordering is not loss.",
                        "Leading/trailing missing runs are bounded by the sender CSV; losses outside this capture window are unknown.",
                        "The 20 ms deadline starts at the first observed packet. Entirely missing AUs have no observable start.",
                        "Late = data or k shards/block become available after deadline. Incomplete = never enough; optional parity is not required.",
                        "Trace readiness excludes repair CPU and earlier retirement caused by a later completed frame; actual fates and repair counts come from rx.csv.",
                        "SO_RXQ_OVFL is cumulative socket loss last delivered with a datagram, not Wi-Fi loss.",
                        "Fluency first: never retransmit or wait for a lost frame. Use timely FEC or continue damaged with an IDR request.",
                        "The conclusion selects a follow-up experiment, not a parity percentage or budget; injected loss is not Wi-Fi evidence."]}
    result["recommendation"], result["recommendation_reason"] = decision(result)
    return result


def main(argv=None):
    args = arguments(__doc__, argv, probe=True)
    result = analyze(args)
    emit_json(args.json, result)
    print(f"Packets: expected={result['expected_packets']} observed={result['observed_unique_packets']} "
          f"missing={result['missing_packets']} data_missing={result['missing_data_packets']} "
          f"parity_missing={result['missing_parity_packets']} injected={result['injected_drops']} loss_rate={result['packet_loss_rate']}")
    print("Consecutive missing-packet burst histogram (length: runs):", result["burst_length_histogram_packets"])
    print(f"Frames against {args.deadline_ms:g} ms: late={result['frames_late']} incomplete={result['frames_incomplete']} "
          f"total={result['frames']}; kernel socket drops={result['kernel_socket_drops']}")
    print(f"Receiver recorded damaged={result['damaged_frames']} repaired_frames={result['frames_repaired']} "
          f"repaired_fragments={result['fragments_repaired']} (unknown repair status={result['repair_status_unknown_frames']}).")
    print_stats("Delivery spread to enough shards", result["delivery_spread_complete_frames"])
    print_stats("Delivery spread, all observed packets", result["delivery_spread_all_observed_frames"])
    for line in (result["definitions"] if args.verbose else []) + result["warnings"]:
        print(line)
    print(result["recommendation_reason"])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        sys.exit(1)
