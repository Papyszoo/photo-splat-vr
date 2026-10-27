// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/button_glyph.h"

#include <algorithm>
#include <string>

namespace splat {
namespace {

enum class Shape { Cross, Circle, Square, Triangle, Options, Pill };

Shape shape_of(std::string_view button) {
    if (button == "Cross") return Shape::Cross;
    if (button == "Circle") return Shape::Circle;
    if (button == "Square") return Shape::Square;
    if (button == "Triangle") return Shape::Triangle;
    if (button == "Options") return Shape::Options;
    return Shape::Pill;
}

// L1/R1 letters keep a 16 px cap height (22 px Inter) for the headset.
int pill_text_size(int height) { return std::max(22, height * 11 / 20); }

int pill_width(std::string_view label, int height) {
    return static_cast<int>(UiCanvas::text_width(label, pill_text_size(height))) + height / 2 + 8;
}

int draw_pill(UiCanvas& canvas, int x, int cy, std::string_view label, int height, Rgba plate,
              Rgba ink) {
    const int width = pill_width(label, height);
    const int top = cy - height / 2;
    canvas.fill_round_rect(x, top, width, height, height / 3, plate);
    const int size = pill_text_size(height);
    const int text_w = static_cast<int>(UiCanvas::text_width(label, size));
    canvas.text(x + (width - text_w) / 2, UiCanvas::text_top_centered(top, height, size), label,
                size, ink);
    return width;
}

}  // namespace

int button_glyph_width(std::string_view button, int height) {
    switch (shape_of(button)) {
        case Shape::Cross:
        case Shape::Circle:
        case Shape::Square:
        case Shape::Triangle: return height;
        case Shape::Options: return height * 3 / 2;
        case Shape::Pill: break;
    }
    if (button == "L1/R1") return pill_width("L1", height) + 4 + pill_width("R1", height);
    return pill_width(button, height);
}

int draw_button_glyph(UiCanvas& canvas, int x, int cy, std::string_view button, int height,
                      Rgba plate, Rgba ink) {
    const float r = static_cast<float>(height) * 0.5f;
    const float cx = static_cast<float>(x) + r;
    const float fy = static_cast<float>(cy);
    const float stroke = std::max(1.6f, r * 0.17f);
    switch (shape_of(button)) {
        case Shape::Cross: {
            canvas.fill_circle(cx, fy, r, plate);
            const float k = r * 0.38f;
            canvas.line(cx - k, fy - k, cx + k, fy + k, stroke, ink);
            canvas.line(cx - k, fy + k, cx + k, fy - k, stroke, ink);
            return height;
        }
        case Shape::Circle:
            canvas.fill_circle(cx, fy, r, plate);
            canvas.stroke_circle(cx, fy, r * 0.44f, stroke, ink);
            return height;
        case Shape::Square: {
            canvas.fill_circle(cx, fy, r, plate);
            const int half = static_cast<int>(r * 0.42f + 0.5f);
            const int t = static_cast<int>(stroke + 0.5f);
            canvas.stroke_round_rect(static_cast<int>(cx) - half, cy - half, 2 * half, 2 * half, 2,
                                     t, ink);
            return height;
        }
        case Shape::Triangle: {
            canvas.fill_circle(cx, fy, r, plate);
            const float top = fy - r * 0.46f;
            const float bottom = fy + r * 0.32f;
            const float half = r * 0.46f;
            canvas.line(cx, top, cx - half, bottom, stroke, ink);
            canvas.line(cx - half, bottom, cx + half, bottom, stroke, ink);
            canvas.line(cx + half, bottom, cx, top, stroke, ink);
            return height;
        }
        case Shape::Options: {
            const int width = height * 3 / 2;
            canvas.fill_round_rect(x, cy - height / 2, width, height, height / 2, plate);
            const float left = static_cast<float>(x) + static_cast<float>(width) * 0.32f;
            const float right = static_cast<float>(x) + static_cast<float>(width) * 0.68f;
            const float gap = r * 0.36f;
            for (int line = -1; line <= 1; ++line) {
                const float y = fy + static_cast<float>(line) * gap;
                canvas.line(left, y, right, y, stroke * 0.85f, ink);
            }
            return width;
        }
        case Shape::Pill: break;
    }
    if (button == "L1/R1") {
        const int left = draw_pill(canvas, x, cy, "L1", height, plate, ink);
        return left + 4 + draw_pill(canvas, x + left + 4, cy, "R1", height, plate, ink);
    }
    return draw_pill(canvas, x, cy, button, height, plate, ink);
}

}  // namespace splat
