// EXIF orientation and manual quarter turns: the eight cases as one 2x3 pixel
// map, and the intrinsics mapped with that same map.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "common/image.h"
#include "image/orientation.h"

using splat::Image;
using splat::image::ImageIntrinsics;

namespace {

// 3x2, rgb[i] = i, so pixel (x, y) starts at byte 3 * (y * 3 + x).
Image make_strip() {
    Image image;
    image.resize(3, 2);
    for (std::size_t i = 0; i < image.rgb.size(); ++i) {
        image.rgb[i] = static_cast<uint8_t>(i);
    }
    return image;
}

int first_byte(const Image& image, int x, int y) { return image.pixel(x, y)[0]; }

bool same_bytes(const Image& a, const Image& b) {
    return a.width == b.width && a.height == b.height && a.rgb == b.rgb;
}

}  // namespace

TEST_CASE("orientation: all eight EXIF cases, size and the two extreme pixels") {
    struct Case {
        int orientation;
        int width;
        int height;
        int top_left;       // source pixel index whose byte triple starts at 3*index
        int bottom_right;
    };
    // Expected values follow from the map in image/orientation.cpp: each entry
    // names the source pixel at that index.
    const Case cases[] = {
        {1, 3, 2, 0, 5},   // as stored
        {2, 3, 2, 2, 3},   // mirror horizontally
        {3, 3, 2, 5, 0},   // rotate 180
        {4, 3, 2, 3, 2},   // mirror vertically
        {5, 2, 3, 0, 5},   // transpose
        {6, 2, 3, 3, 2},   // rotate 90 clockwise
        {7, 2, 3, 5, 0},   // transverse
        {8, 2, 3, 2, 3},   // rotate 270 clockwise
    };

    for (const Case& item : cases) {
        CAPTURE(item.orientation);
        Image image = make_strip();
        splat::image::apply_exif_orientation(image, item.orientation);
        CHECK(image.width == item.width);
        CHECK(image.height == item.height);
        CHECK(first_byte(image, 0, 0) == item.top_left * 3);
        CHECK(first_byte(image, image.width - 1, image.height - 1) == item.bottom_right * 3);
        // The middle of the byte range survives: every source triple appears
        // exactly once in the output.
        std::vector<int> seen(6, 0);
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const int source = first_byte(image, x, y) / 3;
                REQUIRE(source >= 0);
                REQUIRE(source < 6);
                ++seen[static_cast<std::size_t>(source)];
            }
        }
        for (int count : seen) CHECK(count == 1);
    }

    // Values outside 1..8 are "as stored", never a black image.
    Image image = make_strip();
    splat::image::apply_exif_orientation(image, 0);
    CHECK(same_bytes(image, make_strip()));
    splat::image::apply_exif_orientation(image, 99);
    CHECK(same_bytes(image, make_strip()));
}

TEST_CASE("orientation: four quarter turns restore the bytes exactly") {
    Image image = make_strip();
    const Image original = image;
    for (int step = 0; step < 4; ++step) {
        splat::image::rotate_quarters(image, 1);
        CAPTURE(step);
        if (step % 2 == 0) {
            // One turn is a size swap, two turns are not.
            CHECK((image.width == 2) == (step == 0 || step == 2));
        }
    }
    CHECK(same_bytes(image, original));

    // Four counter-clockwise turns too, and a negative count is the same as
    // its positive equivalent.
    Image left = make_strip();
    for (int step = 0; step < 4; ++step) splat::image::rotate_quarters(left, -1);
    CHECK(same_bytes(left, original));

    Image minus_one = make_strip();
    Image plus_three = make_strip();
    splat::image::rotate_quarters(minus_one, -1);
    splat::image::rotate_quarters(plus_three, 3);
    CHECK(same_bytes(minus_one, plus_three));

    Image no_turn = make_strip();
    splat::image::rotate_quarters(no_turn, 4);
    CHECK(same_bytes(no_turn, original));
}

TEST_CASE("orientation: intrinsics follow the same map as the pixels") {
    const ImageIntrinsics source{3, 2, 30.0f, 40.0f, 1.5f, 1.0f};

    // 90 degrees clockwise: the axes swap, cx' = (h - 1) - cy, cy' = cx.
    const ImageIntrinsics rotated = splat::image::orient_intrinsics(source, 6, 0);
    CHECK(rotated.width == 2);
    CHECK(rotated.height == 3);
    CHECK(rotated.fx == doctest::Approx(40.0f));
    CHECK(rotated.fy == doctest::Approx(30.0f));
    CHECK(rotated.cx == doctest::Approx(0.0f));
    CHECK(rotated.cy == doctest::Approx(1.5f));

    // Orientation 1 and no turn change nothing.
    const ImageIntrinsics unchanged = splat::image::orient_intrinsics(source, 1, 0);
    CHECK(unchanged.width == 3);
    CHECK(unchanged.height == 2);
    CHECK(unchanged.fx == doctest::Approx(30.0f));
    CHECK(unchanged.fy == doctest::Approx(40.0f));
    CHECK(unchanged.cx == doctest::Approx(1.5f));
    CHECK(unchanged.cy == doctest::Approx(1.0f));

    // Four turns are the identity, and the manual turn composes after the EXIF
    // one (image and intrinsics use the same order).
    ImageIntrinsics four = source;
    for (int step = 0; step < 4; ++step) four = splat::image::orient_intrinsics(four, 1, 1);
    CHECK(four.width == 3);
    CHECK(four.height == 2);
    CHECK(four.cx == doctest::Approx(1.5f));
    CHECK(four.cy == doctest::Approx(1.0f));
    CHECK(four.fx == doctest::Approx(30.0f));
    CHECK(four.fy == doctest::Approx(40.0f));

    // A mirrored orientation keeps the axes: cx moves, fx/fy do not swap.
    const ImageIntrinsics mirrored = splat::image::orient_intrinsics(source, 2, 0);
    CHECK(mirrored.width == 3);
    CHECK(mirrored.height == 2);
    CHECK(mirrored.cx == doctest::Approx(0.5f));  // (3 - 1) - 1.5
    CHECK(mirrored.cy == doctest::Approx(1.0f));
    CHECK(mirrored.fx == doctest::Approx(30.0f));
    CHECK(mirrored.fy == doctest::Approx(40.0f));
}
