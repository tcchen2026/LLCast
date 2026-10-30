"""A huge frame jump must not flood reports/CSV or block PING/key recovery."""
from profile_capture import capture, ready
import csv
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time

receiver, directory = sys.argv[1:]
directory = pathlib.Path(directory)
directory.mkdir(parents=True, exist_ok=True)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
profiler = capture(directory)
path = directory / "rx.csv"
rx = subprocess.Popen([receiver, "--no-decode", "--port", str(port), "--seconds", "2",
                       "--profile-to", profiler.endpoint], stdout=subprocess.PIPE,
                      stderr=subprocess.STDOUT, text=True)
session, jump = 17, 1 << 30
reports, recovery = {}, []


def common(kind):
    return struct.pack("<HBBI", 0x534C, 2, kind, session)


try:
    first = ready(rx)
    assert "listening on UDP" in first, first
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("127.0.0.1", port))
        sock.settimeout(1)
        sequence = 0

        def video(frame, key=False):
            global sequence
            sock.send(common(1) + struct.pack("<IIIHHBBHQIIB3x", sequence, frame, 17,
                0, 1, int(key), 0, 0, 12345, 100, 200, 1) + bytes(17))
            sequence += 1

        video(0, True)
        video(jump)
        ping_time = time.monotonic()
        sock.send(common(2) + struct.pack("<IQ", 123, 456))
        video(jump + 1, True)
        for frame in range(jump + 2, jump + 102):
            video(frame)
        pong = False
        while len(reports) < 104 or not pong:
            packet = sock.recv(1400)
            assert packet[:8] == common(packet[3]), packet
            if packet[3] == 3:
                assert struct.unpack_from("<IQ", packet, 8) == (123, 456)
                assert time.monotonic() - ping_time < 1
                pong = True
            elif packet[3] == 4:
                frame = struct.unpack_from("<I", packet, 8)[0]
                assert frame not in reports, ("duplicate report", frame)
                reports[frame] = packet[12]
                assert packet[13] == int(frame == jump)
            elif packet[3] == 5:
                recovery.append(struct.unpack_from("<I", packet, 8)[0])
            else:
                raise AssertionError(packet)
        assert recovery == [jump - 1], recovery
        assert reports[1] == 3 and reports[jump] == 6
        assert sum(fate == 3 for fate in reports.values()) == 1
        assert sum(fate == 6 for fate in reports.values()) == 103
        deadline = time.monotonic() + 2
        while (not path.exists() or path.stat().st_size < 4096) and time.monotonic() < deadline:
            time.sleep(.01)
        assert rx.poll() is None and path.stat().st_size > 4096, "Collector must stream before exit"
    rest, _ = rx.communicate(timeout=5)
    (directory / "receiver.log").write_text(first + rest)
    assert rx.returncode == 0, first + rest
    profiler.wait_disconnect()
    with path.open(newline="") as file:
        rows = list(csv.DictReader(file))
    assert len(rows) == 104
    lost = [r for r in rows if r["fate"] == "lost"]
    assert len(lost) == 1 and int(lost[0]["frame"]) == 1 and int(lost[0]["span"]) == jump - 1
    assert all(int(r["span"]) == 1 for r in rows if r["fate"] != "lost")
    assert [int(r["frame"]) for r in rows if r["damaged"] == "1"] == [jump]
    assert sum(int(r["span"]) for r in rows) == jump + 102
    assert f"lost={jump - 1}" in rest and "rows_live=0" in rest, rest
    assert int(re.search(r"rows_peak=(\d+)", rest)[1]) <= 3, rest
    print("receiver gap: one span row/report, PING responsive, damaged delivery and key recovery verified")
finally:
    if rx.poll() is None:
        rx.kill()
        rx.wait()

    profiler.close()
