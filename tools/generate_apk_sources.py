"""Generate immutable native text/guard from the shared source files."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from device_session import session_script

ROOT = Path(__file__).resolve().parents[1]


def waiting_header():
    strings = {node.attrib["name"]: node.text for node in
               ET.parse(ROOT / "android/res/values/strings.xml").getroot().findall("string")}
    # The card's font is A-Z, 0-9 and '.'; waiting_refresh's %1$s becomes printf's %d.
    return "/* Generated from android/res/values/strings.xml. */\n" + "\n".join(
        "#define LLCAST_" + name.upper() + " " + json.dumps(strings[name].replace("%1$s", "%d"), ensure_ascii=True)
        for name in ("waiting_title", "waiting_status", "waiting_start", "waiting_stop", "waiting_refresh")) + "\n"


def guard_header():
    script = session_script(None, [], 0, display=True, apk=True)
    return "/* Generated from tools/device_session.py. */\n#define LLCAST_SESSION_SCRIPT \\\n" + " \\\n".join(json.dumps(line + "\n") for line in script.splitlines()) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--waiting", type=Path)
    parser.add_argument("--guard", type=Path)
    args = parser.parse_args()
    for path, content in ((args.waiting, waiting_header), (args.guard, guard_header)):
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)
            text = content()
            if not path.exists() or path.read_text() != text:
                path.write_text(text)
