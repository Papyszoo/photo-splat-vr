// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/font.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

// stb_truetype is vendored unmodified (third_party/stb, public domain/MIT);
// its implementation lives in this one translation unit, internal linkage.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace splat {

// The embedded TTF (generated from third_party/inter/Inter-Medium.ttf).
extern const unsigned char kUiFontData[];
extern const std::size_t kUiFontData_size;

namespace ui_font {
namespace {

constexpr int kMinSize = 6;
constexpr int kMaxSize = 160;

struct Font {
    stbtt_fontinfo info{};
    bool ok = false;
    int ascent_units = 0;
    int descent_units = 0;  // negative
    int cap_units = 0;
    int fallback_glyph = 0;  // '?'

    std::mutex mutex;
    std::unordered_map<uint64_t, UiGlyph> glyphs;
    std::unordered_map<uint64_t, int> kerning;  // font units, by glyph pair
    UiGlyph empty;

    Font() {
        const int offset = stbtt_GetFontOffsetForIndex(kUiFontData, 0);
        if (offset < 0 || stbtt_InitFont(&info, kUiFontData, offset) == 0) return;
        int line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent_units, &descent_units, &line_gap);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) != 0) cap_units = y1;
        fallback_glyph = stbtt_FindGlyphIndex(&info, '?');
        ok = true;
    }

    float scale(int size) const {
        return stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(size));
    }

    int glyph_index(uint32_t codepoint) const {
        const int index = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
        return index != 0 ? index : fallback_glyph;
    }
};

Font& font() {
    static Font instance;
    return instance;
}

int clamp_size(int size) { return size < kMinSize ? kMinSize : (size > kMaxSize ? kMaxSize : size); }

}  // namespace

uint32_t next_codepoint(std::string_view text, std::size_t& index) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(index);
    int extra = 0;
    uint32_t code = 0;
    if (lead < 0x80) {
        ++index;
        return lead;
    } else if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        code = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        code = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        code = lead & 0x07;
    } else {
        ++index;
        return 0xFFFD;
    }
    if (index + static_cast<std::size_t>(extra) >= text.size()) {
        ++index;
        return 0xFFFD;
    }
    for (int step = 1; step <= extra; ++step) {
        const unsigned char next = byte(index + static_cast<std::size_t>(step));
        if ((next & 0xC0) != 0x80) {
            ++index;
            return 0xFFFD;
        }
        code = (code << 6) | (next & 0x3F);
    }
    index += static_cast<std::size_t>(extra) + 1;
    return code;
}

int ascent(int size) {
    const Font& f = font();
    if (!f.ok) return size;
    return static_cast<int>(std::lround(f.ascent_units * f.scale(clamp_size(size))));
}

int line_height(int size) {
    const Font& f = font();
    if (!f.ok) return size;
    return static_cast<int>(
        std::lround((f.ascent_units - f.descent_units) * f.scale(clamp_size(size))));
}

int cap_height(int size) {
    const Font& f = font();
    if (!f.ok) return size * 3 / 4;
    return static_cast<int>(std::lround(f.cap_units * f.scale(clamp_size(size))));
}

const UiGlyph& glyph(int size, int phase, uint32_t codepoint) {
    Font& f = font();
    if (!f.ok) return f.empty;
    size = clamp_size(size);
    phase = phase < 0 ? 0 : (phase >= kSubpixel ? kSubpixel - 1 : phase);
    const uint64_t key = (static_cast<uint64_t>(size) << 40) |
                         (static_cast<uint64_t>(phase) << 32) | codepoint;

    std::lock_guard<std::mutex> lock(f.mutex);
    const auto found = f.glyphs.find(key);
    if (found != f.glyphs.end()) return found->second;

    UiGlyph out;
    const int index = f.glyph_index(codepoint);
    const float scale = f.scale(size);
    const float shift = static_cast<float>(phase) / static_cast<float>(kSubpixel);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&f.info, index, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
    if (x1 > x0 && y1 > y0) {
        out.x0 = x0;
        out.y0 = y0;
        out.w = x1 - x0;
        out.h = y1 - y0;
        out.coverage.assign(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h), 0);
        stbtt_MakeGlyphBitmapSubpixel(&f.info, out.coverage.data(), out.w, out.h, out.w, scale,
                                      scale, shift, 0.0f, index);
    }
    return f.glyphs.emplace(key, std::move(out)).first->second;
}

float advance(int size, uint32_t codepoint, uint32_t next) {
    Font& f = font();
    if (!f.ok) return static_cast<float>(size) * 0.5f;
    const int index = f.glyph_index(codepoint);
    int advance_units = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&f.info, index, &advance_units, &bearing);
    int kern = 0;
    if (next != 0) {
        const int next_index = f.glyph_index(next);
        const uint64_t key = (static_cast<uint64_t>(index) << 32) | static_cast<uint32_t>(next_index);
        std::lock_guard<std::mutex> lock(f.mutex);
        const auto found = f.kerning.find(key);
        if (found != f.kerning.end()) {
            kern = found->second;
        } else {
            kern = stbtt_GetGlyphKernAdvance(&f.info, index, next_index);
            f.kerning.emplace(key, kern);
        }
    }
    return static_cast<float>(advance_units + kern) * f.scale(clamp_size(size));
}

int text_width(std::string_view text, int size) {
    float pen = 0.0f;
    std::size_t index = 0;
    bool have = index < text.size();
    uint32_t code = have ? next_codepoint(text, index) : 0;
    while (have) {
        const bool have_next = index < text.size();
        const uint32_t next = have_next ? next_codepoint(text, index) : 0;
        pen += advance(size, code, next);
        code = next;
        have = have_next;
    }
    return static_cast<int>(std::lround(pen));
}

}  // namespace ui_font
}  // namespace splat
