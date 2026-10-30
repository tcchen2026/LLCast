#!/usr/bin/env python3
"""Regenerate the checked-in Android PNGs from the shared LLCast icon.

Requires Pillow and DejaVu Sans Bold; APK builds use the checked-in PNGs.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont

ANDROID = Path(__file__).resolve().parent
ROOT = ANDROID.parent


def main():
    launcher = ANDROID / "res/drawable-xxxhdpi/icon.png"
    banner = ANDROID / "res/drawable-xhdpi/banner.png"
    launcher.parent.mkdir(parents=True, exist_ok=True)
    banner.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        mark = Path(directory) / "mark.png"
        subprocess.run([sys.executable, str(ROOT / "tools/make_icons.py"),
                        "--png", "192", str(launcher), "--png", "512", str(mark)], check=True)
        # Compose at 4x, retaining the icon's square proportions in the TV banner.
        image = Image.new("RGBA", (1280, 720), (16, 28, 44, 255))
        with Image.open(mark) as icon:
            image.alpha_composite(icon, (56, 104))
        title = ET.parse(ANDROID / "res/values/strings.xml").getroot().find("string[@name='app_name']").text
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 128)
        ImageDraw.Draw(image).text((616, 360), title, font=font, fill="white", anchor="lm")
        image.convert("RGB").resize((320, 180), Image.Resampling.LANCZOS).save(banner)


if __name__ == "__main__":
    main()
