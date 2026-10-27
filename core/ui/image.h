#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace splat {

// RGBA8 image handed to UiCanvas::blit, row-major, top row first.
//
// `splat::Image` (core/common/image.h) is the decoder's tightly packed RGB8
// type and is already used across components, so the panel's four-channel type
// has its own name instead of overloading that one. `splat::ui::Image` is an
// alias for call sites that only deal with UI types.
struct UiImage {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba;  // w * h * 4 bytes, straight (not premultiplied)

    bool empty() const { return w == 0 || h == 0 || rgba.empty(); }
    size_t pixel_count() const { return static_cast<size_t>(w) * static_cast<size_t>(h); }

    void resize(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        rgba.assign(pixel_count() * 4, 0);
    }

    uint8_t* pixel(uint32_t x, uint32_t y) {
        return rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    }
    const uint8_t* pixel(uint32_t x, uint32_t y) const {
        return rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    }
};

namespace ui {
using Image = UiImage;
}  // namespace ui

}  // namespace splat
