#include "ui/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>

#include "jobs/image_io.h"
#include "ui/font.h"

namespace splat {
namespace {


// Source-over with straight alpha. The RGB mix is divided by the result alpha,
// so drawing a translucent colour onto an empty pixel keeps the colour and only
// records the alpha: the GPU multiplies by alpha again when it blends the
// overlay, and two draws composite the same way one does. All arithmetic stays
// in integers so a test can predict every byte.
void blend(uint8_t* dst, Rgba src) {
    if (src.a == 0) return;
    if (src.a == 255) {
        dst[0] = src.r;
        dst[1] = src.g;
        dst[2] = src.b;
        dst[3] = 255;
        return;
    }
    const uint32_t sa = src.a;
    const uint32_t ia = 255u - sa;
    const uint32_t da = dst[3];
    const uint32_t out_alpha = sa + (da * ia + 127u) / 255u;
    if (out_alpha == 0) return;
    const uint32_t denominator = out_alpha * 255u;
    dst[0] = static_cast<uint8_t>(
        (static_cast<uint32_t>(src.r) * sa * 255u + dst[0] * da * ia + out_alpha * 127u) /
        denominator);
    dst[1] = static_cast<uint8_t>(
        (static_cast<uint32_t>(src.g) * sa * 255u + dst[1] * da * ia + out_alpha * 127u) /
        denominator);
    dst[2] = static_cast<uint8_t>(
        (static_cast<uint32_t>(src.b) * sa * 255u + dst[2] * da * ia + out_alpha * 127u) /
        denominator);
    dst[3] = static_cast<uint8_t>(std::min(255u, out_alpha));
}

// Clips a box to the canvas and returns false when nothing is left.
bool clip_box(int& x, int& y, int& w, int& h, int canvas_w, int canvas_h) {
    if (w <= 0 || h <= 0) return false;
    const long x0 = std::max<long>(0, x);
    const long y0 = std::max<long>(0, y);
    const long x1 = std::min<long>(canvas_w, static_cast<long>(x) + w);
    const long y1 = std::min<long>(canvas_h, static_cast<long>(y) + h);
    if (x0 >= x1 || y0 >= y1) return false;
    x = static_cast<int>(x0);
    y = static_cast<int>(y0);
    w = static_cast<int>(x1 - x0);
    h = static_cast<int>(y1 - y0);
    return true;
}

// Bilinear sample of one channel at (u, v) in source texel coordinates.
uint8_t sample_bilinear(const UiImage& image, float u, float v, int channel) {
    const float max_x = static_cast<float>(image.w - 1);
    const float max_y = static_cast<float>(image.h - 1);
    u = std::clamp(u, 0.0f, max_x);
    v = std::clamp(v, 0.0f, max_y);
    const int x0 = static_cast<int>(u);
    const int y0 = static_cast<int>(v);
    const int x1 = std::min(x0 + 1, static_cast<int>(image.w) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(image.h) - 1);
    const float fx = u - static_cast<float>(x0);
    const float fy = v - static_cast<float>(y0);
    const auto at = [&](int x, int y) {
        return static_cast<float>(image.pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                                      [channel]);
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return static_cast<uint8_t>(std::lround(top + (bottom - top) * fy));
}


float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

// Signed distance from (px, py) to a rounded box, negative inside.
float round_box_distance(float px, float py, float x, float y, float w, float h, float r) {
    const float half_w = w * 0.5f;
    const float half_h = h * 0.5f;
    const float qx = std::fabs(px - (x + half_w)) - (half_w - r);
    const float qy = std::fabs(py - (y + half_h)) - (half_h - r);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

int clamp_radius(int radius, int w, int h) {
    return std::max(0, std::min(radius, std::min(w, h) / 2));
}

}  // namespace

UiCanvas::UiCanvas(uint32_t w, uint32_t h)
    : width_(w),
      height_(h),
      pixels_(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0) {}

uint8_t* UiCanvas::pixel_at(int x, int y) {
    return pixels_.data() + (static_cast<size_t>(y) * width_ + static_cast<size_t>(x)) * 4;
}

void UiCanvas::blend_impl(int x, int y, Rgba color) { blend(pixel_at(x, y), color); }

void UiCanvas::cover_impl(int x, int y, Rgba color, float coverage) {
    if (coverage <= 0.0f) return;
    if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) return;
    if (coverage < 1.0f) {
        color.a = static_cast<uint8_t>(std::lround(static_cast<float>(color.a) * coverage));
    }
    blend(pixel_at(x, y), color);
}

void UiCanvas::fill_impl(int x, int y, int w, int h, Rgba color) {
    if (!clip_box(x, y, w, h, static_cast<int>(width_), static_cast<int>(height_))) return;
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) blend_impl(col, row, color);
    }
}

void UiCanvas::clear(Rgba color) {
    dirty_ = true;
    if (color.a == 255) {
        for (size_t i = 0; i < pixels_.size(); i += 4) {
            pixels_[i] = color.r;
            pixels_[i + 1] = color.g;
            pixels_[i + 2] = color.b;
            pixels_[i + 3] = color.a;
        }
        return;
    }
    std::fill(pixels_.begin(), pixels_.end(), 0);
    if (color.a != 0) {
        for (uint32_t y = 0; y < height_; ++y) {
            for (uint32_t x = 0; x < width_; ++x) blend_impl(static_cast<int>(x),
                                                             static_cast<int>(y), color);
        }
    }
}

void UiCanvas::fill_rect(int x, int y, int w, int h, Rgba color) {
    dirty_ = true;
    fill_impl(x, y, w, h, color);
}

void UiCanvas::stroke_rect(int x, int y, int w, int h, int thickness, Rgba color) {
    dirty_ = true;
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    const int t = std::min(thickness, (std::min(w, h) + 1) / 2);
    // Two bands plus a gap do not fit: the stroke is the whole box. Drawing
    // the bands anyway would blend the shared pixels twice (visible whenever
    // the colour is translucent), so fill once instead.
    if (2 * t >= w || 2 * t >= h) {
        fill_impl(x, y, w, h, color);
        return;
    }
    // Top and bottom span the full width; the side bands only cover the rows
    // between them, so no pixel is touched by two bands.
    fill_impl(x, y, w, t, color);
    fill_impl(x, y + h - t, w, t, color);
    fill_impl(x, y + t, t, h - 2 * t, color);
    fill_impl(x + w - t, y + t, t, h - 2 * t, color);
}

uint32_t UiCanvas::text_width(std::string_view str, int size) {
    return static_cast<uint32_t>(ui_font::text_width(str, size));
}

int UiCanvas::line_height(int size) { return ui_font::line_height(size); }

int UiCanvas::text_top_centered(int box_y, int box_h, int size) {
    const int baseline = box_y + (box_h + ui_font::cap_height(size) + 1) / 2;
    return baseline - ui_font::ascent(size);
}

uint32_t UiCanvas::text(int x, int y, std::string_view str, int size, Rgba color) {
    dirty_ = true;
    const int baseline = y + ui_font::ascent(size);
    float pen = 0.0f;
    std::size_t index = 0;
    bool have = index < str.size();
    uint32_t code = have ? ui_font::next_codepoint(str, index) : 0;
    while (have) {
        const bool have_next = index < str.size();
        const uint32_t next = have_next ? ui_font::next_codepoint(str, index) : 0;
        const float origin = static_cast<float>(x) + pen;
        const float whole = std::floor(origin);
        const int phase = static_cast<int>((origin - whole) * ui_font::kSubpixel);
        const UiGlyph& glyph = ui_font::glyph(size, phase, code);
        const int left = static_cast<int>(whole) + glyph.x0;
        const int top = baseline + glyph.y0;
        for (int row = 0; row < glyph.h; ++row) {
            const int py = top + row;
            if (py < 0 || py >= static_cast<int>(height_)) continue;
            const uint8_t* line = glyph.coverage.data() + static_cast<std::size_t>(row) * glyph.w;
            for (int column = 0; column < glyph.w; ++column) {
                const uint8_t coverage = line[column];
                if (coverage == 0) continue;
                cover_impl(left + column, py, color, static_cast<float>(coverage) / 255.0f);
            }
        }
        pen += ui_font::advance(size, code, next);
        code = next;
        have = have_next;
    }
    return static_cast<uint32_t>(std::lround(pen));
}

void UiCanvas::fill_round_rect(int x, int y, int w, int h, int radius, Rgba color) {
    dirty_ = true;
    if (w <= 0 || h <= 0) return;
    const int r = clamp_radius(radius, w, h);
    const float fr = static_cast<float>(r);
    for (int row = y; row < y + h; ++row) {
        if (row < 0 || row >= static_cast<int>(height_)) continue;
        if (row >= y + r && row < y + h - r) {
            fill_impl(x, row, w, 1, color);
            continue;
        }
        for (int col = x; col < x + w; ++col) {
            if (col >= x + r && col < x + w - r) {
                cover_impl(col, row, color, 1.0f);
                continue;
            }
            const float d = round_box_distance(static_cast<float>(col) + 0.5f,
                                               static_cast<float>(row) + 0.5f, static_cast<float>(x),
                                               static_cast<float>(y), static_cast<float>(w),
                                               static_cast<float>(h), fr);
            cover_impl(col, row, color, clamp01(0.5f - d));
        }
    }
}

void UiCanvas::stroke_round_rect(int x, int y, int w, int h, int radius, int thickness,
                                 Rgba color) {
    dirty_ = true;
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    const int r = clamp_radius(radius, w, h);
    const int t = std::min(thickness, std::min(w, h) / 2);
    const float inner_r = static_cast<float>(std::max(0, r - t));
    for (int row = y; row < y + h; ++row) {
        if (row < 0 || row >= static_cast<int>(height_)) continue;
        const bool straight = row >= y + r + t && row < y + h - r - t;
        for (int col = x; col < x + w; ++col) {
            if (straight && col >= x + t && col < x + w - t) {
                col = x + w - t - 1;  // skip the untouched inside
                continue;
            }
            const float px = static_cast<float>(col) + 0.5f;
            const float py = static_cast<float>(row) + 0.5f;
            const float outer = clamp01(
                0.5f - round_box_distance(px, py, static_cast<float>(x), static_cast<float>(y),
                                          static_cast<float>(w), static_cast<float>(h),
                                          static_cast<float>(r)));
            const float inner = clamp01(0.5f - round_box_distance(
                                                   px, py, static_cast<float>(x + t),
                                                   static_cast<float>(y + t),
                                                   static_cast<float>(w - 2 * t),
                                                   static_cast<float>(h - 2 * t), inner_r));
            cover_impl(col, row, color, outer - inner);
        }
    }
}

void UiCanvas::glow_round_rect(int x, int y, int w, int h, int radius, int spread, Rgba color) {
    dirty_ = true;
    if (w <= 0 || h <= 0 || spread <= 0) return;
    const int r = clamp_radius(radius, w, h);
    const float fs = static_cast<float>(spread);
    for (int row = y - spread; row < y + h + spread; ++row) {
        if (row < 0 || row >= static_cast<int>(height_)) continue;
        const bool beside = row >= y + r && row < y + h - r;
        for (int col = x - spread; col < x + w + spread; ++col) {
            if (beside && col >= x && col < x + w) {
                col = x + w - 1;  // inside the box: left as it is
                continue;
            }
            const float d = round_box_distance(static_cast<float>(col) + 0.5f,
                                               static_cast<float>(row) + 0.5f, static_cast<float>(x),
                                               static_cast<float>(y), static_cast<float>(w),
                                               static_cast<float>(h), static_cast<float>(r));
            if (d <= 0.0f || d >= fs) continue;
            const float fall = 1.0f - d / fs;
            cover_impl(col, row, color, fall * fall);
        }
    }
}

void UiCanvas::fill_circle(float cx, float cy, float radius, Rgba color) {
    dirty_ = true;
    if (radius <= 0.0f) return;
    const int x0 = static_cast<int>(std::floor(cx - radius - 1.0f));
    const int x1 = static_cast<int>(std::ceil(cx + radius + 1.0f));
    const int y0 = static_cast<int>(std::floor(cy - radius - 1.0f));
    const int y1 = static_cast<int>(std::ceil(cy + radius + 1.0f));
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            const float dx = static_cast<float>(col) + 0.5f - cx;
            const float dy = static_cast<float>(row) + 0.5f - cy;
            cover_impl(col, row, color, clamp01(radius - std::sqrt(dx * dx + dy * dy) + 0.5f));
        }
    }
}

void UiCanvas::stroke_circle(float cx, float cy, float radius, float thickness, Rgba color) {
    stroke_arc(cx, cy, radius, thickness, 1.0f, color);
}

void UiCanvas::stroke_arc(float cx, float cy, float radius, float thickness, float frac,
                          Rgba color) {
    dirty_ = true;
    if (radius <= 0.0f || thickness <= 0.0f || !(frac > 0.0f)) return;
    frac = std::min(frac, 1.0f);
    const float reach = radius + thickness;
    const float end = frac * 6.28318530718f;
    const int x0 = static_cast<int>(std::floor(cx - reach - 1.0f));
    const int x1 = static_cast<int>(std::ceil(cx + reach + 1.0f));
    const int y0 = static_cast<int>(std::floor(cy - reach - 1.0f));
    const int y1 = static_cast<int>(std::ceil(cy + reach + 1.0f));
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            const float dx = static_cast<float>(col) + 0.5f - cx;
            const float dy = static_cast<float>(row) + 0.5f - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            float coverage = clamp01(thickness * 0.5f - std::fabs(dist - radius) + 0.5f);
            if (coverage <= 0.0f) continue;
            if (frac < 1.0f) {
                // Clockwise angle from 12 o'clock (y grows downwards).
                float angle = std::atan2(dx, -dy);
                if (angle < 0.0f) angle += 6.28318530718f;
                coverage *= clamp01((end - angle) * dist + 0.5f);
            }
            cover_impl(col, row, color, coverage);
        }
    }
}

void UiCanvas::line(float x0, float y0, float x1, float y1, float thickness, Rgba color) {
    dirty_ = true;
    if (thickness <= 0.0f) return;
    const float half = thickness * 0.5f;
    const int left = static_cast<int>(std::floor(std::min(x0, x1) - half - 1.0f));
    const int right = static_cast<int>(std::ceil(std::max(x0, x1) + half + 1.0f));
    const int top = static_cast<int>(std::floor(std::min(y0, y1) - half - 1.0f));
    const int bottom = static_cast<int>(std::ceil(std::max(y0, y1) + half + 1.0f));
    const float vx = x1 - x0;
    const float vy = y1 - y0;
    const float length2 = vx * vx + vy * vy;
    for (int row = top; row < bottom; ++row) {
        for (int col = left; col < right; ++col) {
            const float px = static_cast<float>(col) + 0.5f - x0;
            const float py = static_cast<float>(row) + 0.5f - y0;
            const float t = length2 > 0.0f ? clamp01((px * vx + py * vy) / length2) : 0.0f;
            const float dx = px - vx * t;
            const float dy = py - vy * t;
            cover_impl(col, row, color, clamp01(half - std::sqrt(dx * dx + dy * dy) + 0.5f));
        }
    }
}

void UiCanvas::blit(int x, int y, int dw, int dh, const UiImage& image, bool bilinear) {
    dirty_ = true;
    if (dw <= 0 || dh <= 0 || image.empty()) return;
    int x0 = x;
    int y0 = y;
    int cw = dw;
    int ch = dh;
    if (!clip_box(x0, y0, cw, ch, static_cast<int>(width_), static_cast<int>(height_))) return;

    for (int row = y0; row < y0 + ch; ++row) {
        for (int col = x0; col < x0 + cw; ++col) {
            Rgba color{};
            if (bilinear) {
                const float u = (static_cast<float>(col - x) + 0.5f) *
                                    static_cast<float>(image.w) / static_cast<float>(dw) -
                                0.5f;
                const float v = (static_cast<float>(row - y) + 0.5f) *
                                    static_cast<float>(image.h) / static_cast<float>(dh) -
                                0.5f;
                color.r = sample_bilinear(image, u, v, 0);
                color.g = sample_bilinear(image, u, v, 1);
                color.b = sample_bilinear(image, u, v, 2);
                color.a = sample_bilinear(image, u, v, 3);
            } else {
                const uint32_t sx = std::min<uint32_t>(
                    image.w - 1,
                    static_cast<uint32_t>((static_cast<int64_t>(col - x) * image.w) / dw));
                const uint32_t sy = std::min<uint32_t>(
                    image.h - 1,
                    static_cast<uint32_t>((static_cast<int64_t>(row - y) * image.h) / dh));
                const uint8_t* texel = image.pixel(sx, sy);
                color = Rgba{texel[0], texel[1], texel[2], texel[3]};
            }
            blend_impl(col, row, color);
        }
    }
}

void UiCanvas::blit_cover(int x, int y, int w, int h, const UiImage& image, int radius) {
    dirty_ = true;
    if (w <= 0 || h <= 0 || image.empty()) return;
    const float scale = std::max(static_cast<float>(w) / static_cast<float>(image.w),
                                 static_cast<float>(h) / static_cast<float>(image.h));
    const float origin_u = (static_cast<float>(image.w) - static_cast<float>(w) / scale) * 0.5f;
    const float origin_v = (static_cast<float>(image.h) - static_cast<float>(h) / scale) * 0.5f;
    const int r = clamp_radius(radius, w, h);
    for (int row = y; row < y + h; ++row) {
        if (row < 0 || row >= static_cast<int>(height_)) continue;
        const bool straight = row >= y + r && row < y + h - r;
        const float v = origin_v + (static_cast<float>(row - y) + 0.5f) / scale - 0.5f;
        for (int col = x; col < x + w; ++col) {
            if (col < 0 || col >= static_cast<int>(width_)) continue;
            float coverage = 1.0f;
            if (!straight && (col < x + r || col >= x + w - r)) {
                coverage = clamp01(0.5f - round_box_distance(
                                              static_cast<float>(col) + 0.5f,
                                              static_cast<float>(row) + 0.5f, static_cast<float>(x),
                                              static_cast<float>(y), static_cast<float>(w),
                                              static_cast<float>(h), static_cast<float>(r)));
                if (coverage <= 0.0f) continue;
            }
            const float u = origin_u + (static_cast<float>(col - x) + 0.5f) / scale - 0.5f;
            Rgba color{sample_bilinear(image, u, v, 0), sample_bilinear(image, u, v, 1),
                       sample_bilinear(image, u, v, 2), sample_bilinear(image, u, v, 3)};
            cover_impl(col, row, color, coverage);
        }
    }
}

void UiCanvas::bar(int x, int y, int w, int h, float frac, Rgba track, Rgba fill) {
    dirty_ = true;
    if (w <= 0 || h <= 0) return;
    fill_impl(x, y, w, h, track);
    const float clamped = std::clamp(std::isfinite(frac) ? frac : 0.0f, 0.0f, 1.0f);
    const int filled = static_cast<int>(std::lround(clamped * static_cast<float>(w)));
    if (filled > 0) fill_impl(x, y, filled, h, fill);
}

Status UiCanvas::write_png(const Path& path) const {
    const Result<std::vector<uint8_t>> png =
        encode_png(pixels_.data(), static_cast<int>(width_), static_cast<int>(height_));
    if (!png.ok()) return Status::error(png.message());

    std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file) return Status::error("cannot open " + path.string());
    const std::vector<uint8_t>& bytes = png.value();
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) return Status::error("failed while writing " + path.string());
    return Status::success();
}

}  // namespace splat
