#include "render/text_raster.h"

#include <algorithm>
#include <cstddef>

// font8x8_basic.h is the verbatim upstream public-domain header (C). Its
// 0xFF glyph bytes narrow to char, which clang rejects in C++11 mode; the
// bytes are read back as unsigned char below, so the values are preserved.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++11-narrowing"
#endif
#include "font8x8/font8x8_basic.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace splat {
namespace {

constexpr uint32_t kGlyphPixels = 8;
constexpr uint32_t kLineGapPixels = 2;
constexpr uint32_t kMiddleDot = 0x00B7;
constexpr uint32_t kUnknown = 0xFFFFFFFFu;

// Decodes one UTF-8 code point at `index` and advances past it. Invalid bytes
// are consumed one at a time and reported as kUnknown so the caller can draw
// '?'.
uint32_t decode_utf8(const std::string& text, size_t& index) {
    const unsigned char first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        ++index;
        return first;
    }
    uint32_t extra = 0;
    uint32_t code = 0;
    if ((first & 0xE0u) == 0xC0u) {
        extra = 1;
        code = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
        extra = 2;
        code = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
        extra = 3;
        code = first & 0x07u;
    } else {
        ++index;
        return kUnknown;
    }
    if (index + extra >= text.size()) {
        ++index;
        return kUnknown;
    }
    for (uint32_t i = 1; i <= extra; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[index + i]);
        if ((next & 0xC0u) != 0x80u) {
            ++index;
            return kUnknown;
        }
        code = (code << 6) | (next & 0x3Fu);
    }
    index += extra + 1;
    if (code > 0x10FFFFu) return kUnknown;
    return code;
}

void fill_rect(TextImage& image, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height) {
    for (uint32_t y = y0; y < y0 + height && y < image.height; ++y) {
        uint8_t* row = image.pixels.data() + static_cast<size_t>(y) * image.width;
        for (uint32_t x = x0; x < x0 + width && x < image.width; ++x) row[x] = 255;
    }
}

// Draws one 8x8 glyph cell, scaled by `scale`, at the cell's top-left pixel.
void draw_glyph(TextImage& image, uint32_t x0, uint32_t y0, uint32_t code, uint32_t scale) {
    if (code == kMiddleDot) {
        // The basic-Latin font has no separator dot: a centred 2x2-font-pixel
        // square, the same weight as a glyph stem.
        fill_rect(image, x0 + 3 * scale, y0 + 3 * scale, 2 * scale, 2 * scale);
        return;
    }
    // Only printable ASCII has a glyph; control characters and anything else
    // become '?'. Space (0x20) is a blank glyph, so words still separate.
    if (code < 0x20 || code > 0x7E) code = '?';
    for (uint32_t row = 0; row < kGlyphPixels; ++row) {
        const unsigned char bits = static_cast<unsigned char>(font8x8_basic[code][row]);
        for (uint32_t column = 0; column < kGlyphPixels; ++column) {
            if ((bits >> column) & 1u) {
                fill_rect(image, x0 + column * scale, y0 + row * scale, scale, scale);
            }
        }
    }
}

}  // namespace

TextImage rasterise_text(const std::vector<std::string>& lines, uint32_t scale) {
    if (scale == 0) scale = 1;

    std::vector<std::vector<uint32_t>> glyphs;
    glyphs.reserve(lines.size());
    size_t columns = 0;
    for (const std::string& line : lines) {
        std::vector<uint32_t> codes;
        size_t index = 0;
        while (index < line.size()) {
            uint32_t code = decode_utf8(line, index);
            if (code == kUnknown) code = '?';
            codes.push_back(code);
        }
        columns = std::max(columns, codes.size());
        glyphs.push_back(std::move(codes));
    }
    if (glyphs.empty() || columns == 0) return TextImage{};

    const uint32_t cell = kGlyphPixels * scale;
    const uint32_t line_height = kGlyphPixels * scale;
    const uint32_t gap = kLineGapPixels * scale;
    TextImage image;
    image.width = static_cast<uint32_t>(columns) * cell;
    image.height = static_cast<uint32_t>(glyphs.size()) * line_height +
                   static_cast<uint32_t>(glyphs.size() - 1) * gap;
    image.pixels.assign(static_cast<size_t>(image.width) * image.height, 0);

    for (size_t line = 0; line < glyphs.size(); ++line) {
        const uint32_t y0 = static_cast<uint32_t>(line) * (line_height + gap);
        for (size_t column = 0; column < glyphs[line].size(); ++column) {
            draw_glyph(image, static_cast<uint32_t>(column) * cell, y0, glyphs[line][column],
                       scale);
        }
    }
    return image;
}

}  // namespace splat
