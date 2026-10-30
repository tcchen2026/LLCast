#!/usr/bin/env python3
"""Run the native receiver on Android or this VM; --dry-run executes nothing."""
import argparse
from contextlib import ExitStack
import datetime
import hashlib
import ipaddress
import json
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import time

from profile import Collector
from device_session import launch_command, prune_command, session_script

ROOT = Path(__file__).resolve().parents[1]


class Adb:
    def __init__(self, serial, dry_run=False):
        self.serial, self.dry_run = serial, dry_run
        self.forwards = {}

    def run(self, *arguments, check=True, quiet=False):
        command = ["adb", "-s", self.serial, *map(str, arguments)]
        if not quiet:
            print("+ " + shlex.join(command), flush=True)
        if self.dry_run:
            return subprocess.CompletedProcess(command, 0, "", "")
        return subprocess.run(command, capture_output=True, text=True, timeout=15, check=check)

    def shell(self, *arguments, check=True, quiet=False):
        return self.run("shell", shlex.join(list(map(str, arguments))), check=check, quiet=quiet)

    def reconnect(self):
        # Reconnecting this endpoint does not require restarting the ADB server.
        if ":" in self.serial:
            command = ["adb", "connect", self.serial]
            print("+ " + shlex.join(command), flush=True)
            if not self.dry_run:
                try:
                    subprocess.run(command, capture_output=True, text=True, timeout=5, check=False)
                except (subprocess.SubprocessError, OSError):
                    pass
        for local, remote in self.forwards.items():
            try:
                self.run("forward", local, remote, check=False)
            except (subprocess.SubprocessError, OSError):
                pass # the next transport retry reinstalls it

    def profile_forward(self, remote_port, cleanup):
        remote = f"tcp:{remote_port}"
        allocated = self.run("forward", "tcp:0", remote).stdout.strip()
        if self.dry_run:
            return allocated
        local = "tcp:" + allocated
        # Register immediately, including a malformed allocation response.
        cleanup.callback(self.run, "forward", "--remove", local, check=False)
        if not allocated.isdecimal() or not 1 <= int(allocated) <= 65535:
            raise RuntimeError("ADB did not return its allocated profile port")
        self.forwards[local] = remote
        cleanup.callback(self.forwards.pop, local, None)
        return allocated


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--no-decode", action="store_true", help="Network probe; leaves Android services alone")
    mode.add_argument("--display", action="store_true", help="MPP/DRM under a restoring watchdog; requires root ADB")
    parser.add_argument("--local", action="store_true", help="Run the host receiver here; implies --no-decode, uses no ADB")
    parser.add_argument("--local-address", default="192.168.185.100", help="VM IPv4 address printed for the PC and used to bind the local profile collector")
    parser.add_argument("--serial", default="127.0.0.1:5555")
    parser.add_argument("--seconds", type=int, default=0, help="Optional timed test duration; default 0 runs indefinitely")
    parser.add_argument("--frames", type=int, default=0, help="Optional test frame limit (including counted losses)")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--profile", action="store_true", help="Collect rx/packet tables through TCP; no diagnostic files on the box")
    parser.add_argument("--profile-port", type=int, default=47801, help="TCP profile port; local mode also accepts PC profiles here")
    parser.add_argument("--dump", action="store_true", help="Save ordered complete AUs (including damaged references) as stream.hevc; --no-decode only")
    parser.add_argument("--loss-timeout-ms", type=int, default=20, help="Assembly deadline, 1..1000 ms; default 20")
    parser.add_argument("--plane-color-space", type=int)
    parser.add_argument("--interface", default="wlan0", help="Interface whose IPv4 address the PC should use")
    parser.add_argument("--build-dir", type=Path, help="Default: build/host for --local, otherwise build/android")
    parser.add_argument("--binary", type=Path, help="Use this prebuilt receiver instead of building")
    parser.add_argument("--no-build", action="store_true", help="Use build-dir/llstream_rx without rebuilding")
    parser.add_argument("--results", type=Path, help="New output directory; default build/results/<stamp>-rx (or -vmrx)")
    parser.add_argument("--dry-run", action="store_true", help="Print commands and, for Android, the device script without executing anything")
    args = parser.parse_args(argv)
    if args.local:
        if args.display:
            parser.error("--local supports --no-decode only")
        args.no_decode = True
    if not args.no_decode and not args.display:
        parser.error("Choose --local, --no-decode, or --display")
    args.build_dir = args.build_dir or ROOT / ("build/host" if args.local else "build/android")
    if args.dump and not args.no_decode:
        parser.error("--dump requires --no-decode")
    if args.dump and not (args.seconds or args.frames):
        parser.error("--dump requires --seconds or --frames")
    if not 1 <= args.profile_port <= 65535:
        parser.error("--profile-port must be 1..65535")
    if not 0 <= args.frames <= 0xffffffff:
        parser.error("--frames must be 0 (unlimited) or 1..4294967295")
    if not 1 <= args.loss_timeout_ms <= 1000:
        parser.error("--loss-timeout-ms must be 1..1000")
    try:
        ipaddress.IPv4Address(args.local_address)
    except ipaddress.AddressValueError:
        parser.error("--local-address must be an IPv4 address")
    if not 0 <= args.seconds <= 86400 or not 1 <= args.port <= 65535:
        parser.error("Use 0 (unlimited) or 1..86400 seconds and port 1..65535")
    if args.plane_color_space is not None and not args.display:
        parser.error("--plane-color-space requires --display")
    if args.plane_color_space is None:
        args.plane_color_space = 3
    if not 0 <= args.plane_color_space <= 12:
        parser.error("--plane-color-space must be 0..12")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,15}", args.interface):
        parser.error("Invalid interface name")
    return args


def box_addresses(output):
    values = re.findall(r"\binet\s+([0-9.]+)/[0-9]+", output)
    return [value for value in values if not ipaddress.IPv4Address(value).is_loopback]


def file_probe(remote, name):
    # ADB itself exits 1 on transport errors; do not confuse that with test -f.
    return ["sh", "-c", "if test -f " + shlex.quote(remote + "/" + name) +
            "; then echo present; else echo missing; fi"]


def collect(adb, remote, results, names, *, deadline):
    pending = list(names)
    missing = []
    dump_verified = False
    while pending:
        name = pending[0]
        try:
            if not dump_verified:
                exists = adb.shell(*file_probe(remote, name), check=False)
                if exists.returncode or exists.stdout.strip() not in ("present", "missing"):
                    raise RuntimeError(exists.stderr or "ADB file check failed")
                if exists.stdout.strip() == "missing":
                    missing.append(name)
                    pending.pop(0)
                    continue
                adb.run("pull", remote + "/" + name, results / name)
                if name == "stream.hevc":
                    size = adb.shell("stat", "-c", "%s", remote + "/" + name).stdout.strip()
                    if not size.isdecimal() or (results / name).stat().st_size != int(size):
                        raise RuntimeError("Dump size mismatch; device copy retained")
                    dump_verified = True
            if dump_verified:
                # Keep this state on a lost rm acknowledgement: retry removal,
                # not a pull of a possibly already removed, verified file.
                adb.shell("rm", "-f", remote + "/" + name)
            pending.pop(0)
            dump_verified = False
        except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Could not collect device results at {remote}: {exc}") from exc
            adb.reconnect()
            time.sleep(0.5)
    return missing


def stop_local(process):
    """The native receiver handles TERM and drains profiling/dump output at exit."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def run_local(args, stamp):
    if platform.system() != "Linux":
        raise RuntimeError("--local needs the Linux host receiver")
    results = (args.results or ROOT / "build/results" / (stamp + "-vmrx")).resolve()
    binary = (args.binary or args.build_dir / "llstream_rx").resolve()
    command = [str(binary), "--no-decode", "--port", str(args.port), "--loss-timeout-ms", str(args.loss_timeout_ms)]
    if args.profile:
        command += ["--profile-to", f"{args.local_address}:{args.profile_port}"]
    if args.seconds:
        command += ["--seconds", str(args.seconds)]
    if args.frames:
        command += ["--frames", str(args.frames)]
    if args.dump:
        command += ["--dump", str(results / "stream.hevc")]
    pc_command = ["bin\\llstream_win.exe", "--host", args.local_address, "--port", str(args.port),
                  "--pattern-motion", "8",
                  "--bitrate-mbps", "20"]
    if args.seconds:
        pc_command += ["--seconds", str(min(20, max(1, args.seconds - 10)))]
    if args.frames:
        pc_command += ["--frames", str(args.frames)]
    launcher = "Run " + subprocess.list2cmdline(["vmtest.bat", args.local_address]) + " on the PC (three 20 s runs, UDP 47800)."
    if args.port != 47800:
        launcher = "vmtest.bat uses UDP 47800; use the command below for this receiver's custom port."
    print("Results:", results, flush=True)
    if not args.binary and not args.no_build:
        builds = []
        if not (args.build_dir / "CMakeCache.txt").exists():
            builds.append(["cmake", "-S", str(ROOT), "-B", str(args.build_dir), "-DCMAKE_BUILD_TYPE=Release"])
        builds.append(["cmake", "--build", str(args.build_dir), "--target", "llstream_rx", "-j", "2"])
        for build in builds:
            print("+ " + shlex.join(build), flush=True)
            if not args.dry_run:
                subprocess.run(build, check=True)
    print("+ " + shlex.join(command), flush=True)
    lifetime = (f"Receiver duration: {args.seconds} s." if args.seconds else
                f"Receiver frame limit: {args.frames}." if args.frames else "Receiver runs indefinitely.")
    if args.dry_run:
        if args.profile:
            print(f"# profile.py --listen {args.local_address}:{args.profile_port} --save {shlex.quote(str(results))}")
        print("# Native stdout/stderr ->", results / "run.log")
        print("# Wait for UDP readiness. " + launcher)
        print("# Matching clean run for this receiver: " + subprocess.list2cmdline(pc_command))
        print("# " + lifetime + " Ctrl-C stops it.")
        return 0
    results.mkdir(parents=True, exist_ok=False)
    metadata = {"mode": "local-no-decode", "started_utc": stamp, "seconds": args.seconds, "frames": args.frames,
                "address": args.local_address, "port": args.port, "command": command,
                "pc_command": pc_command, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    (results / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    process, outcome, profiler = None, "failed", None
    try:
        if args.profile:
            profiler = Collector(listen=f"{args.local_address}:{args.profile_port}", save=results).start()
        with (results / "run.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 5
            ready = False
            while time.monotonic() < deadline:
                if "Receiver listening on UDP" in (results / "run.log").read_text():
                    ready = True
                    break
                if process.poll() is not None:
                    break
                time.sleep(.05)
            if not ready:
                raise RuntimeError("Local receiver did not become ready; inspect " + str(results / "run.log"))
            print(f"VM receiver ready: {args.local_address}:{args.port}; run on the PC now:", flush=True)
            print(launcher, flush=True)
            print("Matching clean run for this receiver:", flush=True)
            print(subprocess.list2cmdline(pc_command), flush=True)
            print(lifetime +
                  " Ctrl-C stops it and flushes logs.", flush=True)
            process.wait(timeout=args.seconds + 10 if args.seconds else None)
            outcome = "completed" if process.returncode == 0 else "failed"
    except KeyboardInterrupt:
        outcome = "interrupted"
    except subprocess.TimeoutExpired:
        outcome = "watchdog"
    finally:
        if process is not None:
            stop_local(process)
        if profiler:
            try:
                if process is not None and process.returncode in (0, -15, 130, 143):
                    profiler.wait_disconnect(program="llstream_rx")
            finally:
                profiler.close()
        metadata.update(status=process.returncode if process is not None else None, outcome=outcome)
        (results / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
        if process is not None:
            (results / "status").write_text(str(process.returncode) + "\n")
    print((results / "run.log").read_text(), end="")
    print("Results:", results, flush=True)
    if outcome == "interrupted":
        return 130
    if outcome == "watchdog":
        raise RuntimeError("Local receiver exceeded its shutdown deadline; inspect saved logs")
    if process.returncode:
        raise RuntimeError(f"Local receiver exited with status {process.returncode}; results: {results}")
    return 0


def main(argv=None):
    args = parse_args(argv)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    if args.local:
        return run_local(args, stamp)
    with ExitStack() as cleanup:
        return run_box(args, stamp, cleanup)


def run_box(args, stamp, cleanup):
    remote = "/data/local/tmp/llstream-rx/" + stamp
    results = (args.results or ROOT / "build/results" / (stamp + "-rx")).resolve()
    binary = (args.binary or args.build_dir / "llstream_rx").resolve()
    watchdog = args.seconds + 10 if args.seconds else 0
    adb = Adb(args.serial, args.dry_run)
    command = [remote + "/llstream_rx", "--port", str(args.port),
               "--loss-timeout-ms", str(args.loss_timeout_ms)]
    if args.profile:
        command += ["--profile", str(args.profile_port)]
    if args.seconds:
        command += ["--seconds", str(args.seconds)]
    if args.frames:
        command += ["--frames", str(args.frames)]
    command += (["--display", "--plane-color-space", str(args.plane_color_space)]
                if args.display else ["--no-decode"])
    if args.dump:
        command += ["--dump", remote + "/stream.hevc"]
    script = session_script(remote, command, watchdog, display=args.display)
    files = ["run.log", "program-status", "status"]
    if args.dump:
        files += ["stream.hevc", "stream.hevc.jsonl"]
    if args.display:
        files += ["services-before.txt", "before-display.txt", "before-drm-state.txt",
                  "active-display.txt", "active-drm-state.txt", "restored-display.txt",
                  "restored-drm-state.txt", "restoration-status"]
        files += ["restoration-display-check.txt"]
    print("Results:", results, flush=True)
    if not args.binary and not args.no_build:
        build = ["cmake", "--build", str(args.build_dir), "--target", "llstream_rx", "-j", "2"]
        print("+ " + shlex.join(build), flush=True)
        if not args.dry_run:
            subprocess.run(build, check=True)
    if not args.dry_run:
        results.mkdir(parents=True, exist_ok=False)
        (results / "receiver-session.sh").write_text(script)
    uid = adb.shell("id", "-u").stdout.strip()
    if args.display and not args.dry_run and uid != "0":
        raise RuntimeError("--display requires root ADB; no services were changed")
    address_output = adb.shell("ip", "-4", "addr", "show", "dev", args.interface).stdout
    addresses = box_addresses(address_output)
    if args.dry_run:
        print("Box IP: obtained from the preceding command; use it in stream.bat/probe.bat.")
    elif addresses:
        print("Box IPv4 for stream.bat/probe.bat:", ", ".join(addresses), "UDP", args.port, flush=True)
    else:
        raise RuntimeError("No IPv4 address on " + args.interface + "; choose --interface for the PC-to-box path")
    metadata = {"serial": args.serial, "remote_directory": remote, "mode": "display" if args.display else "no-decode",
                "seconds": args.seconds, "frames": args.frames, "watchdog_seconds": watchdog, "port": args.port,
                "addresses": addresses, "started_utc": stamp, "command": command}
    if not args.dry_run:
        metadata["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        (results / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    adb.shell("mkdir", "-p", remote)
    adb.run("push", binary, remote + "/llstream_rx")
    adb.shell("chmod", "755", remote + "/llstream_rx")
    adb.run("push", results / "receiver-session.sh", remote + "/receiver-session.sh")
    if args.dry_run:
        print("--- BEGIN DEVICE SCRIPT ---\n" + script + "--- END DEVICE SCRIPT ---", flush=True)
    if args.display:
        print("The device session will stop the compositor/HWC and restore their prior running state on exit, "
              "including the framework restart needed by this firmware.", flush=True)
    if not args.seconds and not args.frames:
        print("Receiver runs indefinitely, including while no sender is connected.", flush=True)
    if args.profile:
        forwarded = adb.profile_forward(args.profile_port, cleanup)
        if args.dry_run:
            print("# profile.py 127.0.0.1:FORWARDED_PORT --save " + shlex.quote(str(results)))
            print("# Finally: adb forward --remove tcp:FORWARDED_PORT (only this forward)")
        else:
            cleanup.enter_context(Collector(connect="127.0.0.1:" + forwarded, save=results))
    try:
        adb.run("shell", launch_command(remote))
    except (subprocess.SubprocessError, OSError):
        # Restarting display services can break the launching transport after
        # nohup has succeeded. Never launch a second copy; poll the first one.
        print("Launch transport interrupted; checking the detached session.", flush=True)
        adb.reconnect()
    status_command = ["sh", "-c", "cat " + shlex.quote(remote + "/status") + " 2>/dev/null || true"]
    if args.dry_run:
        print("# Poll every 30 s for unlimited sessions, 0.5 s for timed tests.")
        adb.shell(*status_command)
        print("# On an interrupted transport, retry the endpoint:")
        adb.reconnect()
        print("# For each existing artifact, pull it; record absent artifacts in run.json:")
        for name in files:
            adb.shell(*file_probe(remote, name))
            adb.run("pull", remote + "/" + name, results / name)
            if name == "stream.hevc":
                adb.shell("stat", "-c", "%s", remote + "/" + name)
                print("# Only after the local dump size matches the device size:")
                adb.shell("rm", "-f", remote + "/" + name)
        adb.shell(*prune_command(str(Path(remote).parent)))
        return 0
    deadline = time.monotonic() + watchdog + (90 if args.display else 30) if watchdog else float("inf")
    status = None
    while time.monotonic() < deadline:
        try:
            text = adb.shell(*status_command, quiet=True).stdout.strip()
            if text:
                if not re.fullmatch(r"[0-9]{1,3}", text):
                    raise RuntimeError("Invalid device status: " + repr(text))
                status = int(text)
                break
        except (subprocess.SubprocessError, OSError):
            adb.reconnect()
        time.sleep(0.5 if watchdog else 30)
    missing = collect(adb, remote, results, files, deadline=time.monotonic() + 60)
    metadata.update(status=status, missing_artifacts=missing)
    (results / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    adb.shell(*prune_command(str(Path(remote).parent)))
    if (results / "run.log").exists():
        print((results / "run.log").read_text(), end="")
    if status is None:
        raise RuntimeError("Session status unavailable; the independent watchdog owns cleanup. Device files: " + remote)
    if status:
        raise RuntimeError(f"Receiver/watchdog exited with status {status}; results: {results}")
    if "run.log" in missing:
        raise RuntimeError("Receiver completed without its required logs: " + ", ".join(missing))
    print("Completed. Results:", results, flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Stopped host monitoring. The detached Android receiver keeps running; its device session restores Android when it exits.",
              file=sys.stderr)
        sys.exit(130)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
