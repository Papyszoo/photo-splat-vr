// SPDX-License-Identifier: GPL-3.0-or-later

#include "image/orientation.h"

#include <utility>

namespace splat {
namespace image {
namespace {

// x' = a*x + b*y + c, y' = d*x + e*y + f, with x/y pixel indices.
struct PixelMap {
    int a = 1, b = 0, c = 0;
    int d = 0, e = 1, f = 0;

    bool swaps_axes() const { return b != 0; }
};

// EXIF orientation 1..8 for a `w` x `h` image, using the source size in the
// offsets. The table is the one every EXIF reader agrees on (e.g. libexif's
// exif_orientation_transform); it is written once here and nowhere else.
PixelMap orientation_map(int w, int h, int orientation) {
    const int x_last = w - 1;
    const int y_last = h - 1;
    switch (orientation) {
        case 2: return PixelMap{-1, 0, x_last, 0, 1, 0};   // mirror horizontally
        case 3: return PixelMap{-1, 0, x_last, 0, -1, y_last};  // rotate 180
        case 4: return PixelMap{1, 0, 0, 0, -1, y_last};    // mirror vertically
        case 5: return PixelMap{0, 1, 0, 1, 0, 0};          // transpose
        case 6: return PixelMap{0, -1, y_last, 1, 0, 0};    // rotate 90 clockwise
        case 7: return PixelMap{0, -1, y_last, -1, 0, x_last};  // transverse
        case 8: return PixelMap{0, 1, 0, -1, 0, x_last};    // rotate 270 clockwise
        case 1:
        default: return PixelMap{};
    }
}

// Clockwise quarter turns.
PixelMap quarter_map(int w, int h, int quarters) {
    const int x_last = w - 1;
    const int y_last = h - 1;
    switch (quarters) {
        case 1: return PixelMap{0, -1, y_last, 1, 0, 0};
        case 2: return PixelMap{-1, 0, x_last, 0, -1, y_last};
        case 3: return PixelMap{0, 1, 0, -1, 0, x_last};
        case 0:
        default: return PixelMap{};
    }
}

void map_image(Image& image, const PixelMap& map) {
    if (image.empty()) return;
    const int w = image.width;
    const int h = image.height;
    const int out_w = map.swaps_axes() ? h : w;
    const int out_h = map.swaps_axes() ? w : h;

    Image out;
    out.resize(out_w, out_h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int dx = map.a * x + map.b * y + map.c;
            const int dy = map.d * x + map.e * y + map.f;
            const uint8_t* from = image.pixel(x, y);
            uint8_t* to = out.pixel(dx, dy);
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
        }
    }
    image = std::move(out);
}

ImageIntrinsics map_intrinsics(const ImageIntrinsics& in, const PixelMap& map, int out_w,
                               int out_h) {
    ImageIntrinsics out;
    out.width = out_w;
    out.height = out_h;
    out.cx = static_cast<float>(map.a) * in.cx + static_cast<float>(map.b) * in.cy +
             static_cast<float>(map.c);
    out.cy = static_cast<float>(map.d) * in.cx + static_cast<float>(map.e) * in.cy +
             static_cast<float>(map.f);
    // A transposing map turns the image's x axis into the y axis, so the focal
    // lengths travel with the axes.
    out.fx = map.swaps_axes() ? in.fy : in.fx;
    out.fy = map.swaps_axes() ? in.fx : in.fy;
    return out;
}

int normalize_quarters(int quarters) { return ((quarters % 4) + 4) % 4; }

}  // namespace

void apply_exif_orientation(Image& image, int orientation) {
    if (image.empty()) return;
    if (orientation < 1 || orientation > 8) orientation = 1;
    map_image(image, orientation_map(image.width, image.height, orientation));
}

void rotate_quarters(Image& image, int quarters) {
    if (image.empty()) return;
    map_image(image, quarter_map(image.width, image.height, normalize_quarters(quarters)));
}

ImageIntrinsics orient_intrinsics(const ImageIntrinsics& intrinsics, int orientation,
                                  int rotate_quarters) {
    if (orientation < 1 || orientation > 8) orientation = 1;

    // EXIF first, then the manual turns, exactly the order decode_photo()
    // applies them to the pixels.
    ImageIntrinsics out = intrinsics;
    if (orientation != 1 && out.width > 0 && out.height > 0) {
        const PixelMap map = orientation_map(out.width, out.height, orientation);
        out = map_intrinsics(out, map, map.swaps_axes() ? intrinsics.height : intrinsics.width,
                             map.swaps_axes() ? intrinsics.width : intrinsics.height);
    }
    const int quarters = normalize_quarters(rotate_quarters);
    if (quarters != 0 && out.width > 0 && out.height > 0) {
        const PixelMap map = quarter_map(out.width, out.height, quarters);
        out = map_intrinsics(out, map, map.swaps_axes() ? out.height : out.width,
                             map.swaps_axes() ? out.width : out.height);
    }
    return out;
}

}  // namespace image
}  // namespace splat
