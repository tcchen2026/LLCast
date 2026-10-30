"""Host receiver/dump integration, including the --local runner. No ADB/PC."""
from profile_capture import capture
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
# Frame completion ends the run. Wall-clock deadlines only detect a real hang;
# neither the receiver's recording window nor a fixed sleep races paced replay.
WAIT_TIMEOUT = 60


def port(kind=socket.SOCK_DGRAM):
    with socket.socket(socket.AF_INET, kind) as reservation:
        reservation.bind(("127.0.0.1", 0))
        return str(reservation.getsockname()[1])


def wait_ready(process, path, text):
    deadline = time.monotonic() + WAIT_TIMEOUT
    while time.monotonic() < deadline:
        if path.exists() and text in path.read_text():
            return
        if process.poll() is not None:
            break
        time.sleep(.05)
    raise AssertionError("Process not ready: " + path.read_text())


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def check_dump(receiver, sender, asset, output):
    udp_port = port()
    run = output / "vmrx"
    command = [sys.executable, str(ROOT / "tools/run_receiver.py"), "--local", "--binary", receiver,
               "--local-address", "127.0.0.1",
               "--dump", "--profile", "--frames", "180", "--loss-timeout-ms", "1000", "--port", udp_port,
               "--profile-port", port(socket.SOCK_STREAM),
               "--results", str(run)]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "runner.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_ready(process, output / "runner.log", "VM receiver ready")
            sent = subprocess.run([sender, "127.0.0.1", str(asset), "--port", udp_port,
                                   "--fps", "30", "--pace-mbps", "100"],
                                  capture_output=True, text=True, timeout=WAIT_TIMEOUT)
            (output / "sender.log").write_text(sent.stdout + sent.stderr)
            assert sent.returncode == 0, sent.stdout + sent.stderr
            assert process.wait(timeout=WAIT_TIMEOUT) == 0, (output / "runner.log").read_text()
        finally:
            stop(process)
    expected, received = asset.read_bytes(), (run / "stream.hevc").read_bytes()
    assert received == expected, f"Dump differs: received {len(received)}, expected {len(expected)} bytes"
    with (run / "rx.csv").open() as file:
        rows = list(csv.DictReader(file))
    assert len(rows) == 180 and all(row["fate"] == "delivered" for row in rows)
    assert all(int(row["codec"]) == 1 for row in rows)
    sessions = [json.loads(line) for line in (run / "stream.hevc.jsonl").read_text().splitlines()]
    assert len(sessions) == 1 and sessions[0]["codec"] == "HEVC" and sessions[0]["offset"] == 0
    assert [int(row["frame"]) for row in rows] == list(range(180))
    metadata = json.loads((run / "run.json").read_text())
    assert metadata["mode"] == "local-no-decode" and metadata["status"] == 0
    assert metadata["address"] == "127.0.0.1"
    assert "--pattern-motion" in metadata["pc_command"]
    assert "dumped_frames=180" in (run / "run.log").read_text()
    digest = hashlib.sha256(received).hexdigest()
    (output / "verification.json").write_text(json.dumps({"frames": 180, "bytes": len(received), "sha256": digest}, indent=2) + "\n")
    print(f"{run}: 180 ordered AUs, {len(received)} bytes, byte-identical HEVC; sha256={digest}")


def check_write_failure(receiver, sender, output):
    # Force an immediate write failure, not just a final stdio flush error:
    # the first synthetic key AU is > the receiver's 64 KiB output buffer.
    udp_port = port()
    profiler = capture(output / "full-device")
    with (output / "full-device.log").open("w") as log:
        process = subprocess.Popen([receiver, "--no-decode", "--dump", "/dev/full", "--port", udp_port,
                                    "--frames", "1", "--loss-timeout-ms", "1000", "--profile-to", profiler.endpoint],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_ready(process, output / "full-device.log", "Receiver listening")
            result = subprocess.run([sender, "127.0.0.1", "--port", udp_port, "--synthetic-mbps", "24",
                                     "--pace-mbps", "100", "--frames", "1"],
                                    capture_output=True, text=True, timeout=WAIT_TIMEOUT)
            (output / "full-device-sender.log").write_text(result.stdout + result.stderr)
            assert process.wait(timeout=WAIT_TIMEOUT) == 1
        finally:
            stop(process)
    assert "HEVC dump write" in (output / "full-device.log").read_text()
    profiler.wait_disconnect()
    profiler.close()
    with (output / "full-device" / "rx.csv").open() as file:
        rows = list(csv.DictReader(file))
    assert rows and all(row["fate"] != "delivered" for row in rows)
    print("/dev/full: receiver fails and does not report a successfully dumped AU")


def check_interrupt(receiver, output):
    udp_port = port()
    run = output / "interrupted-vmrx"
    with (output / "interrupted-runner.log").open("w") as log:
        process = subprocess.Popen([sys.executable, str(ROOT / "tools/run_receiver.py"), "--local",
                                    "--binary", receiver, "--profile", "--port", udp_port,
                                    "--local-address", "127.0.0.1",
                                    "--profile-port", port(socket.SOCK_STREAM),
                                    "--results", str(run)], stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_ready(process, output / "interrupted-runner.log", "VM receiver ready")
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=WAIT_TIMEOUT) == 130
        finally:
            stop(process)
    metadata = json.loads((run / "run.json").read_text())
    assert metadata["outcome"] == "interrupted" and metadata["status"] == 143, metadata
    assert (run / "rx.csv").read_text().startswith("session,frame,")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as released:
        released.bind(("127.0.0.1", int(udp_port)))  # Native child has actually stopped.
    print("Local Ctrl-C: native receiver drained, CSV closed, socket released, status recorded")


if __name__ == "__main__":
    receiver, sender, asset, destination = sys.argv[1:]
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = Path(destination) / (stamp + "-" + str(os.getpid()))
    output.mkdir(parents=True)
    check_dump(receiver, sender, Path(asset), output)
    check_write_failure(receiver, sender, output)
    check_interrupt(receiver, output)
