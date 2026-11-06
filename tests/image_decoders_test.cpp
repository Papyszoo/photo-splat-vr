// WebP and TIFF (L1-B1). The TIFFs are built from bytes in the test, so the
// LZW case really proves the codec is linked in; the WebP fixture is the
// 16x16 committed file from tests/fixtures/photos (provenance in its README).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void put_entry(std::vector<uint8_t>& out, uint16_t tag, uint16_t type, uint32_t count,
               uint32_t value) {
    put_u16(out, tag);
    put_u16(out, type);
    put_u32(out, count);
    put_u32(out, value);
}

// A baseline little-endian TIFF: one image, one strip, no extras. `orientation`
// 0 omits the tag; any other value adds it (0x0112), which is what the
// double-rotation regression test needs.
std::vector<uint8_t> make_tiff(uint16_t compression, const std::vector<uint8_t>& strip,
                               uint16_t orientation = 0, uint32_t width = 2,
                               uint32_t height = 2) {
    const uint16_t entries = orientation != 0 ? 10 : 9;
    const uint32_t bits_offset = 8 + 2 + entries * 12 + 4;
    const uint32_t strip_offset = bits_offset + 6;
    std::vector<uint8_t> out;
    out.push_back('I');
    out.push_back('I');
    put_u16(out, 42);
    put_u32(out, 8);  // IFD0 at 8

    put_u16(out, entries);
    put_entry(out, 0x0100, 4, 1, width);                       // ImageWidth
    put_entry(out, 0x0101, 4, 1, height);                      // ImageLength
    put_entry(out, 0x0102, 3, 3, bits_offset);                 // BitsPerSample
    put_entry(out, 0x0103, 3, 1, compression);                 // Compression
    put_entry(out, 0x0106, 3, 1, 2);                           // Photometric: RGB
    put_entry(out, 0x0111, 4, 1, strip_offset);                // StripOffsets
    if (orientation != 0) put_entry(out, 0x0112, 3, 1, orientation);  // Orientation
    put_entry(out, 0x0115, 3, 1, 3);                           // SamplesPerPixel
    put_entry(out, 0x0116, 4, 1, height);                      // RowsPerStrip
    put_entry(out, 0x0117, 4, 1, static_cast<uint32_t>(strip.size()));  // StripByteCounts
    put_u32(out, 0);  // no next IFD

    REQUIRE(out.size() == bits_offset);
    put_u16(out, 8);  // R bits
    put_u16(out, 8);  // G bits
    put_u16(out, 8);  // B bits
    REQUIRE(out.size() == strip_offset);
    out.insert(out.end(), strip.begin(), strip.end());
    return out;
}

// TIFF LZW: 9-bit codes, MSB first, clear (256) and EOI (257). The strip below
// uses twelve distinct bytes, so every code is a literal and the dictionary
// never grows past 9 bits (which is what keeps this packer simple and still a
// valid LZW stream for libtiff's decoder).
std::vector<uint8_t> lzw_pack(const std::vector<int>& codes) {
    std::vector<uint8_t> out;
    uint32_t bits = 0;
    int pending = 0;
    for (int code : codes) {
        bits = (bits << 9) | static_cast<uint32_t>(code & 0x1FF);
        pending += 9;
        while (pending >= 8) {
            pending -= 8;
            out.push_back(static_cast<uint8_t>((bits >> pending) & 0xFF));
        }
    }
    if (pending > 0) out.push_back(static_cast<uint8_t>((bits << (8 - pending)) & 0xFF));
    return out;
}

std::vector<int> lzw_codes_for(const std::vector<uint8_t>& strip) {
    std::vector<int> codes;
    codes.push_back(256);  // clear
    for (uint8_t byte : strip) codes.push_back(byte);
    codes.push_back(257);  // end of information
    return codes;
}

double channel_mean(const Image& image, int channel) {
    double sum = 0.0;
    const size_t pixels = image.pixel_count();
    for (size_t index = 0; index < pixels; ++index) {
        sum += image.rgb[index * 3 + static_cast<size_t>(channel)];
    }
    return sum / static_cast<double>(pixels);
}

}  // namespace

TEST_CASE("tiff: a hand-built 2x2 uncompressed TIFF decodes to the exact RGB") {
#if defined(SPLAT_HAVE_TIFF)
    // Twelve distinct bytes, so the LZW case below can reuse the same strip.
    const std::vector<uint8_t> strip = {10,  20,  30,  40,  50,  60,
                                        70,  80,  90,  100, 110, 120};
    const std::vector<uint8_t> tiff = make_tiff(1, strip);

    DecodeInfo info;
    const splat::Result<Image> decoded =
        splat::image::decode_photo(tiff.data(), tiff.size(), DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(info.format == splat::ImageFormat::tiff);
    CHECK(decoded.value().width == 2);
    CHECK(decoded.value().height == 2);
    CHECK(decoded.value().rgb == strip);
#else
    WARN("SPLAT_HAVE_TIFF is not defined: libtiff is not in this build");
#endif
}

TEST_CASE("tiff: an LZW TIFF decodes to the same RGB (the codec is linked in)") {
#if defined(SPLAT_HAVE_TIFF)
    const std::vector<uint8_t> strip = {10,  20,  30,  40,  50,  60,
                                        70,  80,  90,  100, 110, 120};
    const std::vector<uint8_t> packed = lzw_pack(lzw_codes_for(strip));
    REQUIRE(packed.size() > strip.size() / 2);  // it really is encoded data
    const std::vector<uint8_t> tiff = make_tiff(5, packed);  // 5 = LZW

    DecodeInfo info;
    const splat::Result<Image> decoded =
        splat::image::decode_photo(tiff.data(), tiff.size(), DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(decoded.value().width == 2);
    CHECK(decoded.value().height == 2);
    CHECK(decoded.value().rgb == strip);
#else
    WARN("SPLAT_HAVE_TIFF is not defined: libtiff is not in this build");
#endif
}

TEST_CASE("tiff: an Orientation tag is applied exactly once") {
#if defined(SPLAT_HAVE_TIFF)
    // 2x1 stored, Orientation 6 (90 clockwise): the result is 1x2. libtiff
    // normalises to the requested orientation itself, so a decoder that also
    // lets decode_photo() apply the tag turns the photo twice and ends at 2x1.
    const std::vector<uint8_t> strip = {10, 20, 30, 40, 50, 60};
    const std::vector<uint8_t> tiff = make_tiff(1, strip, 6, 2, 1);

    DecodeInfo info;
    const splat::Result<Image> decoded =
        splat::image::decode_photo(tiff.data(), tiff.size(), DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(info.source_width == 2);
    CHECK(info.source_height == 1);
    CHECK(info.exif_orientation == 6);
    CHECK(info.width == 1);
    CHECK(info.height == 2);
    CHECK(decoded.value().width == 1);
    CHECK(decoded.value().height == 2);
    // 90 clockwise keeps the first stored pixel at the top.
    CHECK(decoded.value().pixel(0, 0)[0] == 10);
    CHECK(decoded.value().pixel(0, 1)[0] == 40);
#else
    WARN("SPLAT_HAVE_TIFF is not defined: libtiff is not in this build");
#endif
}

TEST_CASE("raw: the extension hint sends a TIFF container to LibRaw, not libtiff") {
#if defined(SPLAT_HAVE_TIFF)
    // .nef/.arw/.orf/.rw2/.pef/.srw are plain TIFF container bytes: content
    // sniffing alone says "tiff", and libtiff would either decode a raw mosaic
    // as an image or fail on its compression. The hint is what routes them.
    const std::vector<uint8_t> strip = {10, 20,  30,  40,  50,  60,
                                        70, 80,  90,  100, 110, 120};
    const std::vector<uint8_t> tiff = make_tiff(1, strip);

    DecodeOptions options;
    options.hint = splat::ImageFormat::raw;
    const splat::Result<Image> result =
        splat::image::decode_photo(tiff.data(), tiff.size(), options, nullptr);
#if defined(SPLAT_HAVE_RAW)
    REQUIRE_FALSE(result.ok());
    const std::string message = result.message();
    CAPTURE(message);
    CHECK(message.find("RAW") != std::string::npos);
#else
    REQUIRE_FALSE(result.ok());
    const std::string message = result.message();
    CHECK(message.find("RAW") != std::string::npos);
#endif
#else
    WARN("SPLAT_HAVE_TIFF is not defined: libtiff is not in this build");
#endif
}

TEST_CASE("tiff: a JPEG-compressed TIFF is refused by name, not by a crash") {
#if defined(SPLAT_HAVE_TIFF)
    const std::vector<uint8_t> strip(32, 0x5A);
    const std::vector<uint8_t> tiff = make_tiff(7, strip);  // 7 = JPEG

    const splat::Result<Image> decoded =
        splat::image::decode_photo(tiff.data(), tiff.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(decoded.ok());
    CAPTURE(decoded.message());
    CHECK(decoded.message().find("JPEG") != std::string::npos);
    CHECK(decoded.message().find("not built in") != std::string::npos);
    CHECK(decoded.message().find("baseline") != std::string::npos);

    // Deflate is refused the same way (the SDK has no zlib).
    const std::vector<uint8_t> deflate = make_tiff(8, strip);
    const splat::Result<Image> refused =
        splat::image::decode_photo(deflate.data(), deflate.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.message().find("Deflate") != std::string::npos);

    // Truncated bytes fail as a decode error, never a crash.
    const std::vector<uint8_t> plain = make_tiff(1, strip);
    const splat::Result<Image> truncated =
        splat::image::decode_photo(plain.data(), 6, DecodeOptions{}, nullptr);
    REQUIRE_FALSE(truncated.ok());
#else
    WARN("SPLAT_HAVE_TIFF is not defined: libtiff is not in this build");
#endif
}

TEST_CASE("webp: the 16x16 fixture matches its source PNG's channel means") {
#if defined(SPLAT_HAVE_WEBP)
    const Path webp_path = fixture("tiny.webp");
    const Path png_path = fixture("tiny_source.png");
    INFO("fixture " << webp_path.string());
    REQUIRE(std::filesystem::exists(webp_path));
    REQUIRE(std::filesystem::exists(png_path));
    // The fixture is committed small on purpose; a regenerated file that grew
    // means the wrong cwebp flags were used.
    CHECK(std::filesystem::file_size(webp_path) <= 1024);

    DecodeInfo info;
    const splat::Result<Image> webp =
        splat::image::decode_photo(webp_path, DecodeOptions{}, &info);
    REQUIRE_MESSAGE(webp.ok(), webp.message());
    CHECK(info.format == splat::ImageFormat::webp);
    CHECK(webp.value().width == 16);
    CHECK(webp.value().height == 16);

    const splat::Result<Image> source = splat::image::decode_photo(png_path, DecodeOptions{}, nullptr);
    REQUIRE_MESSAGE(source.ok(), source.message());
    REQUIRE(source.value().width == 16);
    REQUIRE(source.value().height == 16);

    for (int channel = 0; channel < 3; ++channel) {
        CAPTURE(channel);
        const double expected = channel_mean(source.value(), channel);
        const double actual = channel_mean(webp.value(), channel);
        INFO("mean " << actual << " vs " << expected);
        CHECK(std::abs(actual - expected) <= 2.0);
    }
#else
    WARN("SPLAT_HAVE_WEBP is not defined: libwebp is not in this build");
#endif
}
