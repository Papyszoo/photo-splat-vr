// EXIF orientation and manual 90-degree rotation for a decoded photo.
//
// Both are the same thing: a 2x3 pixel map. The eight EXIF cases, and each
// clockwise quarter turn, are expressed as (a b c / d e f) with
//   x' = a*x + b*y + c,  y' = d*x + e*y + f
// and the image and the intrinsics are mapped with that one map, so a rotated
// scene can never carry intrinsics for a differently oriented photo.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/image.h"

namespace splat {
namespace image {

// Focal length and principal point in pixels of a decoded image, at the size
// it has before any orientation. Kept local to core/image so the layer does not
// depend on core/scene; core/jobs/image_io.cpp maps it to splat::Intrinsics.
struct ImageIntrinsics {
    int width = 0;
    int height = 0;
    float fx = 0.0f;
    float fy = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
};

// Applies EXIF orientation 1..8 (values outside that range are treated as 1):
// 1 as stored, 2 mirrored horizontally, 3 rotated 180, 4 mirrored vertically,
// 5 transposed, 6 rotated 90 clockwise, 7 transversed, 8 rotated 270 clockwise.
void apply_exif_orientation(Image& image, int orientation);

// Clockwise quarter turns, `quarters` taken mod 4; negative values turn
// counter-clockwise. Four calls restore the bytes exactly.
void rotate_quarters(Image& image, int quarters);

// The principal point is moved by the same map as the pixels, and fx/fy swap
// when the map swaps the axes.
ImageIntrinsics orient_intrinsics(const ImageIntrinsics& intrinsics, int orientation,
                                  int rotate_quarters);

}  // namespace image
}  // namespace splat
