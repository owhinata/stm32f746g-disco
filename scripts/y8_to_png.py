#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Convert a raw Y8 greyscale frame captured by `camera save` to PNG (issue #45).

The OV5640 Y8 mode emits one luma byte per pixel; the firmware writes it raw,
width*height bytes.  This maps directly to an 8-bit greyscale PNG.

Usage:
    python3 scripts/y8_to_png.py --width 320 --height 240 capture.raw out.png

Requires Pillow:  pip install Pillow
"""

import argparse
import sys

from PIL import Image


def main() -> int:
    ap = argparse.ArgumentParser(
        description="raw Y8 greyscale -> PNG (camera save output)")
    ap.add_argument("input", help="raw Y8 file (e.g. from `camera save`)")
    ap.add_argument("output", help="PNG file to write")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    expected = args.width * args.height
    if len(data) != expected:
        print(f"error: {args.input} is {len(data)} bytes, expected "
              f"{expected} ({args.width}x{args.height} Y8)", file=sys.stderr)
        return 1

    Image.frombytes("L", (args.width, args.height), data).save(args.output)
    print(f"wrote {args.output} ({args.width}x{args.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
