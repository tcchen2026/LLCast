#!/usr/bin/env python3
"""Join stream stages by session/frame and estimate capture-to-flip latency."""
from collections import defaultdict
import csv
import sys

from analysis_common import (MOD, arguments, emit_json, fate_counts, integer, load,
                             print_stats, quality_counts, quality_row, session_of, stats)

SENDER = "sender monotonic (QPC-derived on Windows), us -> ms"
RECEIVER = "receiver CLOCK_MONOTONIC, ns -> ms"
MAPPED = "receiver mapped to sender monotonic using minimum-RTT offset"


def analyze(args):
    senders, receiver, clocks, counters, warnings = load(args)
    intervals = defaultdict(list)
    definitions = {
        "capture_to_encoded": ("capture_us", "encoded_us", SENDER),
        "encoded_to_sent": ("encoded_us", "sent_us", SENDER),
        "sent_to_first_rx": ("sent_us", "first_rx_sender_us", MAPPED),
        "first_rx_to_complete": ("first_rx_us", "complete_us", RECEIVER),
        "complete_to_decoded": ("complete_us", "decoded_us", RECEIVER),
        "decoded_to_flip": ("decoded_us", "flip_us", RECEIVER),
        "first_to_last_send": ("sent_us", "last_send_us", SENDER),
        "capture_to_complete": ("capture_us", "complete_sender_us", MAPPED),
        "capture_to_flip": ("capture_us", "flip_sender_us", MAPPED),
    }
    joined, mappings = [], []
    missing_clock, missing_receiver = 0, 0
    pair_us = counters.get("offset_pair_uncertainty_ns", 0) / 1000
    for sender in senders:
        session, frame = sender["session"], integer(sender, "frame")
        received = receiver.get(session, frame)
        missing_receiver += received is None
        row = {"session": session, "frame": frame, "fate": received["fate"] if received else "unreported"}
        row.update(quality_row(received))
        for name in ("capture_us", "encoded_us", "last_send_us"):
            row[name] = integer(sender, name) or None
        row["sent_us"] = integer(sender, "first_send_us") or None
        # Capture may be deliberately repeated old content. Select a clock
        # near transmission, not near that older content's presentation.
        anchor = row["sent_us"] or row["encoded_us"]
        clock = clocks.choose(session, anchor, args.clock_window * 1e6, args.drift_ppm, pair_us) if anchor else None
        if clock:
            mappings.append(clock)
        else:
            missing_clock += 1
        for name in ("first_rx", "complete", "decoded", "flip"):
            value = integer(received, name + "_ns", 0) if received and integer(received, "span", 1) == 1 else 0
            row[name + "_us"] = value / 1000 if value else None
            row[name + "_sender_us"] = value / 1000 - clock["offset_us"] if value and clock else None
        row["offset_us"] = clock["offset_us"] if clock else None
        row["offset_uncertainty_us"] = clock["uncertainty_us"] if clock else None
        row["clock_sample_age_us"] = clock["age_us"] if clock else None
        row["clock_sample_rtt_us"] = clock["rtt_us"] if clock else None
        for name, (start, end, _) in definitions.items():
            value = (row[end] - row[start]) / 1000 if row[start] is not None and row[end] is not None else None
            row[name + "_ms"] = value
            if value is not None:
                intervals[name].append(value)
        joined.append(row)
    fates, recorded_fates = fate_counts(senders, receiver)
    sessions = {row["session"] for row in senders}
    flips = sorted((integer(row, "flip_ns", 0), integer(row, "flip_sequence", 0))
                   for row in receiver.rows if session_of(row) in sessions and row["fate"] == "shown"
                   and integer(row, "flip_ns", 0))
    repeated = 0
    for before, after in zip(flips, flips[1:]):
        step = (after[1] - before[1]) % MOD
        if 1 < step < MOD // 2:
            repeated += step - 1
    result = {
        "sender_frames": len(senders), "sessions": sorted(sessions),
        "matched_frames": len(senders) - missing_receiver, "fates": fates,
        **quality_counts(senders, receiver),
        "receiver_fates_in_selected_sessions_span_weighted": recorded_fates,
        "repeated_vblanks_between_logged_flips": repeated,
        "repeated_vblanks_receiver_log_all_sessions": counters.get("repeated_vblanks"),
        "intervals": {name: stats(intervals[name], domain=definition[2]) for name, definition in definitions.items()},
        "clock": {"mapped_frames": len(mappings), "unmapped_frames": missing_clock,
                  "invalid_samples": clocks.invalid, "window_radius_seconds": args.clock_window,
                  "assumed_drift_ppm": args.drift_ppm,
                  "receiver_clock_pair_uncertainty_us": counters.get("offset_pair_uncertainty_ns", 0) / 1000
                       if "offset_pair_uncertainty_ns" in counters else None,
                  "offset_us": stats([p["offset_us"] for p in mappings], "us", "receiver minus sender"),
                  "uncertainty_us": stats([p["uncertainty_us"] for p in mappings], "us", "offset estimate +/-"),
                  "sample_age_us": stats([p["age_us"] for p in mappings], "us", "sender monotonic"),
                  "rtt_us": stats([p["rtt_us"] for p in mappings], "us", "four-timestamp elapsed-time difference")},
        "receiver_counters": counters, "warnings": warnings,
        "definitions": ["sent = first non-injected socket submission; packet transmission overlaps reception.",
                        "capture = desktop-content presentation (or replay schedule), including repeated old content.",
                        "flip = DRM event timestamp; this excludes projector processing and light emission.",
                        "Damaged means references may be missing; it is independent of shown/delivered fate, not a pixel-quality measurement.",
                        "Legacy logs without damage/repair fields leave those statuses unknown.",
                        "Uncertainty = RTT/2 + 1 us quantization + logged clock-pair uncertainty + assumed drift * sample age.",
                        "The drift allowance is an assumption, not a measured bound. Clock asymmetry is unresolved.",
                        "Negative intervals are retained, never clamped; compare them with offset uncertainty.",
                        "Repeats are gaps between observed flip sequences; no repeats can be inferred before/after them."]}
    if args.joined:
        with args.joined.open("w", newline="") as file:
            fields = list(joined[0]) if joined else ["session", "frame", "fate"]
            writer = csv.DictWriter(file, fieldnames=fields)
            writer.writeheader()
            writer.writerows(joined)
    return result


def main(argv=None):
    args = arguments(__doc__, argv)
    result = analyze(args)
    emit_json(args.json, result)
    print(f"Joined {result['matched_frames']}/{result['sender_frames']} sender frames by (session, frame).")
    print("Fates:", ", ".join(f"{key}={value}" for key, value in result["fates"].items()))
    print(f"Damaged={result['damaged_frames']} (unknown={result['damage_status_unknown_frames']}); "
          f"FEC repaired frames={result['frames_repaired']} fragments={result['fragments_repaired']} "
          f"(unknown frames={result['repair_status_unknown_frames']}).")
    print("Repeated vblanks between logged flips:", result["repeated_vblanks_between_logged_flips"])
    for name, value in result["intervals"].items():
        print_stats(name, value)
    print_stats("Offset uncertainty (+/-)", result["clock"]["uncertainty_us"])
    print(f"Clock sample window: +/-{args.clock_window:g} s; assumed drift: {args.drift_ppm:g} ppm; "
          f"unmapped frames: {result['clock']['unmapped_frames']}.")
    for line in (result["definitions"] if args.verbose else []) + result["warnings"]:
        print(line)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        sys.exit(1)
