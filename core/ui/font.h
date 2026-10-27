// The panel font: Inter Medium (SIL OFL 1.1, third_party/inter), compiled into
// the binary (cmake/embed_file.cmake) and rasterised by stb_truetype, so the
// console needs no font file and no system font.
//
// Sizes are pixel em sizes (CSS "font-size"). A glyph is rasterised the first
// time a (size, subpixel phase, code point) is drawn and kept for the life of
// the process; the panel uses four sizes, so the cache stays at a few hundred
// small coverage bitmaps (well under a megabyte). Text is UTF-8; a code point
// the font lacks is drawn as '?'.
//
// The cache is guarded by a mutex: the panel draws on one thread, but a test or
// a thumbnail worker must never race it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace splat {

// One rasterised glyph: 8-bit coverage and its offset from the pen position
// on the baseline (y0 is negative above the baseline).
struct UiGlyph {
    int x0 = 0;
    int y0 = 0;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> coverage;  // w * h, row-major
};

namespace ui_font {

// Decodes the code point at `index` and advances past it. A malformed byte
// yields U+FFFD and advances by one, so a bad name never stalls a loop.
uint32_t next_codepoint(std::string_view text, std::size_t& index);

// Metrics at `size` px, rounded to whole pixels.
int ascent(int size);       // baseline below the top of the line box
int line_height(int size);  // ascent + descent
int cap_height(int size);   // height of 'H' above the baseline

// Horizontal subpixel phases a glyph is rasterised at (quarter pixels).
constexpr int kSubpixel = 4;

// The glyph for `codepoint` at `size` px, rasterised at `phase` / kSubpixel of
// a pixel to the right. The reference stays valid for the process.
const UiGlyph& glyph(int size, int phase, uint32_t codepoint);

// Advance of `codepoint` plus the kerning towards `next` (0 = none), in
// pixels.
float advance(int size, uint32_t codepoint, uint32_t next);

// Width of one line, rounded the same way UiCanvas::text() places glyphs.
int text_width(std::string_view text, int size);

}  // namespace ui_font
}  // namespace splat
