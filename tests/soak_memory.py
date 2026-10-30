"""Wall-clock synthetic soak of the production sender/receiver (Linux, no MPP).

Run explicitly; the default is 30 minutes at 60 fps / 24 Mbps. Records RSS and
private memory with profiling disabled. Process memory must plateau after warmup;
only this harness writes the sampled memory CSV.
"""
import argparse
import csv
import json
import pathlib
import re
import socket
import statistics
import subprocess
import time


def memory(pid):
    fields = {}
    for line in pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "kB":
            fields[parts[0].rstrip(":")] = int(parts[1])
    return fields["Rss"], fields["Private_Clean"] + fields["Private_Dirty"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receiver")
    parser.add_argument("sender")
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--mbps", type=float, default=24)
    args = parser.parse_args()
    assert args.seconds >= 10
    out = args.directory
    out.mkdir(parents=True, exist_ok=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = str(reservation.getsockname()[1])
    rx_command = [args.receiver, "--no-decode", "--port", port,
                  "--seconds", str(args.seconds + 2)]
    tx_command = [args.sender, "127.0.0.1", "--port", port,
                  "--frames", str(args.seconds * args.fps), "--fps", str(args.fps),
                  "--synthetic-mbps", str(args.mbps)]
    (out / "commands.json").write_text(json.dumps([rx_command, tx_command], indent=2) + "\n")
    rx = subprocess.Popen(rx_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tx = None
    measurements = []
    warmup = min(60, args.seconds / 4)
    interval = min(10, args.seconds / 10)
    try:
        first = rx.stdout.readline()
        assert "listening on UDP" in first, first
        with (out / "sender.log").open("w") as tx_log, (out / "memory.csv").open("w") as mem_log:
            writer = csv.writer(mem_log)
            writer.writerow(["elapsed_s", "rx_rss_kib", "rx_private_kib", "tx_rss_kib",
                             "tx_private_kib"])
            tx = subprocess.Popen(tx_command, stdout=tx_log, stderr=subprocess.STDOUT, text=True)
            start = time.monotonic()
            print(f"soak: receiver pid={rx.pid} sender pid={tx.pid}, {args.seconds}s", flush=True)
            while tx.poll() is None:
                elapsed = time.monotonic() - start
                assert rx.poll() is None, "receiver exited before sender"
                assert elapsed < args.seconds + 30, "sender exceeded soak deadline"
                try:
                    sample = [elapsed, *memory(rx.pid), *memory(tx.pid)]
                except (FileNotFoundError, ProcessLookupError):
                    assert tx.wait(timeout=5) == 0
                    break
                measurements.append(sample)
                writer.writerow(sample)
                mem_log.flush()
                time.sleep(min(interval, max(0.1, args.seconds + 0.5 - elapsed)))
            assert tx.wait(timeout=5) == 0, (out / "sender.log").read_text()
        rest, _ = rx.communicate(timeout=10)
        (out / "receiver.log").write_text(first + rest)
        assert rx.returncode == 0, first + rest
        steady = [m for m in measurements if m[0] >= warmup]
        assert len(steady) >= 5 and measurements[-1][0] >= args.seconds - interval - 1
        summary = {"seconds": args.seconds, "fps": args.fps, "payload_mbps": args.mbps,
                   "warmup_seconds": warmup, "samples": len(measurements)}
        for column, name in enumerate(["rx_rss", "rx_private", "tx_rss", "tx_private"], 1):
            values = [m[column] for m in steady]
            summary[name + "_kib"] = {"min": min(values), "max": max(values),
                "start": statistics.median(values[:3]), "end": statistics.median(values[-3:])}
        summary["profiling"] = False
        summary["sender_rows"] = int(re.search(r"Sender frames=(\d+)", (out / "sender.log").read_text())[1])
        summary["receiver_frames"] = int(re.search(r"Receiver frames=(\d+)", rest)[1])
        assert "Profile " not in first + rest + (out / "sender.log").read_text()
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        assert summary["sender_rows"] == args.seconds * args.fps
        assert summary["receiver_frames"] >= summary["sender_rows"] * 0.99
        for key, value in summary.items():
            if key.endswith("_kib"):
                assert value["max"] - value["min"] <= 2048, (key, value)
        print(json.dumps(summary, indent=2), flush=True)
        print("soak: PASS (post-warmup RSS/private-memory range <= 2 MiB per process)", flush=True)
    finally:
        for process in (tx, rx):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
