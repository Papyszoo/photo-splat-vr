#pragma once

namespace splat {

// A rectangle in canvas pixels, top-left origin, used for screen content and
// widget areas. `w`/`h` are extents, so the covered pixels are [x, x + w) and
// [y, y + h).
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }

    bool contains(float px, float py) const {
        return px >= static_cast<float>(x) && px < static_cast<float>(right()) &&
               py >= static_cast<float>(y) && py < static_cast<float>(bottom());
    }
};

}  // namespace splat
