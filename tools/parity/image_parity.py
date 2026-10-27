#!/usr/bin/env python3
"""End-to-end image parity for the C3 SHARP port (spec "Testing").

Renders the reference `.ply` and the port's scene folder / `.psplat` with the
project's own renderer (`splat_desktop --screenshot`) from the capture pose and
from 8 fixed poses inside the +-5 cm comfort radius, then reports PSNR per pose
per photo.

    DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
    tools/parity/image_parity.py \
        --renderer build-S2-sharp/platform/desktop/splat_desktop \
        --out "$DATA"/runs/sharp_e2e/parity \
        "$DATA"/golden/photo01_living_room/reference.ply \
        "$DATA"/runs/sharp_e2e/photo01_living_room \
        ...

Reference scenes are `.ply`, port scenes are scene folders (the script renders
`scene.psplat`). The poses are the capture pose plus +-5 cm along x, y and z and
two mixed offsets, which is what the spec's comfort radius allows.

Both renders use the same size (the capture image size unless --size is given)
and the same intrinsics: the renderer reads SHARP's `intrinsic`/`image_size`
elements from a `.ply`, and the scene folder's `meta.json` for a `.psplat`, so
passing the same photo to both keeps the projection identical.

Exit status is 1 if any photo's mean PSNR over the 8 poses is below --gate.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass

POSES = [
    ("capture", (0.0, 0.0, 0.0, 0.0, 0.0)),
    ("x-5cm", (-0.05, 0.0, 0.0, 0.0, 0.0)),
    ("x+5cm", (0.05, 0.0, 0.0, 0.0, 0.0)),
    ("y-5cm", (0.0, -0.05, 0.0, 0.0, 0.0)),
    ("y+5cm", (0.0, 0.05, 0.0, 0.0, 0.0)),
    ("z-5cm", (0.0, 0.0, -0.05, 0.0, 0.0)),
    ("z+5cm", (0.0, 0.0, 0.05, 0.0, 0.0)),
    ("diag", (-0.04, 0.03, -0.03, 2.0, -1.5)),
]


# ---- minimal PNG reader (8-bit RGB/RGBA, non-interlaced) ---------------------

def read_png(path: str):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    offset = 8
    width = height = colour_type = 0
    idat = bytearray()
    while offset < len(data):
        (length,) = struct.unpack(">I", data[offset : offset + 4])
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, colour_type, *_ = struct.unpack(">IIBBBBB", payload)
            if bit_depth != 8 or colour_type not in (2, 6):
                raise ValueError(f"{path}: unsupported PNG (depth {bit_depth}, type {colour_type})")
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    channels = 3 if colour_type == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 3)
    previous = bytearray(stride)
    position = 0
    for y in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position : position + stride])
        position += stride
        if filter_type == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 0xFF
        elif filter_type == 2:
            for x in range(stride):
                line[x] = (line[x] + previous[x]) & 0xFF
        elif filter_type == 3:
            for x in range(stride):
                left = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((left + previous[x]) >> 1)) & 0xFF
        elif filter_type == 4:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = previous[x]
                c = previous[x - channels] if x >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + predictor) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"{path}: unsupported PNG filter {filter_type}")
        previous = line
        if channels == 3:
            out[y * width * 3 : (y + 1) * width * 3] = line
        else:
            row = out[y * width * 3 : (y + 1) * width * 3]
            row[0::3] = line[0::4]
            row[1::3] = line[1::4]
            row[2::3] = line[2::4]
    return width, height, bytes(out)


def psnr(a: bytes, b: bytes) -> float:
    if len(a) != len(b):
        raise ValueError("screenshots differ in size")
    total = 0
    for index in range(0, len(a), 4096):
        chunk_a = a[index : index + 4096]
        chunk_b = b[index : index + 4096]
        total += sum((x - y) * (x - y) for x, y in zip(chunk_a, chunk_b))
    mse = total / len(a)
    if mse == 0.0:
        return math.inf
    return 10.0 * math.log10((255.0 * 255.0) / mse)


@dataclass
class Scene:
    path: str
    size: str | None


def scene_path(path: str) -> Scene:
    if os.path.isdir(path):
        return Scene(os.path.join(path, "scene.psplat"), None)
    return Scene(path, None)


def capture_size(renderer_scene: str) -> str | None:
    """The scene's own capture size, so both renders use the same resolution."""
    if os.path.isdir(renderer_scene):
        meta = os.path.join(renderer_scene, "meta.json")
        if os.path.isfile(meta):
            with open(meta, "r", encoding="utf-8") as handle:
                root = json.load(handle)
            intr = root.get("intrinsics", {})
            if intr.get("width") and intr.get("height"):
                return f"{intr['width']}x{intr['height']}"
        return None
    # A .ply: read SHARP's image_size element out of the header + payload.
    with open(renderer_scene, "rb") as handle:
        header = b""
        while b"end_header" not in header:
            line = handle.readline()
            if not line:
                return None
            header += line
        if b"element image_size" not in header:
            return None
        # Walk the payload element by element until image_size.
        elements = []
        for line in header.split(b"\n"):
            words = line.split()
            if words[:1] == [b"element"]:
                elements.append([words[1].decode(), int(words[2]), []])
            elif words[:1] == [b"property"] and elements:
                elements[-1][2].append(words[1].decode())
        sizes = {"char": 1, "uchar": 1, "int8": 1, "uint8": 1, "short": 2, "ushort": 2,
                 "int16": 2, "uint16": 2, "int": 4, "uint": 4, "int32": 4, "uint32": 4,
                 "float": 4, "float32": 4, "double": 8, "float64": 8}
        for name, count, properties in elements:
            stride = sum(sizes[p] for p in properties)
            if name == "image_size":
                payload = handle.read(count * stride)
                width, height = struct.unpack("<II", payload[:8])
                return f"{width}x{height}"
            handle.seek(count * stride, 1)
    return None


def render(renderer: str, scene: str, pose, output: str, size: str | None) -> None:
    command = [renderer, scene, "--screenshot", output, "--pose", ",".join(f"{v:g}" for v in pose)]
    if size:
        command += ["--size", size]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise RuntimeError(f"render failed: {' '.join(command)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--renderer", required=True, help="path to splat_desktop")
    parser.add_argument("--out", required=True, help="screenshot directory")
    parser.add_argument("--gate", type=float, default=35.0, help="PSNR gate in dB (default 35)")
    parser.add_argument("--size", default=None, help="eye resolution WxH (default: capture size)")
    parser.add_argument("pairs", nargs="+", help="reference.ply + scene folder, repeated")
    arguments = parser.parse_args()
    if len(arguments.pairs) % 2 != 0:
        parser.error("expected pairs of <reference.ply> <scene-folder>")

    if arguments.out:
        os.makedirs(arguments.out, exist_ok=True)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="sharp-parity-") as scratch:
        for first in range(0, len(arguments.pairs), 2):
            reference_input = arguments.pairs[first]
            ours_input = arguments.pairs[first + 1]
            name = os.path.basename(os.path.normpath(ours_input))
            reference = scene_path(reference_input)
            ours = scene_path(ours_input)
            size = arguments.size or capture_size(reference_input) or capture_size(ours_input)
            scores = []
            print(f"photo {name} ({size or 'renderer default size'})")
            for pose_name, pose in POSES:
                reference_png = os.path.join(scratch, f"{name}-ref-{pose_name}.png")
                ours_png = os.path.join(scratch, f"{name}-port-{pose_name}.png")
                render(arguments.renderer, reference.path, pose, reference_png, size)
                render(arguments.renderer, ours.path, pose, ours_png, size)
                width, height, reference_pixels = read_png(reference_png)
                width_ours, height_ours, ours_pixels = read_png(ours_png)
                if (width, height) != (width_ours, height_ours):
                    raise RuntimeError(f"{name}: render sizes differ")
                value = psnr(reference_pixels, ours_pixels)
                scores.append(value)
                print(f"  {pose_name:<8} {width}x{height}  PSNR {value:6.2f} dB")
                if arguments.out:
                    for source, target in ((reference_png, "ref"), (ours_png, "port")):
                        destination = os.path.join(arguments.out, f"{name}-{target}-{pose_name}.png")
                        with open(source, "rb") as reader, open(destination, "wb") as writer:
                            writer.write(reader.read())
            mean = sum(scores) / len(scores)
            worst = min(scores)
            status = "PASS" if mean >= arguments.gate else "FAIL"
            print(f"  mean {mean:.2f} dB, worst {worst:.2f} dB, gate {arguments.gate:.1f} dB -> {status}")
            if mean < arguments.gate:
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
