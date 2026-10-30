"""Profile helpers for real loopback integration tests (no hardware)."""
from pathlib import Path
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from profile import Collector


def capture(directory):
    # Each test owns its files; stale append data must not survive a rerun.
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name in ("rx.csv", "sender.csv", "clock.csv", "packets.csv", "profile.jsonl"):
        (directory / name).unlink(missing_ok=True)
    return Collector(listen="127.0.0.1:0", save=directory, quiet=True).start()


def ready(process):
    lines = []
    while True:
        line = process.stdout.readline()
        lines.append(line)
        if "listening on UDP" in line:
            return "".join(lines)
        if not line:
            raise AssertionError("Receiver failed before readiness: " + "".join(lines))
