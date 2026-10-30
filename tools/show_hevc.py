#!/usr/bin/env python3
"""Show HEVC through MPP and a DRM plane at 1080p60, then restore Android."""
import argparse
import datetime
from pathlib import Path
import shlex
import subprocess
import sys
import time

from device_session import launch_command, prune_command, session_script

ROOT = Path(__file__).resolve().parents[1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="127.0.0.1:5555")
    parser.add_argument("--input", type=Path,
                        default=ROOT / "build/assets/testsrc2-1920x1080-60fps-180frames-idr.hevc")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--seconds", type=int, default=60, help="Watchdog limit for the display test")
    parser.add_argument("--plane-color-space", type=int, default=3,
                        help="Vendor plane COLOR_SPACE (V4L2 numbering; 3 = BT.709 limited)")
    parser.add_argument("--keep-60hz", action="store_true",
                        help="Save this box's Rockchip HDMI setting as 1080p60 after Android returns")
    args = parser.parse_args(argv)
    if not 1 <= args.repeat <= 100 or not 5 <= args.seconds <= 300:
        parser.error("Use 1..100 repeats and a 5..300 second watchdog")
    if not 0 <= args.plane_color_space <= 12:
        parser.error("--plane-color-space must be 0..12")
    if not args.input.is_file():
        parser.error("Input is missing; run tools/run_hevc_test.py first")
    adb = ["adb", "-s", args.serial]

    def shell(command, **kwargs):
        kwargs.setdefault("timeout", 10)
        return subprocess.run(adb + ["shell", shlex.join(command)], check=True, **kwargs)

    def reconnect():
        if ":" not in args.serial:
            return
        for command in (["adb", "disconnect", args.serial], ["adb", "connect", args.serial]):
            try:
                subprocess.run(command, capture_output=True, timeout=5)
            except (subprocess.SubprocessError, OSError):
                # The framework may still be bringing Wi-Fi back up. Retry on
                # the next polling iteration; on-device cleanup is independent.
                pass

    if shell(["id", "-u"], capture_output=True, text=True).stdout.strip() != "0":
        raise RuntimeError("Root ADB is required to release Android's DRM display ownership")
    subprocess.run(["cmake", "--build", str(ROOT / "build/android"), "-j", "2"], check=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    remote = "/data/local/tmp/llstream-hevc/display-" + stamp
    results = ROOT / "build/results" / ("display-" + stamp)
    results.mkdir(parents=True)
    shell(["mkdir", "-p", remote])
    for source, name in [(ROOT / "build/android/hevc_decode_test", "hevc_decode_test"),
                         (args.input.resolve(), "test.hevc")]:
        subprocess.run(adb + ["push", str(source), remote + "/" + name], check=True)
    shell(["chmod", "755", remote + "/hevc_decode_test"])
    info = shell([remote + "/hevc_decode_test", "--display-info"], capture_output=True, text=True).stdout
    (results / "display-info.txt").write_text(info)
    if args.keep_60hz:
        key = "persist.vendor.resolution.HDMI-A-0"
        old = shell(["getprop", key], capture_output=True, text=True).stdout.strip()
        modes = [line.split(": ", 1)[1] for line in info.splitlines()
                 if line.startswith("Rockchip 1080p60 mode: ")]
        if not old or not modes:
            raise RuntimeError("Could not identify a supported Rockchip HDMI setting")
        (results / "hdmi-setting.txt").write_text(f"property={key}\nbefore={old}\nafter={modes[0]}\n")
        shell(["setprop", key, modes[0]])
        print("Saved HDMI mode:", modes[0], flush=True)
    # Shared watchdog owns service restoration even if ADB disconnects.
    command = [remote + "/hevc_decode_test", remote + "/test.hevc", "--display", "--fps", "60",
               "--repeat", str(args.repeat), "--csv", remote + "/timings.csv",
               "--plane-color-space", str(args.plane_color_space)]
    script = session_script(remote, command, args.seconds, display=True)
    local_script = results / "display-session.sh"
    local_script.write_text(script)
    subprocess.run(adb + ["push", str(local_script), remote + "/display-session.sh"], check=True)
    print("Showing moving HEVC test video at 1080p60. Android's display services will restart afterwards.", flush=True)
    launch = launch_command(remote, "display-session.sh", "display.log")
    subprocess.run(adb + ["shell", launch], check=True, timeout=10)
    deadline = time.monotonic() + args.seconds + 90
    code = None
    announced = False
    while time.monotonic() < deadline:
        time.sleep(0.5)
        try:
            if not announced:
                state = shell(["sh", "-c", "cat " + shlex.quote(remote + "/active-display.txt") +
                               " 2>/dev/null || true"], capture_output=True, text=True).stdout
                if "NV12" in state:
                    (results / "active-display.txt").write_text(state)
                    print("Kernel confirms NV12 scanout from the decoder, in Android's HDMI mode.", flush=True)
                    announced = True
            status = shell(["sh", "-c", "cat " + shlex.quote(remote + "/status") +
                            " 2>/dev/null || true"], capture_output=True, text=True).stdout.strip()
            if status:
                code = int(status)
                break
        except (subprocess.SubprocessError, OSError):
            reconnect()
    if code is None:
        raise RuntimeError("Could not retrieve session status; the device-side watchdog restores Android independently")
    subprocess.run(adb + ["pull", remote + "/display.log", str(results / "display.log")], check=True)
    print((results / "display.log").read_text(), end="")
    if code:
        raise RuntimeError(f"Display test exited with status {code}; see {results / 'display.log'}")
    # The device publishes status only after its restoration check has finished.
    for name in ("restoration-status", "restoration-display-check.txt", "restored-display.txt"):
        subprocess.run(adb + ["pull", remote + "/" + name, str(results / name)], check=True)
    if (results / "restoration-status").read_text().strip() != "1":
        raise RuntimeError("Device did not confirm Android restoration; see " + str(results))
    display_check = (results / "restoration-display-check.txt").read_text().strip()
    if args.keep_60hz and display_check.startswith("required:") and \
            "1920x1080p60" not in (results / "restored-display.txt").read_text():
        raise RuntimeError("Android restored, but its saved scanout is not 1080p60; see " + str(results))
    print("Restoration display check:", display_check)
    if not announced:
        raise RuntimeError("Test completed, but active NV12 scanout was not observed")
    subprocess.run(adb + ["pull", remote + "/timings.csv", str(results / "timings.csv")], check=True)
    shell(prune_command(str(Path(remote).parent), prefix="display-"))
    print("Restored Android services. Results:", results, flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
