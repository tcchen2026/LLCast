#!/usr/bin/env python3
"""Draw the LLCast icon (a screen with a lightning bolt) at any size.

  make_icons.py --ico sender/win/llcast/llcast.ico --icns mac/LLCast.icns
  make_icons.py --png 512 icon.png [--png 192 other.png ...]

Needs Pillow. Each size is drawn at 4x and downsampled, so small sizes stay
sharp without a separate design.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw

TOP_LEFT = (37, 99, 235)      # blue
BOTTOM_RIGHT = (6, 182, 212)  # cyan
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]


def draw(size):
    s = size * 4
    gradient = Image.new("RGB", (s, s))
    pixels = gradient.load()
    for y in range(s):
        for x in range(s):
            t = (x + y) / (2 * (s - 1))
            pixels[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(TOP_LEFT, BOTTOM_RIGHT))
    mask = Image.new("L", (s, s), 0)
    margin = round(s * 0.04)
    ImageDraw.Draw(mask).rounded_rectangle((margin, margin, s - margin, s - margin),
                                           radius=round(s * 0.22), fill=255)
    icon = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    icon.paste(gradient, (0, 0), mask)

    d = ImageDraw.Draw(icon)
    white = (255, 255, 255, 255)
    stroke = max(round(s * 0.065), 4)
    d.rounded_rectangle((s * 0.19, s * 0.24, s * 0.81, s * 0.70), radius=round(s * 0.07),
                        outline=white, width=stroke)
    d.rounded_rectangle((s * 0.36, s * 0.77, s * 0.64, s * 0.77 + stroke), radius=stroke // 2, fill=white)
    bolt = [(0.555, 0.30), (0.39, 0.495), (0.49, 0.495), (0.445, 0.645),
            (0.61, 0.44), (0.51, 0.44), (0.555, 0.30)]
    d.polygon([(x * s, y * s) for x, y in bolt], fill=white)
    return icon.resize((size, size), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ico", type=Path, help="Windows .ico with the standard sizes")
    parser.add_argument("--icns", type=Path, help="macOS .icns")
    parser.add_argument("--png", nargs=2, action="append", default=[], metavar=("SIZE", "PATH"))
    args = parser.parse_args()
    if args.ico:
        images = [draw(n) for n in ICO_SIZES]
        images[-1].save(args.ico, format="ICO", sizes=[(n, n) for n in ICO_SIZES],
                        append_images=images[:-1])
    if args.icns:
        images = [draw(n) for n in ICNS_SIZES]
        images[-1].save(args.icns, format="ICNS", append_images=images[:-1])
    for size, path in args.png:
        draw(int(size)).save(path)


if __name__ == "__main__":
    main()
