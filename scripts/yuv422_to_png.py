#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Convert a raw YUV422 frame captured by `camera save` to PNG (issue #45).

The OV5640 emits packed YUV 4:2:2 as YUYV (Y0 U Y1 V, two pixels per four
bytes); the firmware writes it raw, width*height*2 bytes.  Luma/chroma are
converted to RGB with the full-range BT.601 coefficients (same as JPEG).

Usage:
    python3 scripts/yuv422_to_png.py --width 320 --height 240 capture.raw out.png
    python3 scripts/yuv422_to_png.py --order uyvy ... capture.raw out.png

Requires Pillow:  pip install Pillow
"""

import argparse
import sys

from PIL import Image


def clamp8(v: float) -> int:
    return 0 if v < 0 else (255 if v > 255 else int(v + 0.5))


def yuv422_to_rgb888(data: bytes, width: int, height: int, uyvy: bool) -> bytes:
    out = bytearray(width * height * 3)
    npix = width * height
    for i in range(0, npix, 2):                 # two pixels per 4-byte group
        b = 2 * i
        if uyvy:
            u, y0, v, y1 = data[b], data[b + 1], data[b + 2], data[b + 3]
        else:                                   # YUYV (OV5640 default)
            y0, u, y1, v = data[b], data[b + 1], data[b + 2], data[b + 3]
        for j, y in ((0, y0), (1, y1)):
            cr = v - 128
            cb = u - 128
            r = clamp8(y + 1.402 * cr)
            g = clamp8(y - 0.344136 * cb - 0.714136 * cr)
            bl = clamp8(y + 1.772 * cb)
            o = 3 * (i + j)
            out[o], out[o + 1], out[o + 2] = r, g, bl
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="raw YUV422 (YUYV) -> PNG (camera save output)")
    ap.add_argument("input", help="raw YUV422 file (e.g. from `camera save`)")
    ap.add_argument("output", help="PNG file to write")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--order", choices=("yuyv", "uyvy"), default="yuyv",
                    help="byte order (OV5640 default is yuyv)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    expected = args.width * args.height * 2
    if len(data) != expected:
        print(f"error: {args.input} is {len(data)} bytes, expected "
              f"{expected} ({args.width}x{args.height} YUV422)",
              file=sys.stderr)
        return 1

    rgb = yuv422_to_rgb888(data, args.width, args.height, args.order == "uyvy")
    Image.frombytes("RGB", (args.width, args.height), rgb).save(args.output)
    print(f"wrote {args.output} ({args.width}x{args.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
