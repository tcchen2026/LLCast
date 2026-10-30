#!/usr/bin/env python3
"""Build the Windows programs and publish the test kit to the shared folder.

The kit contains LLCast.exe (the window for users), bin/*.exe, the
launchers and README from tools/windows,
the test clip, and VERSION.txt naming the exact build. Logs written by the
launchers under logs/ are never touched.
"""
import argparse
import datetime
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# Only reviewed programs are published.
PROGRAMS = ["LLCast", "llstream_clock", "llstream_send", "llstream_win"]
CLIP = ROOT / "build/assets/testsrc2-1920x1080-60fps-180frames-idr.hevc"


def run(args, **kwargs):
    print("+ " + " ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--share", type=Path, default=Path("/mnt/hgfs/deb13/LLCast"))
    parser.add_argument("--build", type=Path, default=ROOT / "build/mingw")
    args = parser.parse_args()
    if not args.share.is_dir():
        parser.error(f"Shared folder {args.share} is not mounted")
    # Re-detect both compiler runtimes. A stale C compiler detection can inject
    # 14-win32 into a POSIX C++ link, whose fstream layout is incompatible.
    run(["cmake", "--fresh", "-S", ROOT, "-B", args.build,
         f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/mingw-w64.cmake'}", "-DCMAKE_BUILD_TYPE=Release"],
        stdout=subprocess.DEVNULL)
    targets = subprocess.run(["cmake", "--build", args.build, "--target", "help"],
                             check=True, capture_output=True, text=True).stdout
    built = [name for name in PROGRAMS if f"... {name}" in targets]
    run(["cmake", "--build", args.build, "-j", "8", "--target", *built])

    version = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"],
                             capture_output=True, text=True).stdout.strip() or "unknown"
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    (args.share / "bin").mkdir(exist_ok=True)
    (args.share / "clips").mkdir(exist_ok=True)
    (args.share / "logs").mkdir(exist_ok=True)
    for name in built:
        folder = args.share if name == "LLCast" else args.share / "bin"
        shutil.copy2(args.build / f"{name}.exe", folder / f"{name}.exe")
    for path in sorted((ROOT / "tools/windows").iterdir()):
        if path.suffix.lower() == ".bat":
            (args.share / path.name).write_bytes(path.read_text().replace("\n", "\r\n").encode())
        else:
            shutil.copy2(path, args.share / path.name)
    if CLIP.is_file():
        target = args.share / "clips" / CLIP.name
        if not target.exists() or target.stat().st_size != CLIP.stat().st_size:
            shutil.copy2(CLIP, target)
    (args.share / "VERSION.txt").write_bytes(
        f"commit {version}\r\nbuilt {stamp}\r\nprograms {' '.join(built)}\r\n".encode())
    print(f"Published {', '.join(built)} ({version}) to {args.share}")


if __name__ == "__main__":
    main()
