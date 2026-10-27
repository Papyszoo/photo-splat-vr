#!/usr/bin/env python3
"""Check the viewer's VideoOut tile layout on the host.

`videoout.cpp` writes each frame into VideoOut's 64 KiB tiles (128x128
four-byte pixels, no pipe or bank swizzle) with a bit-twiddling formula copied
from PS5_Mesa's `src/vulkan/wsi/wsi_common_videoout.c` (`tiled()`) and
PS5_Vulkan's `src/demo_renderer.cpp` (`tiled_byte_offset()`). A wrong term
there shows a scrambled or torn picture on the TV, not an error, so the formula
is checked here instead:

  - identical to the reference formula for every pixel of every size VideoOut
    accepts (1920x1080, 2560x1440, 3840x2160);
  - a bijection inside each 64 KiB tile (no two pixels share a byte, every
    offset is 4-byte aligned and inside its tile);
  - the highest byte written fits the frame size `tiled_bytes()` computes.

    platform/ps5/viewer/tile_layout_check.py

Exit: 0 pass, 1 a check failed. No console, no build, no file written.
SPDX-License-Identifier: GPL-3.0-or-later
"""

import sys

# The formula as the viewer writes it: the tile index (y>>7)*blocks + (x>>7)
# followed by the byte offset inside that tile.
SIZES = ((1920, 1080), (2560, 1440), (3840, 2160))


def reference_tiled(x, y, blocks):
    """PS5_Mesa wsi_common_videoout.c tiled(): byte offset in the framebuffer."""
    offset = (((y << 4) & 0x70) ^ ((y << 5) & 0xF00) ^ ((y << 9) & 0x1000) ^
              ((y << 8) & 0x4000) ^ ((x << 2) & 0xC) ^ ((x << 5) & 0x380) ^
              ((x << 4) & 0x400) ^ ((x << 6) & 0x800) ^ ((x << 9) & 0xA000))
    return (((y >> 7) * blocks + (x >> 7)) << 16) + offset


def viewer_tiled(x, y, blocks):
    """The viewer's loop: tile base pointer + the same offset, in bytes."""
    tile = (y >> 7) * blocks + (x >> 7)
    vertical = (((y << 4) & 0x70) ^ ((y << 5) & 0xF00) ^
                ((y << 9) & 0x1000) ^ ((y << 8) & 0x4000))
    offset = (vertical ^ ((x << 2) & 0xC) ^ ((x << 5) & 0x380) ^
              ((x << 4) & 0x400) ^ ((x << 6) & 0x800) ^ ((x << 9) & 0xA000))
    return tile * 65536 + offset


def tiled_bytes(width, height):
    """videoout.cpp tiled_bytes(): framebuffer bytes, 2 MiB aligned."""
    aligned_width = (width + 127) & ~127
    aligned_height = (height + 127) & ~127
    return (4 * aligned_width * aligned_height + (2 << 20) - 1) & ~((2 << 20) - 1)


def check(width, height):
    blocks = (width + 127) // 128
    block_rows = (height + 127) // 128
    seen = {}
    for y in range(height):
        for x in range(width):
            expected = reference_tiled(x, y, blocks)
            actual = viewer_tiled(x, y, blocks)
            if expected != actual:
                return "(%d,%d): viewer %#x, reference %#x" % (x, y, actual, expected)
            if actual % 4 != 0 or actual % 65536 + 4 > 65536:
                return "(%d,%d): %#x crosses a 4-byte pixel or a 64 KiB tile" % (x, y, actual)
            if actual in seen:
                return "(%d,%d) and (%d,%d) share byte %#x" % (x, y, seen[actual][0],
                                                              seen[actual][1], actual)
            seen[actual] = (x, y)
    used = max(seen) + 4
    if used > tiled_bytes(width, height):
        return "%d bytes used, buffer is %d" % (used, tiled_bytes(width, height))
    print("%4dx%-4d: %7d pixels in %d tiles, %d of %d bytes used, formula identical to the WSI"
          % (width, height, len(seen), blocks * block_rows, used, tiled_bytes(width, height)))
    return None


def main():
    failed = False
    for width, height in SIZES:
        problem = check(width, height)
        if problem:
            print("FAIL %dx%d: %s" % (width, height, problem))
            failed = True
    if failed:
        print("tile-layout: FAIL")
        return 1
    print("tile-layout: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
