#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include "common/exif.h"

namespace {

void append_jpeg(void* context, void* data, int size) {
    auto* bytes = static_cast<std::vector<uint8_t>*>(context);
    const auto* first = static_cast<const uint8_t*>(data);
    bytes->insert(bytes->end(), first, first + size);
}

// A real, decodable JPEG with no metadata. EXIF is injected afterwards.
std::vector<uint8_t> make_bare_jpeg(int width, int height) {
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = rgb.data() + (static_cast<size_t>(y) * width + x) * 3;
            p[0] = static_cast<uint8_t>(x * 255 / width);
            p[1] = static_cast<uint8_t>(y * 255 / height);
            p[2] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    std::vector<uint8_t> jpeg;
    REQUIRE(stbi_write_jpg_to_func(append_jpeg, &jpeg, width, height, 3, rgb.data(), 90) != 0);
    return jpeg;
}

void put_u16(std::vector<uint8_t>& out, size_t offset, uint16_t value, bool big_endian) {
    if (big_endian) {
        out[offset + 0] = static_cast<uint8_t>(value >> 8);
        out[offset + 1] = static_cast<uint8_t>(value & 0xff);
    } else {
        out[offset + 0] = static_cast<uint8_t>(value & 0xff);
        out[offset + 1] = static_cast<uint8_t>(value >> 8);
    }
}

void put_u32(std::vector<uint8_t>& out, size_t offset, uint32_t value, bool big_endian) {
    if (big_endian) {
        out[offset + 0] = static_cast<uint8_t>(value >> 24);
        out[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xff);
        out[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[offset + 3] = static_cast<uint8_t>(value & 0xff);
    } else {
        out[offset + 0] = static_cast<uint8_t>(value & 0xff);
        out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
        out[offset + 3] = static_cast<uint8_t>(value >> 24);
    }
}

// Minimal TIFF/Exif with FocalLength and optionally FocalLengthIn35mmFilm.
std::vector<uint8_t> make_tiff(bool big_endian, bool include_35mm, uint16_t focal_35mm,
                               uint32_t focal_num, uint32_t focal_den) {
    const size_t ifd0 = 8;              // 8-byte TIFF header
    const size_t exif_ifd = ifd0 + 2 + 12 + 4;  // one entry + next-IFD pointer
    const size_t entry_count = include_35mm ? 2 : 1;
    const size_t values_offset = exif_ifd + 2 + entry_count * 12 + 4;

    std::vector<uint8_t> tiff(values_offset + 8, 0);
    tiff[0] = big_endian ? 'M' : 'I';
    tiff[1] = big_endian ? 'M' : 'I';
    put_u16(tiff, 2, 42, big_endian);
    put_u32(tiff, 4, static_cast<uint32_t>(ifd0), big_endian);

    // IFD0: ExifIFD pointer only.
    put_u16(tiff, ifd0, 1, big_endian);
    put_u16(tiff, ifd0 + 2, 0x8769, big_endian);  // ExifIFD
    put_u16(tiff, ifd0 + 4, 4, big_endian);        // LONG
    put_u32(tiff, ifd0 + 6, 1, big_endian);
    put_u32(tiff, ifd0 + 10, static_cast<uint32_t>(exif_ifd), big_endian);
    put_u32(tiff, ifd0 + 14, 0, big_endian);  // no next IFD

    // Exif SubIFD.
    put_u16(tiff, exif_ifd, static_cast<uint16_t>(entry_count), big_endian);
    size_t entry = exif_ifd + 2;
    if (include_35mm) {
        put_u16(tiff, entry + 0, 0xA405, big_endian);  // FocalLengthIn35mmFilm
        put_u16(tiff, entry + 2, 3, big_endian);       // SHORT
        put_u32(tiff, entry + 4, 1, big_endian);
        put_u16(tiff, entry + 8, focal_35mm, big_endian);
        entry += 12;
    }
    put_u16(tiff, entry + 0, 0x920A, big_endian);  // FocalLength
    put_u16(tiff, entry + 2, 5, big_endian);       // RATIONAL
    put_u32(tiff, entry + 4, 1, big_endian);
    put_u32(tiff, entry + 8, static_cast<uint32_t>(values_offset), big_endian);
    entry += 12;
    put_u32(tiff, entry, 0, big_endian);  // no next IFD

    put_u32(tiff, values_offset + 0, focal_num, big_endian);
    put_u32(tiff, values_offset + 4, focal_den, big_endian);
    return tiff;
}

std::vector<uint8_t> inject_exif(const std::vector<uint8_t>& jpeg, const std::vector<uint8_t>& tiff) {
    std::vector<uint8_t> out;
    out.reserve(jpeg.size() + tiff.size() + 10);
    out.push_back(jpeg[0]);
    out.push_back(jpeg[1]);

    const size_t payload_size = 6 + tiff.size();
    const size_t segment_length = payload_size + 2;
    REQUIRE(segment_length <= 0xffff);
    out.push_back(0xFF);
    out.push_back(0xE1);
    out.push_back(static_cast<uint8_t>(segment_length >> 8));
    out.push_back(static_cast<uint8_t>(segment_length & 0xff));
    const uint8_t exif_id[6] = {'E', 'x', 'i', 'f', 0, 0};
    out.insert(out.end(), exif_id, exif_id + 6);
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

}  // namespace

TEST_CASE("focal_px_from_mm matches the 35 mm film formula") {
    CHECK(splat::focal_px_from_mm(30.0, 1280, 720) == doctest::Approx(1018.2942f).epsilon(1e-5));
    CHECK(splat::focal_px_from_mm(30.0, 1280, 873) == doctest::Approx(1074.2907f).epsilon(1e-5));
    CHECK(splat::focal_px_from_mm(30.0, 1280, 964) == doctest::Approx(1111.0667f).epsilon(1e-5));
}

TEST_CASE("EXIF focal parsing uses FocalLengthIn35mmFilm first") {
    const auto jpeg = inject_exif(make_bare_jpeg(64, 48),
                                  make_tiff(false, true, 35, 50, 1));
    const auto focal = splat::focal_px_from_jpeg(jpeg.data(), jpeg.size(), 1280, 720);
    REQUIRE_MESSAGE(focal.ok(), focal.message());
    CHECK(focal.value() == doctest::Approx(1188.0099f).epsilon(1e-5));

    const auto exif = splat::parse_jpeg_exif_focal(jpeg.data(), jpeg.size());
    REQUIRE_MESSAGE(exif.ok(), exif.message());
    REQUIRE(exif.value().focal_length_35mm.has_value());
    REQUIRE(exif.value().focal_length_mm.has_value());
    CHECK(*exif.value().focal_length_35mm == doctest::Approx(35.0));
    CHECK(*exif.value().focal_length_mm == doctest::Approx(50.0));
}

TEST_CASE("EXIF focal parsing falls back to FocalLength and handles big-endian TIFF") {
    const auto jpeg = inject_exif(make_bare_jpeg(64, 48), make_tiff(true, false, 0, 50, 1));
    const auto focal = splat::focal_px_from_jpeg(jpeg.data(), jpeg.size(), 1280, 720);
    REQUIRE_MESSAGE(focal.ok(), focal.message());
    CHECK(focal.value() == doctest::Approx(1697.1570f).epsilon(1e-5));

    const auto low = inject_exif(make_bare_jpeg(64, 48), make_tiff(false, false, 0, 5, 1));
    const auto low_focal = splat::focal_px_from_jpeg(low.data(), low.size(), 1280, 720);
    REQUIRE_MESSAGE(low_focal.ok(), low_focal.message());
    CHECK(low_focal.value() == doctest::Approx(1425.6119f).epsilon(1e-5));  // 5 mm * 8.4
}

TEST_CASE("JPEG without EXIF uses the 30 mm default") {
    const auto jpeg = make_bare_jpeg(64, 48);
    const auto focal = splat::focal_px_from_jpeg(jpeg.data(), jpeg.size(), 1280, 720);
    REQUIRE_MESSAGE(focal.ok(), focal.message());
    CHECK(focal.value() == doctest::Approx(1018.2942f).epsilon(1e-5));

    const auto exif = splat::parse_jpeg_exif_focal(jpeg.data(), jpeg.size());
    REQUIRE_MESSAGE(exif.ok(), exif.message());
    CHECK_FALSE(exif.value().focal_length_mm.has_value());
    CHECK_FALSE(exif.value().focal_length_35mm.has_value());
}

TEST_CASE("EXIF parser rejects non-JPEG input and truncated EXIF") {
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
    CHECK_FALSE(splat::parse_jpeg_exif_focal(png, sizeof(png)).ok());
    CHECK_FALSE(splat::parse_jpeg_exif_focal(nullptr, 0).ok());

    const auto jpeg = make_bare_jpeg(32, 32);
    auto broken = inject_exif(jpeg, make_tiff(false, true, 35, 50, 1));
    broken.resize(20);  // truncate inside the APP1 segment
    CHECK_FALSE(splat::parse_jpeg_exif_focal(broken.data(), broken.size()).ok());
}

TEST_CASE("EXIF parser survives hostile TIFF offsets") {
    // Every value below is a valid TIFF header with one deliberately broken
    // pointer. The parser must either reject the file or fall back to the
    // default focal length; it must never read outside the buffer.
    const auto jpeg = make_bare_jpeg(16, 16);
    // Byte offsets inside make_tiff(): the first Exif SubIFD entry starts here,
    // its type/count/value fields at +2/+4/+8.
    const size_t exif_entry = 8 + 2 + 12 + 4 + 2;

    // 1. IFD0 offset past the end of the buffer.
    {
        auto tiff = make_tiff(false, true, 35, 50, 1);
        put_u32(tiff, 4, 0xffffff00u, false);
        const auto out = inject_exif(jpeg, tiff);
        CHECK_FALSE(splat::parse_jpeg_exif_focal(out.data(), out.size()).ok());
    }
    // 2. IFD0 offset that only leaves room for the count field.
    {
        auto tiff = make_tiff(false, true, 35, 50, 1);
        put_u32(tiff, 4, static_cast<uint32_t>(tiff.size() - 1), false);
        const auto out = inject_exif(jpeg, tiff);
        CHECK_FALSE(splat::parse_jpeg_exif_focal(out.data(), out.size()).ok());
    }
    // 3. Entry count far larger than the segment.
    {
        auto tiff = make_tiff(false, true, 35, 50, 1);
        put_u16(tiff, 8, 0xffff, false);
        const auto out = inject_exif(jpeg, tiff);
        CHECK_FALSE(splat::parse_jpeg_exif_focal(out.data(), out.size()).ok());
    }
    // 4. Exif SubIFD pointer past the end.
    {
        auto tiff = make_tiff(false, true, 35, 50, 1);
        put_u32(tiff, 8 + 10, 0x7ffffff0u, false);
        const auto out = inject_exif(jpeg, tiff);
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        CHECK_FALSE(exif.ok());
    }
    // 5. Exif SubIFD pointer at the entry count itself: a self-reference, which
    //    must not recurse or loop.
    {
        auto tiff = make_tiff(false, true, 35, 50, 1);
        put_u32(tiff, 8 + 10, 8u, false);
        const auto out = inject_exif(jpeg, tiff);
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        CHECK(exif.ok());
    }
    // 6. A RATIONAL whose value offset points outside the buffer: the tag is
    //    skipped, the rest of the file is still read.
    {
        auto tiff = make_tiff(false, false, 0, 50, 1);
        put_u32(tiff, exif_entry + 8, 0xfffffff0u, false);
        const auto out = inject_exif(jpeg, tiff);
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        REQUIRE_MESSAGE(exif.ok(), exif.message());
        CHECK_FALSE(exif.value().focal_length_mm.has_value());
    }
    // 7. A value offset that wraps when added to the element size.
    {
        auto tiff = make_tiff(false, false, 0, 50, 1);
        put_u32(tiff, exif_entry + 8, 0xffffffffu, false);
        const auto out = inject_exif(jpeg, tiff);
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        REQUIRE_MESSAGE(exif.ok(), exif.message());
        CHECK_FALSE(exif.value().focal_length_mm.has_value());
    }
    // 8. A huge element count for a RATIONAL only ever reads the first value,
    //    and that read is still bounds-checked; the value itself is intact.
    {
        auto tiff = make_tiff(false, false, 0, 50, 1);
        put_u32(tiff, exif_entry + 4, 0xffffffffu, false);
        const auto out = inject_exif(jpeg, tiff);
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        REQUIRE_MESSAGE(exif.ok(), exif.message());
        REQUIRE(exif.value().focal_length_mm.has_value());
        CHECK(*exif.value().focal_length_mm == doctest::Approx(50.0));
    }
    // 9. An unsupported value type is ignored, not misread, and the following
    //    entry is still parsed.
    {
        auto tiff = make_tiff(false, true, 35, 50, 1);
        put_u16(tiff, exif_entry + 2, 99, false);
        const auto out = inject_exif(jpeg, tiff);
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        REQUIRE_MESSAGE(exif.ok(), exif.message());
        CHECK_FALSE(exif.value().focal_length_35mm.has_value());
        REQUIRE(exif.value().focal_length_mm.has_value());
        CHECK(*exif.value().focal_length_mm == doctest::Approx(50.0));
    }
    // 10. Zero denominator: meaningless, so the tag is dropped.
    {
        const auto out = inject_exif(jpeg, make_tiff(false, false, 0, 50, 0));
        const auto exif = splat::parse_jpeg_exif_focal(out.data(), out.size());
        REQUIRE_MESSAGE(exif.ok(), exif.message());
        CHECK_FALSE(exif.value().focal_length_mm.has_value());
    }
    // 11. Every prefix of a valid file must be handled without reading past it.
    {
        const auto full = inject_exif(jpeg, make_tiff(false, true, 35, 50, 1));
        for (size_t length = 0; length <= full.size(); ++length) {
            const auto exif = splat::parse_jpeg_exif_focal(full.data(), length);
            // Ok or error, but no crash and no nonsense value.
            if (exif.ok() && exif.value().focal_length_mm.has_value()) {
                CHECK(*exif.value().focal_length_mm == doctest::Approx(50.0));
            }
        }
    }
}

TEST_CASE("EXIF focal length stays finite for absurd tags") {
    const auto jpeg = make_bare_jpeg(16, 16);
    // 35 mm = 65535 is the largest SHORT the format allows.
    {
        const auto out = inject_exif(jpeg, make_tiff(false, true, 65535, 1, 1));
        const auto focal = splat::focal_px_from_jpeg(out.data(), out.size(), 1280, 720);
        REQUIRE_MESSAGE(focal.ok(), focal.message());
        CHECK(std::isfinite(focal.value()));
        CHECK(focal.value() > 0.0f);
    }
    // FocalLength = 4/1 mm goes through the below-10 mm correction.
    {
        const auto out = inject_exif(jpeg, make_tiff(false, false, 0, 4, 1));
        const auto focal = splat::focal_px_from_jpeg(out.data(), out.size(), 1280, 720);
        REQUIRE_MESSAGE(focal.ok(), focal.message());
        CHECK(std::isfinite(focal.value()));
        CHECK(focal.value() > 0.0f);
    }
}
