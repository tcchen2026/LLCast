"""Collect real host logs, then run both analyzer CLIs; no box or PC needed."""
from profile_capture import capture, ready
import csv
import datetime
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def rows(path):
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def run(receiver, sender, base, drop, fec=0):
    directory = base / ("fec" if fec else "drop" if drop else "clean")
    directory.mkdir()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = str(reservation.getsockname()[1])
    profiler = capture(directory)
    rx = subprocess.Popen([receiver, "--no-decode", "--port", port, "--loss-timeout-ms", "1000", "--seconds", "30",
                           "--profile-to", profiler.endpoint],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        assert select.select([rx.stdout], [], [], 5)[0], "Receiver startup timeout"
        first = ready(rx)
        assert "listening on UDP" in first, first
        tx = subprocess.run([sender, "127.0.0.1", "--port", port, "--synthetic-mbps", "2", "--frames", "75",
                             "--pace-mbps", "15", "--drop-rate", str(drop), "--fec-percent", str(fec),
                             "--profile-to", profiler.endpoint],
                            capture_output=True, text=True, timeout=10)
        (directory / "tx.log").write_text(tx.stdout + tx.stderr)
        assert tx.returncode == 0, tx.stdout + tx.stderr
        rx.terminate()
        output, _ = rx.communicate(timeout=5)
        (directory / "run.log").write_text(first + output)
        assert rx.returncode == 143, first + output
        profiler.wait_disconnect(2)
    finally:
        if rx.poll() is None:
            rx.kill()
            rx.wait()
        profiler.close()
    for name in ("stream", "probe"):
        command = [sys.executable, str(ROOT / "tools" / ("analyze_" + name + ".py")),
                   "--sender", str(directory / "sender.csv"), "--receiver", str(directory / "rx.csv"),
                   "--receiver-log", str(directory / "run.log"), "--json", str(directory / (name + ".json"))]
        command += (["--packets", str(directory / "packets.csv")] if name == "probe"
                    else ["--joined", str(directory / "joined.csv")])
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        (directory / (name + ".txt")).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    stream = json.loads((directory / "stream.json").read_text())
    probe = json.loads((directory / "probe.json").read_text())
    tx = rows(directory / "sender.csv")
    sessions = {int(row["session"]) for row in tx}
    assert len(tx) == 75 and len(sessions) == 1
    assert {int(row["session"]) for row in rows(directory / "clock.csv")} == sessions
    assert stream["sender_frames"] == sum(stream["fates"].values()) == 75
    assert stream["intervals"]["capture_to_flip"]["n"] == 0  # Never call probe timing display latency.
    assert stream["clock"]["mapped_frames"] == 75
    expected = sum(int(row["packets"]) for row in tx)
    injected = sum(int(row["dropped_packets"]) for row in tx)
    assert probe["expected_packets"] == expected
    assert probe["kernel_socket_drops"] == 0, probe
    assert probe["missing_packets"] == injected, probe
    assert sum(int(k) * value for k, value in probe["burst_length_histogram_packets"].items()) == injected
    assert bool(injected) == bool(drop)
    assert stream["damage_status_unknown_frames"] == 0
    assert probe["frames_repaired"] == stream["frames_repaired"]
    assert probe["missing_data_packets"] + probe["missing_parity_packets"] == injected
    assert "nack" not in probe
    if fec:
        assert stream["fates"]["delivered"] == 75 and stream["damaged_frames"] == 0
        assert probe["frames_incomplete"] == 0 and probe["frames_repaired"] > 0
        assert probe["expected_parity_packets"] == sum(int(row["fec_packets"]) for row in tx)
        assert probe["recommendation"] == "no decision"
    elif not drop:
        assert stream["fates"]["delivered"] == 75
        assert probe["recommendation"] == "no FEC"
    else:
        assert stream["fates"]["lost"] > 0
        assert probe["recommendation"] == "no decision"
    print(f"{directory}: frames=75 packets={expected} injected/missing={injected}, "
          f"kernel_drops=0, decision={probe['recommendation']}")


if __name__ == "__main__":
    receiver, sender, destination = sys.argv[1:]
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    base = Path(destination) / (stamp + "-" + str(os.getpid()))
    base.mkdir(parents=True)
    run(receiver, sender, base, 0)
    run(receiver, sender, base, .12)
    run(receiver, sender, base, .01, fec=20)
