"""Small real receiver/replay fixture for check_vmtest; no PC, box, or GPU.

The replay follows pre-encoded pattern offsets 0..7. A proxy drops one known
fragment and suppresses RECOVER so the fixed IDR at frame 5 models delayed
encoder recovery without replay's jump changing the pattern's wire index.
The proxy releases complete AUs in small PONG-gated bursts, insulating the
receiver deadline from load-stretched sender pacing. Only the Windows pattern banner/stage sidecar are synthesized; transport,
clock, receiver, packet, damage, FEC and HEVC dump data are produced by C code.
"""
import copy
from collections import deque
from profile_capture import capture
import csv
import datetime
import json
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_vmtest


def rows(path):
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def write_rows(path, data):
    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(data[0]))
        writer.writeheader()
        writer.writerows(data)


def probe(path):
    return json.loads(check_vmtest.run_checked([
        "ffprobe", "-v", "error", "-threads", "1", "-show_packets", "-show_frames", "-show_entries",
        "packet=pos,size,flags:frame=pkt_pos,pkt_size,width,height,pix_fmt,key_frame,pict_type", "-of", "json", str(path)]).stdout)


def make_input(reference, output):
    with tempfile.TemporaryDirectory(prefix="vmtest-input-") as directory:
        scratch = Path(directory)
        raw, image, encoded = scratch / "frames.nv12", scratch / "reference.nv12", scratch / "frames.hevc"
        with raw.open("wb") as file:
            for frame in range(8):
                check_vmtest.run_checked([reference, "--reference", str(image), "1920", "1080", "--offset", str(frame * 8)])
                file.write(image.read_bytes())
        check_vmtest.run_checked([
            "ffmpeg", "-hide_banner", "-v", "error", "-f", "rawvideo", "-pixel_format", "nv12",
            "-video_size", "1920x1080", "-framerate", "30", "-i", str(raw), "-an", "-c:v", "libx265",
            "-preset", "ultrafast", "-tune", "zerolatency", "-x265-params",
            "lossless=1:keyint=5:min-keyint=5:scenecut=0:open-gop=0:bframes=0:repeat-headers=1:aud=1:info=0:pools=none:frame-threads=1:log-level=error",
            "-pix_fmt", "yuv420p", "-f", "hevc", str(encoded)])
        data = encoded.read_bytes()
        packets = [p for p in probe(encoded)["packets_and_frames"] if p["type"] == "packet"]
        assert len(packets) == 8
        # A valid suffix filler NAL ensures even tiny P AUs have fragment 1.
        filler = b"\x00\x00\x00\x01\x4c\x01" + b"\xff" * 2048 + b"\x80"
        with output.open("wb") as file:
            for packet in packets:
                offset, size = int(packet["pos"]), int(packet["size"])
                file.write(data[offset:offset + size] + filler)
    return [int("K" in packet["flags"]) for packet in packets]


class Proxy:
    def __init__(self, receiver_port, drop):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.receiver = ("127.0.0.1", receiver_port)
        self.drop, self.dropped = drop, 0
        self.stop = threading.Event()
        self.drained = threading.Event()
        self.errors = []
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        peer = None
        frames, ready, fates = {}, deque(), set()
        waiting = None
        gate_id = 0
        gate_at = 0
        gate_packet = b''
        gate_clock = 1 << 63  # private fixture ping; never forwarded to the sender
        try:
            while not self.stop.is_set():
                if ready and waiting is None:
                    packets = ready[0]
                    batch = [packets.popleft() for _ in range(min(16, len(packets)))]
                    for packet in batch:
                        self.sock.sendto(packet, self.receiver)
                    session = struct.unpack_from('<I', batch[0], 4)[0]
                    gate_id += 1
                    waiting = (session, gate_id)
                    # Gate small bursts on the receiver processing them, not
                    # on sleeps stretched by scheduler load. UDP order is local.
                    gate_packet = struct.pack('<HBBIIQ', 0x534c, 2, 2, session, gate_id, gate_clock)
                    gate_at = 0
                    if not packets:
                        ready.popleft()
                # A synchronization PONG can share a loaded UDP socket with
                # incoming test video. Retry only this fixture control ping;
                # video packets are sent exactly once, including the chosen loss.
                if waiting is not None and time.monotonic() >= gate_at:
                    self.sock.sendto(gate_packet, self.receiver)
                    gate_at = time.monotonic() + .1
                if len(fates) == 8 and not frames and not ready and waiting is None:
                    self.drained.set()
                if not select.select([self.sock], [], [], .05)[0]:
                    continue
                data, address = self.sock.recvfrom(65536)
                if address == self.receiver:
                    if data[3] == 3 and struct.unpack_from('<Q', data, 12)[0] == gate_clock:
                        if waiting == struct.unpack_from('<II', data, 4):
                            waiting = None
                        continue
                    if data[3] == 4:
                        fates.add(struct.unpack_from('<I', data, 8)[0])
                    if data[3] != 5 and peer:  # Fixed recovery IDR, not replay's jump.
                        self.sock.sendto(data, peer)
                else:
                    peer = address
                    if data[3] != 1:
                        self.sock.sendto(data, self.receiver)
                        continue
                    session, frame = struct.unpack_from('<I', data, 4)[0], struct.unpack_from('<I', data, 12)[0]
                    index, count = struct.unpack_from('<HH', data, 20)
                    parity = struct.unpack_from('<H', data, 26)[0]
                    pending = frames.setdefault((session, frame), {})
                    pending[index] = data
                    if len(pending) == count + parity:
                        # Collect the complete test AU before starting its RX
                        # deadline. The paced sender can take >1 s under load.
                        packets = deque(pending[i] for i in range(count + parity)
                                        if not (self.drop and frame == 2 and i == 1))
                        self.dropped += int(self.drop and frame == 2)
                        ready.append(packets)
                        del frames[(session, frame)]
        except BaseException as exc:
            self.errors.append(exc)
            self.drained.set()

    def wait_drained(self):
        assert self.drained.wait(60) and not self.errors, self.errors

    def close(self):
        self.stop.set()
        self.thread.join(timeout=60)
        self.sock.close()
        assert not self.thread.is_alive() and not self.errors, self.errors


def record(receiver, sender, reference, base):
    rx_dir, pc_dir = base / "rx", base / "pc"
    rx_dir.mkdir()
    pc_dir.mkdir()
    asset = base / "pattern.hevc"
    keys = make_input(reference, asset)
    assert keys == [1, 0, 0, 0, 0, 1, 0, 0]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    rx_profile = capture(rx_dir)
    # Twenty-four expected frames; the explicit bound permits the final parity
    # and gate PONG to drain before the parent signals completion.
    rx = subprocess.Popen([receiver, "--no-decode", "--port", str(port), "--loss-timeout-ms", "1000", "--frames", "25",
                           "--profile-to", rx_profile.endpoint,
                           "--dump", str(rx_dir / "stream.hevc")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        assert select.select([rx.stdout], [], [], 60)[0]
        first = ""
        while "Receiver listening on UDP" not in first:
            line = rx.stdout.readline()
            assert line, first
            first += line
        for name, drop, fec in (("clean", False, 0), ("loss", True, 0), ("fec", True, 20)):
            proxy = Proxy(port, drop)
            tx_profile = capture(pc_dir / name)
            path = pc_dir / name / "sender.csv"
            try:
                sent = check_vmtest.run_checked([
                    sender, "127.0.0.1", str(asset), "--port", str(proxy.port), "--frames", "8", "--fps", "30",
                    "--pace-mbps", "10", "--fec-percent", str(fec), "--profile-to", tx_profile.endpoint])
                proxy.wait_drained()
            finally:
                proxy.close()
                tx_profile.wait_disconnect(timeout=60)
                tx_profile.close()
            assert proxy.dropped == int(drop)
            (pc_dir / f"run-{name}.log").write_text(
                "Synthetic replay fixture; pattern banner/stages below model the Windows metadata.\n"
                "Pattern source=1920x1080 motion=8 px/frame right; frame zero unshifted\n" + sent.stdout + sent.stderr)
        if rx.poll() is None:
            rx.terminate()
        remaining, _ = rx.communicate(timeout=60)
        (rx_dir / "run.log").write_text(first + remaining)
        assert rx.returncode in (0, 143), first + remaining
        rx_profile.wait_disconnect(timeout=60)
    finally:
        if rx.poll() is None:
            rx.kill()
            rx.wait()
        rx_profile.close()
    return rx_dir, pc_dir


def check_cli(rx_dir, pc_dir, reference, output):
    result = check_vmtest.run_checked([sys.executable, str(ROOT / "tools/check_vmtest.py"),
                                      str(rx_dir), str(pc_dir), "--reference", reference, "--json", str(output)])
    output.with_suffix(".txt").write_text(result.stdout)
    assert "not the box's Wi-Fi" in result.stdout
    assert "wire frame*8" in result.stdout
    assert "QPC-derived on Windows" in result.stdout and "receiver CLOCK_MONOTONIC" in result.stdout
    return json.loads(output.read_text())


def test(receiver, sender, reference, base):
    rx_dir, pc_dir = record(receiver, sender, reference, base)
    report = check_cli(rx_dir, pc_dir, reference, base / "check.json")
    clean, loss, fec = report["sessions"]
    for session in (clean, loss, fec):
        assert session["counts"]["sent"] == 8
        assert session["counts"]["recovery_idrs_sent"] == 1
        assert session["pixels"]["clean"]["y"]["min"] == "inf", session
        assert not session["counts"]["injected_drops"]
    assert clean["counts"]["delivered"] == 8 and clean["counts"]["other_missing"] == 0
    assert loss["counts"]["delivered"] == 7 and loss["counts"]["lost"] == 1
    assert loss["counts"]["damaged"] == 2 and loss["counts"]["other_missing"] == 1
    assert loss["pixels"]["damaged"]["y"]["n"] == 2
    assert float(loss["pixels"]["damaged"]["y"]["min"]) < 50
    event, = loss["recovery"]
    assert (event["lost_frame"], event["key_frame"], event["frames"]) == (2, 5, 3)
    assert event["ms"] > 0 and event["uncertainty_ms"] > 0
    assert fec["counts"]["delivered"] == 8 and fec["counts"]["lost"] == 0
    assert fec["counts"]["repaired"] == 1 and fec["counts"]["other_missing"] == 1
    assert len(report["pictures"]) == 23 and all(p["decoded"] for p in report["pictures"])

    # A folder with just the clean case is sufficient.
    clean_rx, clean_pc = base / "clean-rx", base / "clean-pc"
    clean_rx.mkdir()
    clean_pc.mkdir()
    for path in pc_dir.glob("*clean*"):
        if path.is_dir():
            shutil.copytree(path, clean_pc / path.name)
        else:
            shutil.copyfile(path, clean_pc / path.name)
    for name in ("rx.csv", "packets.csv"):
        write_rows(clean_rx / name, [r for r in rows(rx_dir / name) if int(r["session"]) == clean["session"]])
    size = sum(p["bytes"] for p in report["pictures"] if p["session"] == clean["session"])
    (clean_rx / "stream.hevc").write_bytes((rx_dir / "stream.hevc").read_bytes()[:size])
    shutil.copyfile(rx_dir / "run.log", clean_rx / "run.log")
    alone = check_cli(clean_rx, clean_pc, reference, base / "clean-check.json")
    assert len(alone["sessions"]) == 1 and alone["sessions"][0]["counts"] == clean["counts"]

    sessions, receiver_rows = check_vmtest.load_sessions(rx_dir, pc_dir)
    # File-only captures share the same collector directory/table. Session 0
    # must be ignored before validating/filtering the network session IDs.
    combined_path = pc_dir / 'clean' / 'sender.csv'
    combined_original = combined_path.read_bytes()
    combined_rows = rows(combined_path)
    try:
        write_rows(combined_path, [dict(combined_rows[0], session='0')] + combined_rows)
        mixed, _ = check_vmtest.load_sessions(rx_dir, pc_dir)
        assert mixed.keys() == sessions.keys()
    finally:
        combined_path.write_bytes(combined_original)
    pictures = check_vmtest.expected_pictures(sessions, receiver_rows, (rx_dir / "stream.hevc").stat().st_size)
    metadata = probe(rx_dir / "stream.hevc")
    data = (rx_dir / "stream.hevc").read_bytes()
    # Equal total length cannot hide incorrect AU boundaries. Moving one onto the
    # next NAL's start code is wrong; moving it past a zero_byte is not.
    shifted = copy.deepcopy(pictures)
    zero = next(p for p in shifted[1:] if data[p["offset"]:p["offset"] + 4] == b"\0\0\0\1")
    before = shifted[shifted.index(zero) - 1]
    before["bytes"] += 1
    zero["offset"] += 1
    zero["bytes"] -= 1
    check_vmtest.map_decoded(metadata, shifted, data)
    wrong = copy.deepcopy(pictures)
    wrong[0]["bytes"] += 8
    wrong[1]["offset"] += 8
    wrong[1]["bytes"] -= 8
    try:
        check_vmtest.map_decoded(metadata, wrong, data)
    except ValueError as exc:
        assert "AU offset/size/key mismatch" in str(exc)
    else:
        raise AssertionError("Accepted mismatched AU boundaries")
    # A decoder omission must not shift the references for later pictures.
    missing = next(p for p in pictures if p["session"] == loss["session"] and p["frame"] == 3)
    filtered = {"packets_and_frames": [r for r in metadata["packets_and_frames"]
                if not (r["type"] == "frame" and int(r["pkt_pos"]) == missing["offset"])]}
    mapped = check_vmtest.map_decoded(filtered, pictures, data)
    assert len(mapped) == 22 and not missing["decoded"]
    assert [p["frame"] for p in mapped if p["session"] == loss["session"]] == [0, 1, 4, 5, 6, 7]

    # Delivery, not merely transmission of an IDR, ends recovery.
    lost_key_rows = copy.deepcopy(receiver_rows.rows)
    for row in lost_key_rows:
        if int(row["session"]) == loss["session"] and int(row["frame"]) == 5:
            row.update(fate="lost", complete_ns="0")
    events = check_vmtest.recovery_events(sessions[loss["session"]], check_vmtest.FrameIndex(lost_key_rows))
    assert [event["lost_frame"] for event in events] == [2, 5]
    assert all(event["key_frame"] is None and event["ms"] is None for event in events)
    no_clock = dict(sessions[loss["session"]], clocks=check_vmtest.ClockSamples([]))
    event, = check_vmtest.recovery_events(no_clock, receiver_rows)
    assert event["frames"] == 3 and event["ms"] is None and event["uncertainty_ms"] is None

    # Sender-injected omissions must be separated from other missing packets.
    transport = pc_dir / "loss" / "sender.csv"
    original = transport.read_bytes()
    changed = rows(transport)
    changed[2]["dropped_packets"] = "1"
    try:
        write_rows(transport, changed)
        modified, _ = check_vmtest.load_sessions(rx_dir, pc_dir)
        loss_probe = modified[loss["session"]]["probe"]
        assert loss_probe["injected_drops"] == 1 and loss_probe["unexplained_missing_packets"] == 0
    finally:
        transport.write_bytes(original)
    print(f"{base}: three sessions, clean-only, loss -> damaged pictures -> key, FEC repair, and checked AU/picture mapping passed")


if __name__ == "__main__":
    receiver, sender, reference, destination = sys.argv[1:]
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    base = Path(destination).resolve() / stamp
    base.mkdir(parents=True)
    test(receiver, sender, reference, base)
