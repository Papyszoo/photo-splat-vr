#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace splat {

// An 8-bit single-channel mask, row-major with the top row first. 255 is a
// fully covered pixel (the glyph), 0 is transparent. Used as the R8 texture of
// an overlay quad.
struct TextImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // width * height

    bool empty() const { return width == 0 || height == 0 || pixels.empty(); }
};

// Rasterises lines of text from font8x8 (third_party/font8x8, public domain,
// basic Latin) at `scale` device pixels per font pixel. Glyph cells are
// 8x8; lines advance by 8 * scale plus a 2 * scale gap. U+00B7 (the middle
// dot separator) is drawn as a centred 2x2-font-pixel dot; other non-ASCII
// code points and control characters become '?'. The image is exactly the
// text block: columns = longest line, no padding (the label plate adds its
// own). `scale` below 1 is treated as 1.
TextImage rasterise_text(const std::vector<std::string>& lines, uint32_t scale);

}  // namespace splat
