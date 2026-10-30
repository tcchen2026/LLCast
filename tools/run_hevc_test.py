#!/usr/bin/env python3
"""Build, run, and verify the native Rockchip HEVC probe over ADB."""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
# Bump when the x265 settings below change. "idr": closed GOP, so every
# keyframe is an IDR that a receiver can recover from.
CLIP_SETTINGS = "idr"


def run(args, **kwargs):
    print("+ " + shlex.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def read_exact(stream, size):
    parts = []
    remaining = size
    while remaining:
        data = stream.read(remaining)
        if not data:
            break
        parts.append(data)
        remaining -= len(data)
    return b"".join(parts)


def verify_pixels(clip, hashes, frames, width, height):
    with hashes.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != frames:
        raise RuntimeError(f"Expected {frames} hardware checksums, got {len(rows)}")
    command = ["ffmpeg", "-v", "error", "-i", str(clip), "-map", "0:v:0",
               "-pix_fmt", "nv12", "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1"]
    print("Comparing every decoded NV12 frame with FFmpeg software HEVC decoding...", flush=True)
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    try:
        for index, row in enumerate(rows):
            if (int(row["output_index"]), int(row["input_index"])) != (index, index):
                raise RuntimeError(f"Unexpected frame order at frame {index}: {row}")
            if (int(row["width"]), int(row["height"])) != (width, height):
                raise RuntimeError(f"Unexpected decoded dimensions: {row}")
            size = width * height * 3 // 2
            pixels = read_exact(process.stdout, size)
            if len(pixels) != size:
                raise RuntimeError(f"Software decoder ended early at frame {index}")
            actual = f"{zlib.crc32(pixels):08x}"
            if actual != row["crc32_nv12"]:
                raise RuntimeError(f"Frame {index}: software CRC {actual} != hardware CRC {row['crc32_nv12']}")
        if process.stdout.read(1):
            raise RuntimeError("Software decoder produced extra frames")
        if process.wait(timeout=30):
            raise RuntimeError("Software decoder failed")
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.kill()
            process.wait()
    print(f"PASS: all {frames} frames match the software decoder (CRC32 over all active NV12 pixels).", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="127.0.0.1:5555")
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME"))
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--frames", type=int, default=180)
    parser.add_argument("--fps", type=int, default=60, help="Source frame rate; 1..240")
    parser.add_argument("--unpaced", action="store_true", help="Measure serial decode throughput")
    parser.add_argument("--skip-verify", action="store_true")
    args = parser.parse_args()
    match = re.fullmatch(r"([0-9]+)x([0-9]+)", args.size)
    if not match:
        parser.error("--size must be WIDTHxHEIGHT")
    width, height = map(int, match.groups())
    if any(n < 64 or n > 4096 or n % 2 for n in (width, height)):
        parser.error("Dimensions must be even and between 64 and 4096")
    if not 1 <= args.frames <= 10000 or not 1 <= args.fps <= 240:
        parser.error("Use 1..10000 frames and 1..240 fps")
    if not args.ndk:
        parser.error("Provide --ndk PATH or set ANDROID_NDK_HOME")
    ndk = Path(args.ndk).expanduser().resolve()
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        parser.error(f"No Android NDK toolchain at {toolchain}")
    for tool in ("adb", "cmake", "ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            parser.error(f"Missing required program: {tool}")
    adb = ["adb", "-s", args.serial]

    def shell(command, **kwargs):
        return run(adb + ["shell", shlex.join(list(map(str, command)))], **kwargs)

    shell(["id"])
    build = ROOT / "build"
    vendor = build / "vendor"
    assets = build / "assets"
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    results = build / "results" / stamp
    for directory in (vendor, assets, results):
        directory.mkdir(parents=True, exist_ok=True)
    print(f"Results: {results}", flush=True)
    library = vendor / "libmpp.so"
    run(adb + ["pull", "/vendor/lib64/libmpp.so", library])
    run(adb + ["pull", "/vendor/lib64/libdrm.so", vendor / "libdrm.so"])
    run(["cmake", "-S", ROOT, "-B", build / "android",
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DANDROID_ABI=arm64-v8a",
         "-DANDROID_PLATFORM=android-26", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DMPP_LIBRARY={library}", f"-DDRM_LIBRARY={vendor / 'libdrm.so'}"])
    run(["cmake", "--build", build / "android", "-j", "2"])
    # The name records the encoder settings, so a settings change cannot reuse a stale clip.
    clip = assets / f"testsrc2-{args.size}-{args.fps}fps-{args.frames}frames-{CLIP_SETTINGS}.hevc"
    if not clip.exists():
        partial = clip.with_suffix(".partial")
        run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-y",
             "-f", "lavfi", "-i", f"testsrc2=size={args.size}:rate={args.fps}",
             "-frames:v", args.frames, "-an", "-pix_fmt", "yuv420p", "-c:v", "libx265",
             "-preset", "veryfast", "-tune", "zerolatency", "-crf", "23",
             "-x265-params", f"bframes=0:rc-lookahead=0:repeat-headers=1:aud=1:open-gop=0:keyint={args.fps}:"
             f"min-keyint={args.fps}:scenecut=0:pools=4:frame-threads=1:log-level=error",
             "-f", "hevc", partial])
        partial.replace(clip)
    probe = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
                 "-show_entries", "frame=pict_type,width,height", "-of", "json", clip],
                capture_output=True, text=True)
    frames = json.loads(probe.stdout)["frames"]
    if len(frames) != args.frames or any(
        f["pict_type"] not in ("I", "P") or f["width"] != width or f["height"] != height
        for f in frames
    ):
        raise RuntimeError("Generated clip has unexpected frame count, dimensions, or picture types")

    remote = "/data/local/tmp/llstream-hevc/" + stamp
    binary = remote + "/hevc_decode_test"
    remote_clip = remote + "/test.hevc"
    shell(["mkdir", "-p", remote])
    run(adb + ["push", build / "android/hevc_decode_test", binary])
    run(adb + ["push", clip, remote_clip])
    shell(["chmod", "755", binary])
    (results / "run.json").write_text(json.dumps({
        "serial": args.serial, "remote_directory": remote, "size": args.size,
        "frames": args.frames, "source_fps": args.fps,
        "submission_fps": 0 if args.unpaced else args.fps, "window": 1,
        "clip_sha256": hashlib.sha256(clip.read_bytes()).hexdigest(),
        "libmpp_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "ndk": str(ndk),
    }, indent=2) + "\n")

    def device_run(name, extra):
        # Save failures as well as successes. The watchdog bounds a wedged process.
        seconds = max(30, int(args.frames / args.fps) + 30) if name == "latency" else max(30, args.frames)
        command = ["timeout", str(seconds), binary, remote_clip] + extra
        try:
            result = shell(command, capture_output=True, text=True, timeout=seconds + 15)
        except subprocess.CalledProcessError as exc:
            (results / f"{name}.log").write_text((exc.stdout or "") + (exc.stderr or ""))
            raise
        (results / f"{name}.log").write_text(result.stdout + result.stderr)
        print(result.stdout, end="", flush=True)
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="", flush=True)

    def driver_count():
        result = subprocess.run(adb + ["shell", "cat /proc/mpp_service/rkvdec0/task_count"],
                                capture_output=True, text=True, timeout=10)
        try:
            return int(result.stdout.strip()) if result.returncode == 0 else None
        except ValueError:
            return None

    before = driver_count()
    device_run("latency", ["--fps", "0" if args.unpaced else str(args.fps),
                           "--csv", remote + "/timings.csv"])
    after = driver_count()
    (results / "driver-counters.json").write_text(json.dumps({
        "rkvdec0_tasks_before": before, "rkvdec0_tasks_after": after,
        "delta": after - before if before is not None and after is not None else None,
        "note": "Device-wide counter; concurrent decoder users can also increment it.",
    }, indent=2) + "\n")
    run(adb + ["pull", remote + "/timings.csv", results / "timings.csv"])
    with (results / "timings.csv").open(newline="") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != args.frames:
        raise RuntimeError("Latency run did not return every picture")
    if not args.skip_verify:
        device_run("verification", ["--fps", "0", "--hashes", remote + "/hashes.csv"])
        run(adb + ["pull", remote + "/hashes.csv", results / "hashes.csv"])
        verify_pixels(clip, results / "hashes.csv", args.frames, width, height)
        (results / "verification.txt").write_text(f"PASS: {args.frames} NV12 frame CRC32 values match FFmpeg software decoding.\n")
    print(f"Saved logs and per-frame measurements to {results}", flush=True)
    print(f"Device test files: {remote}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        for name in ("stdout", "stderr"):
            output = getattr(exc, name, None)
            if output:
                print(output.decode(errors="replace") if isinstance(output, bytes) else output,
                      file=sys.stderr, end="")
        sys.exit(1)
