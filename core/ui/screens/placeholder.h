// The tile the browse screens draw for a cover or a scene without a thumbnail.
// Console scenes have no thumb.jpg today, so a missing thumbnail is normal and
// must never look like an error: every such tile is the same calm picture
// placeholder (a sun over a hill), not initials or a random colour.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

#include "ui/canvas.h"
#include "ui/rect.h"

namespace splat {

// One placeholder in `box`, corners rounded by `radius`.
inline void draw_placeholder(UiCanvas& canvas, const Rect& box, int radius) {
    if (box.empty()) return;
    canvas.fill_round_rect(box.x, box.y, box.w, box.h, radius, Rgba{38, 52, 92, 255});
    const Rgba ink{255, 255, 255, 70};
    const float unit = static_cast<float>(std::min(box.w, box.h));
    const float cx = static_cast<float>(box.x) + static_cast<float>(box.w) * 0.5f;
    const float cy = static_cast<float>(box.y) + static_cast<float>(box.h) * 0.5f;
    // A framed picture: the frame, a sun and a hill line.
    const int fw = static_cast<int>(unit * 0.52f);
    const int fh = static_cast<int>(unit * 0.38f);
    const int fx = static_cast<int>(cx) - fw / 2;
    const int fy = static_cast<int>(cy) - fh / 2;
    const float stroke = std::max(2.0f, unit * 0.022f);
    canvas.stroke_round_rect(fx, fy, fw, fh, static_cast<int>(unit * 0.05f),
                             static_cast<int>(stroke + 0.5f), ink);
    canvas.fill_circle(static_cast<float>(fx) + static_cast<float>(fw) * 0.72f,
                       static_cast<float>(fy) + static_cast<float>(fh) * 0.32f, unit * 0.045f, ink);
    const float base = static_cast<float>(fy + fh) - stroke * 2.0f;
    const float left = static_cast<float>(fx) + stroke * 2.0f;
    const float right = static_cast<float>(fx + fw) - stroke * 2.0f;
    canvas.line(left, base, left + static_cast<float>(fw) * 0.3f, base - static_cast<float>(fh) * 0.38f,
                stroke, ink);
    canvas.line(left + static_cast<float>(fw) * 0.3f, base - static_cast<float>(fh) * 0.38f,
                left + static_cast<float>(fw) * 0.52f, base - static_cast<float>(fh) * 0.12f, stroke,
                ink);
    canvas.line(left + static_cast<float>(fw) * 0.52f, base - static_cast<float>(fh) * 0.12f,
                right, base - static_cast<float>(fh) * 0.3f, stroke, ink);
}

}  // namespace splat
