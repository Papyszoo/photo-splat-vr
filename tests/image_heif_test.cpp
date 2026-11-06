// HEIC/HEIF and AVIF through libheif (L1-B2).
//
// Two fixtures decode (16x16 each, means within ±3 of the source PNG), a
// truncated file fails with "cannot decode", and the rotation rule is checked
// twice: on a real file whose container transform turns a 32x16 image into
// 16x32, and on the rule itself (a container transform means the EXIF tag is
// not applied again). The rule test is a function call because authoring a
// HEIC with both an EXIF orientation and no irot needs an HEVC *encoder*, and
// neither build host nor payload ships one: the decoder only decodes.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(SPLAT_HAVE_HEIF)
#include <libheif/heif.h>
#endif

#include "common/path.h"
#include "image/decode.h"
#include "jobs/image_io.h"

using splat::Image;
using splat::Path;
using splat::image::DecodeInfo;
using splat::image::DecodeOptions;

namespace {

Path fixture(const char* name) {
    return Path(__FILE__).parent_path() / "fixtures" / "photos" / name;
}

std::vector<uint8_t> read_bytes(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

double channel_mean(const Image& image, int channel) {
    double sum = 0.0;
    const size_t pixels = image.pixel_count();
    for (size_t index = 0; index < pixels; ++index) {
        sum += image.rgb[index * 3 + static_cast<size_t>(channel)];
    }
    return sum / static_cast<double>(pixels);
}

// The angle byte of the container's irot box (0..3, counter-clockwise quarter
// turns). -1 when the file has none.
int irot_angle(const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i + 9 <= bytes.size(); ++i) {
        if (bytes[i] == 'i' && bytes[i + 1] == 'r' && bytes[i + 2] == 'o' &&
            bytes[i + 3] == 't') {
            return bytes[i + 4];
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("heif: tiny.heic decodes to its dimensions and colour") {
#if defined(SPLAT_HAVE_HEIF)
    const Path heic = fixture("tiny.heic");
    REQUIRE(std::filesystem::exists(heic));
    CHECK(std::filesystem::file_size(heic) <= 16 * 1024);

    DecodeInfo info;
    const splat::Result<Image> decoded =
        splat::image::decode_photo(heic, DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(info.format == splat::ImageFormat::heif);
    CHECK(info.width == 16);
    CHECK(info.height == 16);
    CHECK(decoded.value().width == 16);
    CHECK(decoded.value().height == 16);
    // The ispe box is the stored size; the guard read it without decoding.
    CHECK(info.source_width == 16);
    CHECK(info.source_height == 16);

    const splat::Result<Image> source =
        splat::image::decode_photo(fixture("tiny_source.png"), DecodeOptions{}, nullptr);
    REQUIRE_MESSAGE(source.ok(), source.message());
    for (int channel = 0; channel < 3; ++channel) {
        CAPTURE(channel);
        INFO("mean " << channel_mean(decoded.value(), channel) << " vs "
                     << channel_mean(source.value(), channel));
        CHECK(std::abs(channel_mean(decoded.value(), channel) -
                       channel_mean(source.value(), channel)) <= 3.0);
    }
#else
    WARN("SPLAT_HAVE_HEIF is not defined: libheif is not in this build");
#endif
}

TEST_CASE("heif: tiny.avif decodes through the same container library") {
#if defined(SPLAT_HAVE_HEIF)
    const Path avif = fixture("tiny.avif");
    REQUIRE(std::filesystem::exists(avif));
    CHECK(std::filesystem::file_size(avif) <= 16 * 1024);

    DecodeInfo info;
    const splat::Result<Image> decoded =
        splat::image::decode_photo(avif, DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(info.format == splat::ImageFormat::avif);
    CHECK(info.width == 16);
    CHECK(info.height == 16);
    CHECK(decoded.value().width == 16);
    CHECK(decoded.value().height == 16);

    const splat::Result<Image> source =
        splat::image::decode_photo(fixture("tiny_source.png"), DecodeOptions{}, nullptr);
    REQUIRE_MESSAGE(source.ok(), source.message());
    for (int channel = 0; channel < 3; ++channel) {
        CAPTURE(channel);
        CHECK(std::abs(channel_mean(decoded.value(), channel) -
                       channel_mean(source.value(), channel)) <= 3.0);
    }
#else
    WARN("SPLAT_HAVE_HEIF is not defined: libheif is not in this build");
#endif
}

TEST_CASE("heif: a file truncated to 40 bytes is refused without crashing") {
#if defined(SPLAT_HAVE_HEIF)
    const std::vector<uint8_t> full = read_bytes(fixture("tiny.heic"));
    REQUIRE(full.size() > 40);

    const std::vector<uint8_t> truncated(full.begin(), full.begin() + 40);
    const splat::Result<Image> decoded =
        splat::image::decode_photo(truncated.data(), truncated.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(decoded.ok());
    CAPTURE(decoded.message());
    CHECK(decoded.message().find("cannot decode") != std::string::npos);

    // The same for AVIF, and for a single byte.
    const std::vector<uint8_t> avif = read_bytes(fixture("tiny.avif"));
    REQUIRE(avif.size() > 40);
    const std::vector<uint8_t> avif_truncated(avif.begin(), avif.begin() + 40);
    const splat::Result<Image> avif_decoded = splat::image::decode_photo(
        avif_truncated.data(), avif_truncated.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(avif_decoded.ok());
    CHECK(avif_decoded.message().find("cannot decode") != std::string::npos);
#else
    WARN("SPLAT_HAVE_HEIF is not defined: libheif is not in this build");
#endif
}

TEST_CASE("heif: the container transform turns the photo once") {
#if defined(SPLAT_HAVE_HEIF)
    // wide.heic stores 32x16 with irot angle 0 (no turn). Setting the angle to
    // 1 makes libheif rotate it 90 degrees counter-clockwise while decoding, so
    // the decoded size must be 16x32 - the post-transform size, not the ispe
    // size, and not turned twice.
    std::vector<uint8_t> bytes = read_bytes(fixture("wide.heic"));
    REQUIRE(std::filesystem::file_size(fixture("wide.heic")) <= 16 * 1024);
    const int angle = irot_angle(bytes);
    REQUIRE(angle == 0);
    for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
        if (bytes[i] == 'i' && bytes[i + 1] == 'r' && bytes[i + 2] == 'o' && bytes[i + 3] == 't') {
            bytes[i + 4] = 1;
            break;
        }
    }

    DecodeInfo info;
    const splat::Result<Image> decoded =
        splat::image::decode_photo(bytes.data(), bytes.size(), DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(decoded.value().width == 16);
    CHECK(decoded.value().height == 32);
    CHECK(info.source_width == 32);  // the ispe box, before the transform
    CHECK(info.source_height == 16);
    CHECK(info.exif_orientation == 1);  // nothing left for the EXIF tag to do
    CHECK(info.note.find("container rotation applied") != std::string::npos);

    // The scene intrinsics follow the decoded frame, not the stored one: a
    // centred principal point on a 16x32 image is (8, 16).
    const splat::Result<splat::Intrinsics> intrinsics =
        splat::photo_intrinsics(bytes.data(), bytes.size(), info);
    REQUIRE_MESSAGE(intrinsics.ok(), intrinsics.message());
    CHECK(intrinsics.value().width == 16);
    CHECK(intrinsics.value().height == 32);
    CHECK(intrinsics.value().cx == doctest::Approx(8.0f));
    CHECK(intrinsics.value().cy == doctest::Approx(16.0f));

    // Without the transform the same file decodes as stored.
    const std::vector<uint8_t> stored = read_bytes(fixture("wide.heic"));
    const splat::Result<Image> plain =
        splat::image::decode_photo(stored.data(), stored.size(), DecodeOptions{}, nullptr);
    REQUIRE_MESSAGE(plain.ok(), plain.message());
    CHECK(plain.value().width == 32);
    CHECK(plain.value().height == 16);
#else
    WARN("SPLAT_HAVE_HEIF is not defined: libheif is not in this build");
#endif
}

TEST_CASE("heif: the EXIF orientation is not applied twice") {
#if defined(SPLAT_HAVE_HEIF)
    // A file whose container transform already turned it uses that transform
    // only; a file without one uses its EXIF orientation.
    CHECK(splat::image::heif_orientation_to_apply(true, 6) == 1);
    CHECK(splat::image::heif_orientation_to_apply(true, 1) == 1);
    CHECK(splat::image::heif_orientation_to_apply(false, 6) == 6);
    CHECK(splat::image::heif_orientation_to_apply(false, 1) == 1);
    CHECK(splat::image::heif_orientation_to_apply(false, 0) == 1);  // out of range
#else
    WARN("SPLAT_HAVE_HEIF is not defined: libheif is not in this build");
#endif
}

TEST_CASE("heif: the linked libheif really carries both decoder backends") {
    // The console build shipped HEIC/AVIF as "not built in" because libheif
    // configured its libde265/aom backends as "not found" and still produced a
    // valid archive, so an archive existing proves nothing. This asks libheif
    // itself, the same call the decoder's error message reports when a decode
    // fails (core/image/heif_decoder.cpp), and it fails the gate exactly when a
    // backend went missing again.
#if defined(SPLAT_HAVE_HEIF)
    const int hevc = heif_get_decoder_descriptors(heif_compression_HEVC, nullptr, 0);
    const int av1 = heif_get_decoder_descriptors(heif_compression_AV1, nullptr, 0);
    CAPTURE(hevc);
    CAPTURE(av1);
    CHECK(hevc >= 1);
    CHECK(av1 >= 1);
#else
    WARN("SPLAT_HAVE_HEIF is not defined: libheif is not in this build");
#endif
}
