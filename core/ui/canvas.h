#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "ui/image.h"

namespace splat {

// Straight-alpha 8-bit colour. Overlays are uploaded as straight alpha and the
// GPU blends them, so the canvas keeps the same convention.
struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;

    friend bool operator==(Rgba lhs, Rgba rhs) {
        return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
    }
    friend bool operator!=(Rgba lhs, Rgba rhs) { return !(lhs == rhs); }
};

// The panel is drawn on the CPU into one RGBA8 canvas (1280x720 in use) and
// uploaded as a single overlay texture. Every draw clips to the canvas and
// blends source-over; a canvas starts fully transparent (all zero bytes).
//
// Coordinates are signed pixels, (0,0) is the top-left. Text is UTF-8 in the
// panel font (core/ui/font.h, Inter, anti-aliased); `size` is the pixel em
// size. The rounded shapes are anti-aliased by their pixel coverage.
class UiCanvas {
public:
    UiCanvas(uint32_t w, uint32_t h);

    void clear(Rgba color);

    // Filled rectangle, clipped and alpha blended. w or h <= 0 draws nothing.
    void fill_rect(int x, int y, int w, int h, Rgba color);

    // Rectangle outline drawn inside the box. thickness is clamped so the four
    // bands never overlap.
    void stroke_rect(int x, int y, int w, int h, int thickness, Rgba color);

    // Rounded rectangle (radius clamped to half the shorter side), filled or
    // as an outline `thickness` px wide inside the box.
    void fill_round_rect(int x, int y, int w, int h, int radius, Rgba color);
    void stroke_round_rect(int x, int y, int w, int h, int radius, int thickness, Rgba color);

    // A soft halo around a rounded rectangle: `color` at the edge fading to
    // nothing `spread` px outside it. The inside is left untouched.
    void glow_round_rect(int x, int y, int w, int h, int radius, int spread, Rgba color);

    // Circles and a round-capped line, centres in (sub)pixels.
    void fill_circle(float cx, float cy, float radius, Rgba color);
    void stroke_circle(float cx, float cy, float radius, float thickness, Rgba color);
    // The clockwise part of a ring from 12 o'clock covering `frac` of it.
    void stroke_arc(float cx, float cy, float radius, float thickness, float frac, Rgba color);
    void line(float x0, float y0, float x1, float y1, float thickness, Rgba color);

    // Draws one line of text with the top of its line box at (x, y) and
    // returns the advance width, even when clipping trims the glyphs.
    uint32_t text(int x, int y, std::string_view text, int size, Rgba color);

    // Advance width of a line, in pixels; exactly what text() returns.
    static uint32_t text_width(std::string_view text, int size);
    // Height of a line box at `size` and the y that centres the capitals of a
    // line in a box (box_y, box_h): what every label inside a plate uses.
    static int line_height(int size);
    static int text_top_centered(int box_y, int box_h, int size);

    // Scaled copy of `image` into the (x, y, dw, dh) box, clipped per
    // destination pixel. Nearest by default; bilinear samples the four
    // neighbouring texels for thumbnails and photographic previews.
    void blit(int x, int y, int dw, int dh, const UiImage& image, bool bilinear = false);

    // `image` scaled to cover the box (centre crop, bilinear) with rounded
    // corners: the tile thumbnails.
    void blit_cover(int x, int y, int w, int h, const UiImage& image, int radius);

    // Track rectangle plus a fill of `frac` (clamped to [0, 1]) of the width,
    // rounded to the nearest pixel.
    void bar(int x, int y, int w, int h, float frac, Rgba track, Rgba fill);

    const std::vector<uint8_t>& pixels() const { return pixels_; }
    std::vector<uint8_t>& pixels() { return pixels_; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Any draw call (clear included) marks the canvas dirty, even one that is
    // fully clipped and changes no pixel; the host uploads while dirty.
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // Encodes the canvas with core/jobs/image_io's encode_png and writes it.
    Status write_png(const Path& path) const;

private:
    uint8_t* pixel_at(int x, int y);
    void fill_impl(int x, int y, int w, int h, Rgba color);
    void blend_impl(int x, int y, Rgba color);
    // Blends `color` with its alpha scaled by `coverage` (0..1).
    void cover_impl(int x, int y, Rgba color, float coverage);

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> pixels_;
    bool dirty_ = false;
};

}  // namespace splat
