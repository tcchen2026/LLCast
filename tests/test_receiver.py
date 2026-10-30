"""Exercise the real POSIX receiver over loopback; no device dependencies."""
from profile_capture import capture, ready
import csv
import pathlib
import re
import socket
import subprocess
import sys


def rows(path):
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


receiver, sender, directory = sys.argv[1:]
directory = pathlib.Path(directory)
directory.mkdir(parents=True, exist_ok=True)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
    reservation.bind(("127.0.0.1", 0))
    port = str(reservation.getsockname()[1])
profiler = capture(directory)
rx_csv = directory / "rx.csv"
rx = subprocess.Popen(
    # Functional packet/CSV check: assembler unit tests cover the 20 ms default.
    # Give a descheduled VM time to process fragments already in its socket.
    [receiver, "--no-decode", "--port", port, "--frames", "60", "--loss-timeout-ms", "1000",
     "--profile-to", profiler.endpoint],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
)
try:
    first = ready(rx)
    assert "listening on UDP" in first, first
    assert int(re.search(r"rcvbuf_effective=(\d+)", first)[1]) > 0, first
    assert re.search(r"rcvbuf_method=SO_RCVBUF(?:FORCE)?\b", first), first
    sender_rows = []
    for session in range(2):
        tx_profile = capture(directory / f"sender-{session}")
        path = directory / f"sender-{session}" / "sender.csv"
        result = subprocess.run(
            [sender, "127.0.0.1", "--port", port, "--synthetic-mbps", "1",
             "--pace-mbps", "20", "--frames", "30", "--profile-to", tx_profile.endpoint],
            capture_output=True, text=True, timeout=5,
        )
        (directory / f"sender-{session}.log").write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        tx_profile.wait_disconnect()
        tx_profile.close()
        values = rows(path)
        assert len(values) == 30
        assert all(int(r["fate"]) == 6 for r in values), values
        assert all(int(r["decoded_delta_us"]) == 0xFFFFFFFF for r in values)
        assert all(int(r["flip_delta_us"]) == 0xFFFFFFFF for r in values)
        assert all(int(r["last_send_us"]) - int(r["first_send_us"]) >= 500 for r in values)
        assert any(int(r["clock_valid"]) for r in values)
        clocks = rows(path.parent / "clock.csv")
        assert len(clocks) >= 2
        for p in clocks:
            tx, rx_at = int(p["sender_tx_us"]), int(p["sender_rx_us"])
            rr, rt = int(p["receiver_rx_us"]), int(p["receiver_tx_us"])
            assert rt >= rr and rx_at >= tx
            assert int(p["rtt_us"]) == (rx_at - tx) - (rt - rr)
        sender_rows.extend(values)
    output, _ = rx.communicate(timeout=5)
    (directory / "receiver.log").write_text(first + output)
    assert rx.returncode == 0, first + output
    profiler.wait_disconnect()
    received = rows(rx_csv)
    assert len(received) == 60
    assert len({r["session"] for r in received}) == 2
    assert all(r["fate"] == "delivered" for r in received)
    assert all(int(r["complete_ns"]) >= int(r["first_rx_ns"]) > 0 for r in received)
    assert all(int(r["flip_ns"]) == 0 and int(r["decoded_ns"]) == 0 for r in received)
    # A loaded VM may deschedule a paired REALTIME/MONOTONIC read. Those
    # timestamps must be marked, and assembly intervals must stay nonnegative.
    fallback_count = int(re.search(r"timestamp_fallbacks=(\d+)", output)[1])
    trace_fallbacks = sum(int(p["timestamp_fallback"]) for p in rows(directory / "packets.csv"))
    assert fallback_count >= trace_fallbacks
    assert "kernel_drops=0" in output and "report_send_errors=0" in output, output
    print("receiver: two sessions, 60 delivered frames, kernel timestamps, pacing and clock CSV verified")
finally:
    if rx.poll() is None:
        rx.kill()
        rx.wait()

    profiler.close()
