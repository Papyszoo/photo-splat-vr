# font8x8

An 8x8 monochrome bitmap font (basic Latin, U+0000 to U+007F) used by the
scene label overlay (`core/render/text_raster.cpp`).

- File vendored: `font8x8_basic.h`, unmodified.
- Author: Daniel Hepper <daniel@hepper.net>, based on Marcel Sondaar's and
  IBM's public-domain 8x8 VGA fonts.
- License: public domain (see the header comment in
  `font8x8_basic.h`).
- Upstream: <https://github.com/dhepper/font8x8>
- Retrieved: 2026-10-26 from
  `raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h`.
- Glyph rows are little-endian bytes: bit `x` of row `y` is the pixel at
  column `x`, top row first.
