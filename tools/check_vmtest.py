#!/usr/bin/env python3
"""Check vmtest sender logs against a VM receiver's CSVs and HEVC dump."""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from analysis_common import (ClockSamples, FrameIndex, arguments, csv_rows, emit_json,
                             integer, print_stats, session_of, clock_path)
import analyze_probe
import analyze_stream

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 1920, 1080
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2


def check_stage_times(row, stage, received):
    """Independent stage ordering and a wire timestamp echoed by the receiver.

    Joined rows intentionally repeat present/lock values; comparing just those
    aliases cannot detect a corrupt/mismatched transport half.
    """
    present, locked = integer(row, "capture_us"), integer(row, "encoded_us")
    acquire, convert, start = (integer(stage, key, 0) for key in
                               ("acquire_return_us", "conversion_submit_us", "encode_start_us"))
    first, last = integer(row, "first_send_us"), integer(row, "last_send_us")
    returned = integer(stage, "send_return_us", 0)
    if (locked < present or (start and not present <= start <= locked) or
            (convert and not present <= convert <= (start or locked)) or
            (acquire and convert and acquire > convert) or
            (first and not locked <= first <= last) or (returned and last > returned)):
        raise ValueError("stage/transport timestamps are out of order")
    if received and integer(received, "first_rx_ns", 0) and "encoded_us" in received:
        if integer(received, "capture_us") != present or integer(received, "encoded_us") != min(locked - present, 0xfffffffe):
            raise ValueError("stage/transport encoded time disagrees with receiver's wire timestamp")


def load_sessions(rx_dir, pc_dir):
    receiver = FrameIndex(list(csv_rows(rx_dir / "rx.csv", (
        "session", "frame", "fate", "first_rx_ns", "complete_ns", "damaged",
        "frames_repaired", "fragments_repaired"))))
    sessions = {}
    for name in ("clean", "loss", "fec"):
        log_path = pc_dir / f"run-{name}.log"
        if not log_path.exists():
            continue
        log = log_path.read_text(errors="replace")
        candidates = (pc_dir / name / "sender.csv", pc_dir / f"sender-{name}.csv",
                      pc_dir / "sender.csv", rx_dir / "sender.csv")
        path = next((p for p in candidates if p.exists()), None)
        if path is None:
            continue
        rows = [row for row in csv_rows(path, ("session", "frame", "bytes")) if integer(row, "session") != 0]
        logged_ids = re.findall(r"Sender session=([0-9a-fA-F]+)\b", log)
        if logged_ids:
            if len(logged_ids) != 1:
                raise ValueError(f"{log_path}: expected one sender session")
            session = int(logged_ids[0], 16)
            rows = [row for row in rows if session_of(row) == session]
        ids = {session_of(row) for row in rows}
        if len(ids) != 1:
            raise ValueError(f"{path}: expected one nonempty session for {name}")
        session = ids.pop()
        if session in sessions:
            raise ValueError("PC files reuse a session ID")
        stages_path = Path(str(path) + ".stages.csv")
        stages = rows if "present_us" in rows[0] else list(csv_rows(stages_path,
            ("frame", "bytes", "key", "sent", "present_us", "bitstream_locked_us")))
        if len(stages) != len(rows):
            raise ValueError(f"{name}: sender/stage row counts differ (incomplete run)")
        # stages.count() supplies pattern_offset before append(); sender.count++
        # supplies the wire frame. Both start at zero, including recovery IDRs.
        for ordinal, (row, stage) in enumerate(zip(rows, stages)):
            if (integer(row, "frame") != ordinal or integer(stage, "frame") != ordinal or
                    integer(stage, "sent") != 1 or integer(stage, "key") not in (0, 1) or
                    integer(row, "bytes") != integer(stage, "bytes") or integer(row, "bytes") <= 0 or
                    integer(row, "capture_us") != integer(stage, "present_us") or
                    integer(row, "encoded_us") != integer(stage, "bitstream_locked_us")):
                raise ValueError(f"{name}: stage/transport mismatch at encode ordinal {ordinal}")
            check_stage_times(row, stage, receiver.get(session, ordinal))
        patterns = re.findall(r"Pattern source=(\d+)x(\d+) motion=(\d+) px/frame right", log)
        if len(patterns) != 1:
            raise ValueError(f"run-{name}.log: expected one moving-pattern banner")
        pattern = tuple(map(int, patterns[0]))
        if pattern != (WIDTH, HEIGHT, 8):
            raise ValueError(f"{name}: expected vmtest's 1920x1080 pattern with motion 8, got {pattern}")
        argv = ["--sender", str(path), "--receiver", str(rx_dir / "rx.csv"),
                "--packets", str(rx_dir / "packets.csv"), "--session", str(session)]
        if (rx_dir / "run.log").exists():
            argv += ["--receiver-log", str(rx_dir / "run.log")]
        args = arguments(__doc__, argv, probe=True)
        args.joined = None
        stream = analyze_stream.analyze(args)
        probe = analyze_probe.analyze(args)
        clocks_path = clock_path(path)
        clocks = ClockSamples(csv_rows(clocks_path, ("session", "sender_tx_us", "receiver_rx_us",
                                                    "receiver_tx_us", "sender_rx_us"))) if clocks_path.exists() else ClockSamples([])
        sessions[session] = dict(name=name, id=session, rows=rows, stages=stages, pattern=pattern,
                                 stream=stream, probe=probe, clocks=clocks, args=args)
    if not sessions:
        raise ValueError("No sender profiles matched the run-{clean,loss,fec}.log sessions")
    return sessions, receiver


def expected_pictures(sessions, receiver, dump_size):
    """In --no-decode, dump writes and delivered CSV rows have the same order."""
    pictures, offset = [], 0
    last = {}
    for row in receiver.rows:
        session, frame = session_of(row), integer(row, "frame")
        if session not in sessions:
            raise ValueError(f"Receiver session {session:08x} has no PC sender file")
        if row["fate"] != "delivered":
            if row["fate"] not in ("lost", "skipped", "unfinished"):
                raise ValueError("Expected a --no-decode receiver run")
            continue
        if integer(row, "span", 1) != 1 or integer(row, "decoded_ns", 0) or integer(row, "flip_ns", 0):
            raise ValueError("Pixel mapping requires --no-decode delivered rows")
        selected = sessions[session]
        if not 0 <= frame < len(selected["rows"]) or frame <= last.get(session, -1):
            raise ValueError("Delivered frames are outside the sender run or out of order")
        last[session] = frame
        size = integer(selected["rows"][frame], "bytes")
        pictures.append(dict(session=session, frame=frame, offset=offset, bytes=size,
                             key=integer(selected["stages"][frame], "key"),
                             damaged=integer(row, "damaged"), decoded=False))
        offset += size
    if offset != dump_size:
        raise ValueError(f"Dump size mismatch: delivered AUs total {offset} bytes, stream.hevc has {dump_size}")
    return pictures


def start_code(data, position):
    """An AU boundary as the position of its start code's 01 byte. Parsers differ
    on which AU owns the zero_byte of a 4-byte start code (FFmpeg 6.1 gives it
    to the previous one, 7.1 to the next); the boundary is the same."""
    i = position
    while i < len(data) and data[i] == 0:
        i += 1
    return i if i == len(data) or (i - position >= 2 and data[i] == 1) else -1 - position


def map_decoded(probe, pictures, data):
    """Match parser packets AND decoded pictures by byte position/size, not ordinal."""
    records = probe.get("packets_and_frames", [])
    packets = [row for row in records if row["type"] == "packet"]
    if len(packets) != len(pictures):
        raise ValueError("HEVC parser AU count differs from delivered CSV rows")

    def bounds(position, size):
        return start_code(data, position), start_code(data, position + size)

    by_start = {}
    for packet, picture in zip(packets, pictures):
        if (bounds(integer(packet, "pos"), integer(packet, "size")) != bounds(picture["offset"], picture["bytes"]) or
                ("K" in packet["flags"]) != bool(picture["key"])):
            raise ValueError(f"HEVC AU offset/size/key mismatch at session {picture['session']:08x} frame {picture['frame']}")
        by_start[start_code(data, picture["offset"])] = picture
    decoded, previous = [], -1
    for frame in (row for row in records if row["type"] == "frame"):
        offset, size = integer(frame, "pkt_pos"), integer(frame, "pkt_size")
        picture = by_start.get(start_code(data, offset))
        if (picture is None or offset <= previous or
                bounds(offset, size) != bounds(picture["offset"], picture["bytes"]) or
                integer(frame, "width") != WIDTH or integer(frame, "height") != HEIGHT or
                frame["pix_fmt"] != "yuv420p" or frame["pict_type"] not in ("I", "P") or
                integer(frame, "key_frame") != picture["key"]):
            raise ValueError("Decoded picture does not match a unique, ordered 1080p HEVC Main AU")
        previous = offset
        picture["decoded"] = True
        decoded.append(picture)
    if any(not p["decoded"] and not p["damaged"] for p in pictures):
        raise ValueError("FFmpeg omitted a clean delivered AU; cannot complete the pixel check")
    return decoded


def run_checked(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed ({result.returncode}): {result.stderr[-2000:]}")
    return result


def pixel_check(dump, pictures, sessions, reference, ffmpeg, ffprobe):
    if not pictures:
        return []
    # Identical software decoder/thread settings in the mapping and PSNR passes.
    probe = run_checked([ffprobe, "-v", "warning", "-threads", "1", "-f", "hevc", "-i", str(dump),
                         "-select_streams", "v:0", "-show_packets", "-show_frames", "-show_entries",
                         "packet=pos,size,flags:frame=pkt_pos,pkt_size,width,height,pix_fmt,key_frame,pict_type", "-of", "json"])
    decoded = map_decoded(json.loads(probe.stdout), pictures, Path(dump).read_bytes())
    if not decoded:
        return probe.stderr.splitlines()
    with tempfile.TemporaryDirectory(prefix="llstream-vmtest-") as directory:
        scratch = Path(directory)
        cache = {}
        command = [ffmpeg, "-hide_banner", "-v", "warning", "-nostats", "-nostdin", "-threads", "1",
                   "-f", "hevc", "-i", str(dump), "-f", "rawvideo", "-pixel_format", "nv12",
                   "-video_size", f"{WIDTH}x{HEIGHT}", "-framerate", "60", "-i", "pipe:0",
                   "-filter_complex_threads", "1", "-filter_complex",
                   "[0:v]settb=1/60,setpts=N,format=yuv420p[actual];"
                   "[1:v]settb=1/60,setpts=N,format=yuv420p[ref];"
                   "[actual][ref]psnr=stats_file=psnr.log:eof_action=pass:repeatlast=0",
                   "-fps_mode", "passthrough", "-progress", "pipe:1", "-f", "null", "-"]
        # File-backed logs cannot block the reference pipe when damaged frames
        # produce decoder warnings. Only one reference frame is held in memory.
        with (scratch / "decode.log").open("w+") as errors, (scratch / "progress").open("w+") as progress:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=progress, stderr=errors, cwd=scratch)
            try:
                for picture in decoded:
                    sw, sh, motion = sessions[picture["session"]]["pattern"]
                    offset = picture["frame"] * motion % sw
                    key = (sw, sh, offset)
                    if key not in cache:
                        path = scratch / f"reference-{sw}-{sh}-{offset}.nv12"
                        run_checked([str(reference), "--reference", str(path), str(sw), str(sh), "--offset", str(offset)])
                        if path.stat().st_size != FRAME_BYTES:
                            raise ValueError("Reference generator did not produce one 1920x1080 NV12 frame")
                        cache[key] = path
                    process.stdin.write(cache[key].read_bytes())
                process.stdin.close()
                code = process.wait()
            except BaseException:
                process.kill()
                process.wait()
                try:
                    process.stdin.close()
                except BrokenPipeError:
                    pass
                raise
            errors.seek(0)
            diagnostics = errors.read()
            if code:
                raise RuntimeError(f"FFmpeg decode/PSNR failed ({code}): {diagnostics[-2000:]}")
            progress.seek(0)
            counts = re.findall(r"^frame=(\d+)$", progress.read(), re.MULTILINE)
            if not counts or int(counts[-1]) != len(decoded):
                raise ValueError("FFmpeg decoded-picture count changed between mapping and PSNR passes")
        lines = (scratch / "psnr.log").read_text().splitlines()
        if len(lines) != len(decoded):
            raise ValueError("FFmpeg did not compare exactly one reference per decoded picture")
        for number, (line, picture) in enumerate(zip(lines, decoded), 1):
            values = dict(field.split(":", 1) for field in line.split())
            if integer(values, "n") != number:
                raise ValueError("Nonconsecutive PSNR results")
            picture["psnr_db"] = {}
            for plane in ("y", "u", "v"):
                value = float(values["psnr_" + plane])
                if math.isnan(value) or value < 0:
                    raise ValueError("Invalid PSNR value")
                picture["psnr_db"][plane] = "inf" if math.isinf(value) else value
    return list(dict.fromkeys((probe.stderr + diagnostics).splitlines()))


def psnr_stats(values):
    """Linear quantiles including perfect (infinite PSNR) pictures, JSON-safe."""
    values = sorted(map(float, values))
    def quantile(fraction):
        if not values:
            return None
        at = (len(values) - 1) * fraction
        low, high = values[math.floor(at)], values[math.ceil(at)]
        value = low if low == high else low + (high - low) * (at - math.floor(at))
        return "inf" if math.isinf(value) else value
    return dict(n=len(values), min=quantile(0), median=quantile(.5), p95=quantile(.95),
                p99=quantile(.99), max=quantile(1), perfect=sum(math.isinf(v) for v in values))


def recovery_events(session, receiver):
    rows, stages = session["rows"], session["stages"]
    clean_keys = [i for i, stage in enumerate(stages) if integer(stage, "key") and
                  (out := receiver.get(session["id"], i)) is not None and
                  out["fate"] == "delivered" and not integer(out, "damaged")]
    events = []
    for i, row in enumerate(rows):
        out = receiver.get(session["id"], i)
        if out is None or out["fate"] != "lost":
            continue
        key = next((k for k in clean_keys if k > i), None)
        event = dict(lost_frame=i, key_frame=key, frames=key - i if key is not None else None,
                     ms=None, uncertainty_ms=None, start="first send" if integer(row, "first_send_us") else "encoded",
                     end="key AU complete", clock_domain=analyze_stream.MAPPED)
        if key is not None:
            key_row = rows[key]
            anchor = integer(key_row, "first_send_us") or integer(key_row, "encoded_us")
            pair_us = session["stream"]["clock"]["receiver_clock_pair_uncertainty_us"] or 0
            clock = session["clocks"].choose(session["id"], anchor, session["args"].clock_window * 1e6,
                                                session["args"].drift_ppm, pair_us)
            complete = integer(receiver.get(session["id"], key), "complete_ns")
            if clock and complete:
                start = integer(row, "first_send_us") or integer(row, "encoded_us")
                event["ms"] = (complete / 1000 - clock["offset_us"] - start) / 1000
                event["uncertainty_ms"] = clock["uncertainty_us"] / 1000
        events.append(event)
    return events


def analyze(rx_dir, pc_dir, *, reference, ffmpeg="ffmpeg", ffprobe="ffprobe"):
    sessions, receiver = load_sessions(rx_dir, pc_dir)
    dump = (rx_dir / "stream.hevc").resolve()
    before = dump.stat()
    pictures = expected_pictures(sessions, receiver, before.st_size)
    warnings = pixel_check(dump, pictures, sessions, reference.resolve(), ffmpeg, ffprobe)
    after = dump.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("stream.hevc changed during analysis; wait for the receiver to finish")
    reports = []
    for session, data in sessions.items():
        selected = [p for p in pictures if p["session"] == session]
        decoded = [p for p in selected if p["decoded"]]
        groups = {name: {plane: psnr_stats([p["psnr_db"][plane] for p in decoded if p["damaged"] == damaged])
                        for plane in ("y", "u", "v")} for name, damaged in (("clean", 0), ("damaged", 1))}
        fates = data["stream"]["fates"]
        counts = dict(sent=len(data["rows"]), delivered=fates["delivered"], lost=fates["lost"],
                      damaged=sum(p["damaged"] for p in selected), repaired=data["stream"]["frames_repaired"],
                      injected_drops=data["probe"]["injected_drops"], other_missing=data["probe"]["unexplained_missing_packets"],
                      recovery_idrs_sent=sum(integer(stage, "key") for stage in data["stages"][1:]),
                      skipped=fates["skipped"], unreported=fates["unreported"], decoded=len(decoded))
        reports.append(dict(name=data["name"], session=session, counts=counts, recovery=recovery_events(data, receiver),
                            pixels=groups, decoder_omitted_damaged=len(selected) - len(decoded),
                            stages=data["stream"]["intervals"], clock=data["stream"]["clock"],
                            warnings=data["stream"]["warnings"]))
    return dict(sessions=reports, pictures=pictures, decode_warnings=warnings,
                mapping="Delivered CSV order checked against HEVC parser AU offsets/sizes/keys; decoded pkt_pos/pkt_size select each reference.",
                pattern_index="Offset=(wire frame*8)%1920: llstream_win uses stages.count() before append; lls_sender uses count++ from zero; stage/transport rows, sizes and timestamps checked.",
                path="Windows -> VMware NAT -> Linux VM; this is not the box's Wi-Fi, MPP, or display latency.")


def number(value):
    return "unavailable" if value is None else value if isinstance(value, str) else f"{value:.3f}"


def print_report(report):
    print(report["path"])
    print(report["mapping"])
    print(report["pattern_index"])
    if report["decode_warnings"]:
        print(f"FFmpeg: {len(report['decode_warnings'])} distinct diagnostic lines (included in --json).")
    for session in report["sessions"]:
        print(f"\n{session['name']} (session {session['session']:08x})")
        print("  Counts: " + " ".join(f"{k}={v}" for k, v in session["counts"].items()))
        for event in session["recovery"]:
            if event["key_frame"] is None:
                print(f"  Loss {event['lost_frame']}: no clean key delivered before this run ended.")
            else:
                print(f"  Loss {event['lost_frame']} -> key {event['key_frame']}: {event['frames']} frames, "
                      f"{number(event['ms'])} ms +/- {number(event['uncertainty_ms'])} ms "
                      f"[{event['start']} -> {event['end']}; {event['clock_domain']}].")
        if not session["recovery"]:
            print("  Recovery: no unrepaired losses recorded.")
        clean = session["pixels"]["clean"]["y"]
        print(f"  Pixels: {clean['n']} clean pictures, minimum Y PSNR {number(clean['min'])} dB; "
              f"decoder omitted {session['decoder_omitted_damaged']} damaged AUs.")
        if session["pixels"]["damaged"]["y"]["n"]:
            for plane, values in session["pixels"]["damaged"].items():
                print(f"  Damaged {plane.upper()} PSNR: n={values['n']} " +
                      " ".join(f"{key}={number(values[key])}" for key in ("min", "median", "p95", "p99", "max")) + " dB")
        else:
            print("  Damaged PSNR: no decoded damaged pictures.")
        for name, values in session["stages"].items():
            if values["n"]:
                print_stats("  " + name, values)
        print("  Decode/flip stages unavailable: the VM receiver uses --no-decode.")
        print_stats("  Offset uncertainty (+/-)", session["clock"]["uncertainty_us"])
        for warning in session["warnings"]:
            print("  " + warning)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rx_dir", type=Path)
    parser.add_argument("pc_dir", type=Path)
    parser.add_argument("--reference", type=Path, default=ROOT / "build/host/test_color_math")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--json", type=Path, help="Save counts, recovery events, per-picture mapping/PSNR and timing summaries")
    args = parser.parse_args(argv)
    inputs = {args.rx_dir / name for name in ("rx.csv", "packets.csv", "stream.hevc", "run.log", "run.json", "status")}
    inputs.update(args.pc_dir / name for name in ("sender.csv", "clock.csv"))
    inputs.add(args.rx_dir / "sender.csv")
    inputs.update(args.pc_dir / name / table for name in ("clean", "loss", "fec") for table in ("sender.csv", "clock.csv"))
    inputs.update(args.pc_dir / (f"sender-{name}.csv" + suffix)
                  for name in ("clean", "loss", "fec") for suffix in ("", ".stages.csv", ".clock.csv"))
    inputs.update(args.pc_dir / f"run-{name}.log" for name in ("clean", "loss", "fec"))
    inputs.add(args.reference)
    if args.json and args.json.resolve() in {path.resolve() for path in inputs}:
        parser.error("--json must not overwrite an input file")
    report = analyze(args.rx_dir, args.pc_dir, reference=args.reference, ffmpeg=args.ffmpeg, ffprobe=args.ffprobe)
    emit_json(args.json, report)
    print_report(report)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        sys.exit(1)
