#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Convert a raw RGB565 frame captured by `camera save` to PNG.

The firmware writes the frame as raw little-endian RGB565 (R5 in bits 15..11,
G6 in 10..5, B5 in 4..0), 320x240 by default -- 153600 bytes.  Channels are
expanded to 8 bits by bit replication (so full-scale 5/6-bit values map to
exactly 255).

Usage:
    python3 scripts/rgb565_to_png.py capture.raw out.png
    python3 scripts/rgb565_to_png.py --width 320 --height 240 capture.raw out.png

Requires Pillow:  pip install Pillow
"""

import argparse
import sys

from PIL import Image


def rgb565_to_rgb888(data: bytes, width: int, height: int) -> bytes:
    out = bytearray(width * height * 3)
    for i in range(width * height):
        p = data[2 * i] | (data[2 * i + 1] << 8)   # little-endian
        r = (p >> 11) & 0x1F
        g = (p >> 5) & 0x3F
        b = p & 0x1F
        out[3 * i]     = (r << 3) | (r >> 2)       # bit-replicate 5 -> 8
        out[3 * i + 1] = (g << 2) | (g >> 4)       # 6 -> 8
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="raw little-endian RGB565 -> PNG (camera save output)")
    ap.add_argument("input", help="raw RGB565 file (e.g. from `camera save`)")
    ap.add_argument("output", help="PNG file to write")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    expected = args.width * args.height * 2
    if len(data) != expected:
        print(f"error: {args.input} is {len(data)} bytes, expected "
              f"{expected} ({args.width}x{args.height} RGB565)",
              file=sys.stderr)
        return 1

    rgb = rgb565_to_rgb888(data, args.width, args.height)
    Image.frombytes("RGB", (args.width, args.height), rgb).save(args.output)
    print(f"wrote {args.output} ({args.width}x{args.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
