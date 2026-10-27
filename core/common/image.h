#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace splat {

// Tightly packed RGB8 image, row-major, top row first. This is the only image
// format that crosses component boundaries; decoders (stb_image) fill it.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3 bytes

    bool empty() const { return width <= 0 || height <= 0 || rgb.empty(); }
    size_t pixel_count() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
    size_t byte_size() const { return pixel_count() * 3; }

    void resize(int w, int h) {
        width = w;
        height = h;
        rgb.assign(pixel_count() * 3, 0);
    }

    uint8_t* pixel(int x, int y) { return rgb.data() + (static_cast<size_t>(y) * width + x) * 3; }
    const uint8_t* pixel(int x, int y) const {
        return rgb.data() + (static_cast<size_t>(y) * width + x) * 3;
    }
};

}  // namespace splat
