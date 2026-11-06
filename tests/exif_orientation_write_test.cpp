// The Orientation tag writer: a JPEG or a baseline TIFF is turned without
// re-encoding a pixel. The eight EXIF values form a closed set under a 90
// degree turn, a file with no tag gains one, and the image data bytes are not
// touched.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/exif.h"
#include "common/image.h"
#include "jobs/image_io.h"
#include "test_util.h"

namespace {

using splat::Result;

std::vector<uint8_t> encode_jpeg(int width, int height) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(x * 255 / width);
            pixel[1] = static_cast<uint8_t>(y * 255 / height);
            pixel[2] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    const auto encoded = splat::encode_jpeg(image, 90);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    return encoded.value();
}

// A minimal TIFF: header, IFD0 with the Orientation entry (or none), no pixels.
std::vector<uint8_t> minimal_tiff(bool big_endian, int orientation) {
    std::vector<uint8_t> bytes;
    const auto u16 = [&bytes, big_endian](uint16_t value) {
        if (big_endian) {
            bytes.push_back(static_cast<uint8_t>(value >> 8));
            bytes.push_back(static_cast<uint8_t>(value & 0xff));
        } else {
            bytes.push_back(static_cast<uint8_t>(value & 0xff));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
        }
    };
    const auto u32 = [&bytes, big_endian](uint32_t value) {
        for (int shift = 0; shift < 4; ++shift) {
            const int at = big_endian ? (3 - shift) : shift;
            bytes.push_back(static_cast<uint8_t>((value >> (8 * at)) & 0xff));
        }
    };
    bytes.push_back(big_endian ? 'M' : 'I');
    bytes.push_back(big_endian ? 'M' : 'I');
    u16(42);
    u32(8);  // IFD0 at 8
    if (orientation == 0) {
        u16(0);
        u32(0);
        return bytes;
    }
    u16(1);
    u16(0x0112);
    u16(3);  // SHORT
    u32(1);
    u16(static_cast<uint16_t>(orientation));
    u16(0);
    u32(0);  // no next IFD
    return bytes;
}

// The JPEG with an Exif APP1 segment right after SOI whose IFD0 carries
// `orientation` (1..8).
std::vector<uint8_t> jpeg_with_exif(const std::vector<uint8_t>& jpeg, int orientation) {
    const std::vector<uint8_t> tiff = minimal_tiff(false, orientation);
    std::vector<uint8_t> app1 = {0xFF, 0xE1};
    const uint16_t length = static_cast<uint16_t>(2 + 6 + tiff.size());
    app1.push_back(static_cast<uint8_t>(length >> 8));
    app1.push_back(static_cast<uint8_t>(length & 0xff));
    const char header[] = {'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), header, header + 6);
    app1.insert(app1.end(), tiff.begin(), tiff.end());
    std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
    out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

// The bytes after the first APP1 segment (the entropy-coded image and any later
// marker): what a tag rewrite must copy verbatim.
std::vector<uint8_t> after_first_app1(const std::vector<uint8_t>& jpeg) {
    size_t pos = 2;
    while (pos + 4 <= jpeg.size()) {
        if (jpeg[pos] != 0xFF) break;
        const uint8_t marker = jpeg[pos + 1];
        const uint16_t length =
            static_cast<uint16_t>((jpeg[pos + 2] << 8) | jpeg[pos + 3]);
        if (marker == 0xE1 && length >= 8) {
            return std::vector<uint8_t>(jpeg.begin() + static_cast<std::ptrdiff_t>(pos + 2 + length),
                                        jpeg.end());
        }
        pos += 2 + length;
    }
    return {};
}

int orientation_of(const std::vector<uint8_t>& bytes) {
    const auto data = splat::parse_exif(bytes.data(), bytes.size());
    REQUIRE_MESSAGE(data.ok(), data.message());
    return data.value().orientation.value_or(1);
}

}  // namespace

TEST_CASE("exif orientation: all eight starting values turn both ways") {
    // The clockwise map and its inverse, plus the four-turn identity.
    for (int orientation = 1; orientation <= 8; ++orientation) {
        CAPTURE(orientation);
        const int right = splat::turn_exif_orientation(orientation, 1);
        CHECK(right >= 1);
        CHECK(right <= 8);
        CHECK(right != orientation);
        CHECK(splat::turn_exif_orientation(right, -1) == orientation);
        CHECK(splat::turn_exif_orientation(orientation, 4) == orientation);
        CHECK(splat::turn_exif_orientation(orientation, -4) == orientation);
        // Left is the inverse of right, and two rights are one half turn.
        CHECK(splat::turn_exif_orientation(orientation, -1) ==
              splat::turn_exif_orientation(orientation, 3));
        CHECK(splat::turn_exif_orientation(splat::turn_exif_orientation(orientation, 1), 1) ==
              splat::turn_exif_orientation(orientation, 2));
    }
    // The standard cases: a normal file turned right is EXIF 6, a portrait
    // phone photo (6) turned right is upside down (3), and a mirrored file
    // stays in the mirrored family.
    CHECK(splat::turn_exif_orientation(1, 1) == 6);
    CHECK(splat::turn_exif_orientation(6, 1) == 3);
    CHECK(splat::turn_exif_orientation(6, -1) == 1);
    CHECK(splat::turn_exif_orientation(2, 1) == 7);
    CHECK(splat::turn_exif_orientation(5, 1) == 2);
}

TEST_CASE("exif orientation: a JPEG with no Exif gains a valid tag, pixels untouched") {
    const std::vector<uint8_t> jpeg = encode_jpeg(64, 32);
    REQUIRE(orientation_of(jpeg) == 1);  // no APP1: the reader's default

    const Result<std::vector<uint8_t>> written = splat::set_exif_orientation(jpeg, 6);
    REQUIRE_MESSAGE(written.ok(), written.message());
    CHECK(orientation_of(written.value()) == 6);
    // The minimal segment was inserted right after SOI: everything from the old
    // byte 2 is still there, in order.
    const std::vector<uint8_t> suffix(written.value().begin() + 2, written.value().end());
    CHECK(suffix.size() > jpeg.size() - 2);
    CHECK(std::search(suffix.begin(), suffix.end(), jpeg.begin() + 2, jpeg.end()) != suffix.end());
    CHECK(after_first_app1(written.value()) ==
          std::vector<uint8_t>(jpeg.begin() + 2, jpeg.end()));
}

TEST_CASE("exif orientation: an existing tag is patched and the image bytes are identical") {
    const std::vector<uint8_t> jpeg = encode_jpeg(64, 32);
    const std::vector<uint8_t> tagged = jpeg_with_exif(jpeg, 1);
    REQUIRE(orientation_of(tagged) == 1);

    const Result<std::vector<uint8_t>> written = splat::set_exif_orientation(tagged, 8);
    REQUIRE_MESSAGE(written.ok(), written.message());
    CHECK(orientation_of(written.value()) == 8);
    // Same size (the entry already existed): only the two value bytes can
    // differ, and the compressed image after APP1 is byte-identical.
    CHECK(written.value().size() == tagged.size());
    int differences = 0;
    for (size_t i = 0; i < tagged.size(); ++i) {
        if (tagged[i] != written.value()[i]) ++differences;
    }
    CHECK(differences <= 2);
    CHECK(after_first_app1(written.value()) == after_first_app1(tagged));
}

TEST_CASE("exif orientation: a JPEG with Exif but no tag gains one, image bytes copied") {
    const std::vector<uint8_t> jpeg = encode_jpeg(64, 32);
    // An APP1 whose IFD0 holds one unrelated entry (ImageWidth), so the tag is
    // absent and the writer has to extend the directory.
    std::vector<uint8_t> tiff = {'I', 'I', 0x2A, 0x00, 8, 0, 0, 0};
    tiff.push_back(1);
    tiff.push_back(0);
    tiff.push_back(0x00);  // tag 0x0100 ImageWidth
    tiff.push_back(0x01);
    tiff.push_back(4);  // LONG
    tiff.push_back(0);
    tiff.push_back(1);  // count
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(64);  // value 64
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);  // no next IFD
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    std::vector<uint8_t> app1 = {0xFF, 0xE1};
    const uint16_t length = static_cast<uint16_t>(2 + 6 + tiff.size());
    app1.push_back(static_cast<uint8_t>(length >> 8));
    app1.push_back(static_cast<uint8_t>(length & 0xff));
    const char header[] = {'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), header, header + 6);
    app1.insert(app1.end(), tiff.begin(), tiff.end());
    std::vector<uint8_t> input(jpeg.begin(), jpeg.begin() + 2);
    input.insert(input.end(), app1.begin(), app1.end());
    input.insert(input.end(), jpeg.begin() + 2, jpeg.end());
    REQUIRE(orientation_of(input) == 1);

    const Result<std::vector<uint8_t>> written = splat::set_exif_orientation(input, 3);
    REQUIRE_MESSAGE(written.ok(), written.message());
    CHECK(orientation_of(written.value()) == 3);
    // The APP1 segment grew by a new IFD0 copy with both entries; the image
    // after it is identical.
    CHECK(written.value().size() > input.size());
    CHECK(after_first_app1(written.value()) == after_first_app1(input));
}

TEST_CASE("exif orientation: a baseline TIFF gains or patches the tag in both byte orders") {
    for (const bool big_endian : {false, true}) {
        CAPTURE(big_endian);
        // Absent: the writer appends a copy of IFD0 with Orientation, and the
        // header points at it. The old IFD0 is still in the file.
        const std::vector<uint8_t> no_tag = minimal_tiff(big_endian, 0);
        const Result<std::vector<uint8_t>> written = splat::set_exif_orientation(no_tag, 6);
        REQUIRE_MESSAGE(written.ok(), written.message());
        CHECK(orientation_of(written.value()) == 6);
        CHECK(written.value().size() > no_tag.size());

        // Present: patched in place, same size, only the value changes.
        for (int orientation = 1; orientation <= 8; ++orientation) {
            const std::vector<uint8_t> tagged = minimal_tiff(big_endian, orientation);
            const int next = splat::turn_exif_orientation(orientation, 1);
            const Result<std::vector<uint8_t>> turned = splat::set_exif_orientation(tagged, next);
            REQUIRE_MESSAGE(turned.ok(), turned.message());
            CHECK(orientation_of(turned.value()) == next);
            CHECK(turned.value().size() == tagged.size());
        }
    }
}
