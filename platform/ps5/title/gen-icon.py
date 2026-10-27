#!/usr/bin/env python3
# The title's placeholder icon: a plain 512x512 PNG with the name
# "PHOTO SPLAT VR" and a few grey dots. No Apple name or mark (spec, Licensing
# rule 5), no photos, no fonts: a 5x7 bitmap drawn by this file, so the icon is
# reproducible from source and carries no third-party asset.
#
#   python3 platform/ps5/title/gen-icon.py -o build/ps5/title/PPSA99202/sce_sys/icon0.png
#   python3 platform/ps5/title/gen-icon.py --check <file>     # size + format only
#
# The home screen wants 512x512; param.json and the runbook rely on that.
# Python 3 standard library only (zlib for the PNG stream, as ps5-env.sh's
# callers already require python3).
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import struct
import sys
import zlib

SIZE = 512
BACKGROUND = (0x14, 0x16, 0x1C)
BORDER = (0x3A, 0x3F, 0x4A)
DOTS = (0x4A, 0x50, 0x5E)
TEXT = (0xF2, 0xF4, 0xF8)
TITLE = "PHOTO SPLAT VR"

# 5x7 bitmap, one string per row; only the glyphs TITLE needs.
FONT = {
    " ": ("00000",) * 7,
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
}

# The splat: centre x, centre y, radius, colour. Plain circles, a hint of the
# shape of the thing, not a logo.
SPLAT = (
    (256, 372, 96, DOTS),
    (148, 396, 44, BORDER),
    (366, 404, 36, BORDER),
    (256, 430, 22, DOTS),
)


def blank():
    return [[BACKGROUND for _ in range(SIZE)] for _ in range(SIZE)]


def disc(pixels, cx, cy, radius, colour):
    r2 = radius * radius
    for y in range(max(cy - radius, 0), min(cy + radius + 1, SIZE)):
        dy = y - cy
        for x in range(max(cx - radius, 0), min(cx + radius + 1, SIZE)):
            dx = x - cx
            if dx * dx + dy * dy <= r2:
                pixels[y][x] = colour


def rectangle(pixels, x0, y0, x1, y1, colour):
    for y in range(max(y0, 0), min(y1, SIZE)):
        row = pixels[y]
        for x in range(max(x0, 0), min(x1, SIZE)):
            row[x] = colour


def text(pixels, message, scale, y0, colour):
    width = len(message) * 6 - 1
    x0 = (SIZE - width * scale) // 2
    for index, character in enumerate(message):
        glyph = FONT[character]
        for row, bits in enumerate(glyph):
            for column, bit in enumerate(bits):
                if bit != "1":
                    continue
                px = x0 + (index * 6 + column) * scale
                py = y0 + row * scale
                rectangle(pixels, px, py, px + scale, py + scale, colour)


def render():
    pixels = blank()
    disc(pixels, 256, 300, 210, (0x1B, 0x1E, 0x26))
    for cx, cy, radius, colour in SPLAT:
        disc(pixels, cx, cy, radius, colour)
    # Border: 4 px, inset 16 px, drawn as four bars so the corners stay square.
    rectangle(pixels, 16, 16, SIZE - 16, 20, BORDER)
    rectangle(pixels, 16, SIZE - 20, SIZE - 16, SIZE - 16, BORDER)
    rectangle(pixels, 16, 16, 20, SIZE - 16, BORDER)
    rectangle(pixels, SIZE - 20, 16, SIZE - 16, SIZE - 16, BORDER)
    text(pixels, TITLE, 5, 132, TEXT)
    return pixels


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path, pixels):
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter: none
        for r, g, b in row:
            raw += bytes((r, g, b))
    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)  # 8-bit RGB, no interlace
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", ihdr))
        out.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        out.write(chunk(b"IEND", b""))


def check(path):
    with open(path, "rb") as source:
        data = source.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return "not a PNG: %s" % path
    width, height = struct.unpack(">II", data[16:24])
    if (width, height) != (SIZE, SIZE):
        return "%s is %dx%d, expected %dx%d" % (path, width, height, SIZE, SIZE)
    return "%s: %dx%d PNG placeholder, no third-party asset" % (path, width, height)


def main():
    parser = argparse.ArgumentParser(description="generate the Photo Splat VR placeholder icon")
    parser.add_argument("-o", "--out", help="write the icon here")
    parser.add_argument("--check", help="check an existing icon")
    arguments = parser.parse_args()
    if arguments.check:
        problem = check(arguments.check)
        if not problem.startswith(arguments.check):
            sys.stderr.write("gen-icon: %s\n" % problem)
            return 1
        print(problem)
        return 0
    if not arguments.out:
        parser.error("either -o FILE or --check FILE")
    write_png(arguments.out, render())
    print(check(arguments.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
