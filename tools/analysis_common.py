"""CSV/clock handling shared by the offline analyzers (standard library only)."""
import argparse
import bisect
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import re

MOD = 1 << 32
FATES = ("shown", "replaced", "lost", "skipped", "decode_error", "delivered", "discarded", "unfinished")


def csv_rows(path, required):
    with Path(path).open(newline="") as file:
        reader = csv.DictReader(file)
        missing = set(required) - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")
        for line, row in enumerate(reader, 2):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"{path}:{line}: truncated or malformed CSV row")
            yield row


def integer(row, name, default=None):
    if name not in row and default is not None:
        return default
    try:
        return int(row[name])
    except (KeyError, ValueError) as exc:
        raise ValueError(f"Invalid integer {name}={row.get(name)!r}") from exc


def session_of(row):
    value = integer(row, "session")
    if not 0 < value < MOD:
        raise ValueError("Invalid session: expected 1..4294967295")
    return value


def percent(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    at = (len(ordered) - 1) * fraction
    low = math.floor(at)
    return ordered[low] + (ordered[math.ceil(at)] - ordered[low]) * (at - low)


def stats(values, unit="ms", domain=None):
    result = {"n": len(values), "unit": unit, "median": percent(values, .5),
              "p95": percent(values, .95), "p99": percent(values, .99),
              "max": max(values) if values else None, "min": min(values) if values else None,
              "negative": sum(value < 0 for value in values)}
    if domain:
        result["clock_domain"] = domain
    return result


def log_counters(path):
    if path is None:
        return {}
    text = Path(path).read_text()
    result = {}
    for line in text.splitlines():
        if line.startswith("Receiver "):
            result.update({key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)})
    return result


class FrameIndex:
    """Look up coalesced loss spans, including wrap, without expanding them."""
    def __init__(self, rows):
        intervals = defaultdict(list)
        self.rows = rows
        for row in rows:
            session = session_of(row)
            frame, span = integer(row, "frame"), integer(row, "span", 1)
            if not 0 <= frame < MOD or not 1 <= span <= MOD // 2:
                raise ValueError("Invalid receiver frame/span")
            if row["fate"] not in FATES or (span > 1 and row["fate"] != "lost"):
                raise ValueError("Unknown receiver fate or non-loss span")
            end = frame + span
            intervals[session].append((frame, min(end, MOD), row))
            if end > MOD:
                intervals[session].append((0, end - MOD, row))
        self.intervals, self.starts = {}, {}
        for session, values in intervals.items():
            values.sort(key=lambda item: item[0])
            if any(a[1] > b[0] for a, b in zip(values, values[1:])):
                raise ValueError("Overlapping receiver frames/spans in one session")
            self.intervals[session] = values
            self.starts[session] = [item[0] for item in values]

    def get(self, session, frame):
        position = bisect.bisect_right(self.starts.get(session, []), frame) - 1
        if position >= 0:
            begin, end, row = self.intervals[session][position]
            if begin <= frame < end:
                return row
        return None


class ClockSamples:
    def __init__(self, rows):
        self.samples = defaultdict(list)
        self.invalid = 0
        for row in rows:
            session = session_of(row)
            tx, rr, rt, rx = (integer(row, name) for name in
                              ("sender_tx_us", "receiver_rx_us", "receiver_tx_us", "sender_rx_us"))
            rtt = (rx - tx) - (rt - rr)
            if min(tx, rr, rt, rx) <= 0 or rx < tx or rt < rr or rtt < 0:
                self.invalid += 1
                continue
            self.samples[session].append({"at_us": (tx + rx) / 2, "rtt_us": rtt,
                                          "offset_us": ((rr - tx) + (rt - rx)) / 2})
        self.times = {}
        for session, values in self.samples.items():
            values.sort(key=lambda item: item["at_us"])
            self.times[session] = [item["at_us"] for item in values]

    def window(self, session, at_us, radius_us):
        times = self.times.get(session, [])
        return self.samples.get(session, [])[bisect.bisect_left(times, at_us - radius_us):
                                             bisect.bisect_right(times, at_us + radius_us)]

    def choose(self, session, at_us, radius_us, drift_ppm=50, pair_us=0):
        nearby = self.window(session, at_us, radius_us)
        if not nearby:
            return None
        best = min(nearby, key=lambda sample: (sample["rtt_us"], abs(sample["at_us"] - at_us)))
        age = abs(best["at_us"] - at_us)
        return dict(best, age_us=age, uncertainty_us=best["rtt_us"] / 2 + 1 + pair_us + age * drift_ppm / 1e6,
                    window_samples=len(nearby), window_rtt_p95_us=percent([p["rtt_us"] for p in nearby], .95))


def clock_path(sender):
    path = sender.parent / "clock.csv"
    return path if path.exists() else Path(str(sender) + ".clock.csv")


def arguments(description, argv=None, *, probe=False):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--sender", type=Path, required=True, help="Transport CSV with a session column")
    parser.add_argument("--receiver", type=Path, required=True, help="Receiver frame CSV")
    parser.add_argument("--session", type=lambda s: int(s, 0), help="Analyze one session from a combined profile")
    parser.add_argument("--clock", type=Path, help="Default: sibling clock.csv (legacy SENDER.clock.csv also accepted)")
    parser.add_argument("--receiver-log", type=Path, help="run.log/receiver.log for socket drops and clock conversion uncertainty")
    parser.add_argument("--clock-window", type=float, default=5, help="Clock sample search radius in seconds (default +/-5)")
    parser.add_argument("--drift-ppm", type=float, default=50, help="Assumed clock drift budget, not a measurement (default 50 ppm)")
    parser.add_argument("--json", type=Path, help="Also write machine-readable summary")
    parser.add_argument("--verbose", action="store_true", help="Print measurement definitions (always included in JSON)")
    if probe:
        parser.add_argument("--packets", type=Path, help="Collected packets.csv table; needed for exact loss/bursts")
        parser.add_argument("--deadline-ms", type=float, default=20)
    else:
        parser.add_argument("--joined", type=Path, help="Write per-frame joined/mapped timestamps and uncertainty")
    args = parser.parse_args(argv)
    for name in ("clock_window", "drift_ppm", "deadline_ms"):
        if hasattr(args, name) and (not math.isfinite(getattr(args, name)) or getattr(args, name) < 0):
            parser.error(name.replace("_", "-") + " must be finite and nonnegative")
    if args.clock_window == 0 or (probe and args.deadline_ms == 0):
        parser.error("Clock window and deadline must be positive")
    inputs = {path.resolve() for path in (args.sender, args.receiver, args.clock or clock_path(args.sender),
              args.receiver_log, getattr(args, "packets", None)) if path}
    outputs = [path.resolve() for path in (args.json, getattr(args, "joined", None)) if path]
    if any(path in inputs for path in outputs) or len(set(outputs)) != len(outputs):
        parser.error("Output paths must differ from all inputs and each other")
    return args


def load(args):
    senders = list(csv_rows(args.sender, ("session", "frame", "packets", "capture_us", "encoded_us", "first_send_us", "last_send_us")))
    file_only = sum(integer(row, "session") == 0 for row in senders)
    senders = [row for row in senders if integer(row, "session") != 0]
    keys = set()
    if getattr(args, "session", None) is not None:
        senders = [row for row in senders if session_of(row) == args.session]
    for row in senders:
        row["session"] = session_of(row)
        frame = integer(row, "frame")
        key = (row["session"], frame)
        if not 0 <= frame < MOD or key in keys:
            raise ValueError("Duplicate or invalid sender (session, frame)")
        keys.add(key)
    receiver = FrameIndex(list(csv_rows(args.receiver, ("session", "frame", "fate", "first_rx_ns", "complete_ns"))))
    for row in senders:
        received = receiver.get(row["session"], integer(row, "frame"))
        if received and integer(received, "capture_us", 0) not in (0, integer(row, "capture_us")):
            raise ValueError("Sender/receiver capture timestamps disagree: mismatched runs")
    path = args.clock or clock_path(args.sender)
    warnings = [f"Ignored {file_only} file-only encode rows (session 0)."] if file_only else []
    if path.exists():
        clocks = ClockSamples(csv_rows(path, ("session", "sender_tx_us", "receiver_rx_us", "receiver_tx_us", "sender_rx_us")))
    elif args.clock:
        raise ValueError("Clock file does not exist: " + str(path))
    else:
        clocks = ClockSamples([])
        warnings.append("No clock companion: cross-clock intervals are unavailable.")
    counters = log_counters(args.receiver_log)
    # A collector dropping records is distinct from the video path losing packets.
    profile_missing = 0
    directories = {item.parent for item in (args.sender, args.receiver, path, getattr(args, "packets", None)) if item}
    for directory in directories:
        metadata = directory / "profile.jsonl"
        if not metadata.exists():
            continue
        with metadata.open() as stream:
            for line in stream:
                message = json.loads(line).get("message", "")
                if message.startswith("#skipped "):
                    profile_missing += int(message.split()[-1])
                elif message.startswith("#truncated "):
                    profile_missing += 1
    counters["profile_records_missing"] = profile_missing
    if profile_missing:
        warnings.append(f"Profile collection skipped/truncated {profile_missing} records; absent rows are not evidence of network loss.")
    if "offset_pair_uncertainty_ns" not in counters:
        warnings.append("No receiver clock-pair uncertainty logged; reported offset uncertainty omits that term.")
    if counters.get("clock_steps") or counters.get("timestamp_fallbacks"):
        warnings.append("Receiver clock steps/timestamp fallbacks occurred; inspect run.log before trusting timing tails.")
    return senders, receiver, clocks, counters, warnings


def fate_counts(senders, receiver):
    counts = Counter({fate: 0 for fate in (*FATES, "unreported")})
    for row in senders:
        found = receiver.get(row["session"], integer(row, "frame"))
        counts[found["fate"] if found else "unreported"] += 1
    sessions = {row["session"] for row in senders}
    recorded = Counter({fate: 0 for fate in FATES})
    for row in receiver.rows:
        if session_of(row) in sessions:
            recorded[row["fate"]] += integer(row, "span", 1)
    return dict(counts), dict(recorded)


def emit_json(path, result):
    if path:
        path.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")


def quality_row(row):
    values = {key: integer(row, key) if row and key in row else None
              for key in ("damaged", "frames_repaired", "fragments_repaired")}
    if values["damaged"] not in (None, 0, 1) or values["frames_repaired"] not in (None, 0, 1):
        raise ValueError("Invalid damage/repair flag")
    fragments = values["fragments_repaired"]
    if fragments is not None and (fragments < 0 or bool(fragments) != bool(values["frames_repaired"])):
        raise ValueError("Invalid repaired-fragment count")
    return values


def quality_counts(senders, receiver):
    rows = [quality_row(receiver.get(row["session"], integer(row, "frame"))) for row in senders]
    return {"damaged_frames": sum(row["damaged"] or 0 for row in rows),
            "damage_status_unknown_frames": sum(row["damaged"] is None for row in rows),
            "frames_repaired": sum(row["frames_repaired"] or 0 for row in rows),
            "fragments_repaired": sum(row["fragments_repaired"] or 0 for row in rows),
            "repair_status_unknown_frames": sum(row["frames_repaired"] is None for row in rows)}


def print_stats(name, result):
    numbers = " ".join(f"{key}={result[key]:.3f}" for key in ("median", "p95", "p99", "max")) if result["n"] else "unavailable"
    print(f"{name}: n={result['n']} {numbers} {result['unit']} [{result.get('clock_domain', 'count')}]")
