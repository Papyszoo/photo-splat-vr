// The dimension and size guards run on the header, before anything is
// allocated, and a normal small photo still decodes.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "image/decode.h"
#include "jobs/image_io.h"
#include "test_util.h"

using splat::image::DecodeInfo;
using splat::image::DecodeOptions;
using splat::image::decode_photo;

namespace {

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void append_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

// One ISO-BMFF box with the given payload (size is 32-bit, which is all this
// hand-built header needs).
std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    append_be32(out, static_cast<uint32_t>(8 + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> ispe(uint32_t width, uint32_t height) {
    std::vector<uint8_t> payload(4, 0);  // version + flags
    append_be32(payload, width);
    append_be32(payload, height);
    return box("ispe", payload);
}

// A HEIC header whose ipco holds a 320x240 thumbnail ispe *before* the
// primary item's 4000x3000 one. The guard must read the primary's, or a
// thumbnail would let the real image past the limit.
std::vector<uint8_t> heic_header_with_thumbnail_first() {
    std::vector<uint8_t> ftyp(4, 0);
    ftyp[0] = 'h';
    ftyp[1] = 'e';
    ftyp[2] = 'i';
    ftyp[3] = 'c';
    append_be32(ftyp, 0);  // minor version

    std::vector<uint8_t> pitm(4, 0);  // version + flags
    append_be16(pitm, 1);             // primary item id 1

    std::vector<uint8_t> ipco_payload = ispe(320, 240);
    const std::vector<uint8_t> primary_ispe = ispe(4000, 3000);
    ipco_payload.insert(ipco_payload.end(), primary_ispe.begin(), primary_ispe.end());
    const std::vector<uint8_t> ipco = box("ipco", ipco_payload);

    std::vector<uint8_t> ipma_payload(4, 0);
    append_be32(ipma_payload, 1);  // one entry
    append_be16(ipma_payload, 1);  // item 1
    ipma_payload.push_back(2);     // two associations
    ipma_payload.push_back(1);     // property 1 (the thumbnail ispe)
    ipma_payload.push_back(2);     // property 2 (the primary ispe)
    const std::vector<uint8_t> ipma = box("ipma", ipma_payload);

    std::vector<uint8_t> iprp_payload = ipco;
    iprp_payload.insert(iprp_payload.end(), ipma.begin(), ipma.end());
    const std::vector<uint8_t> iprp = box("iprp", iprp_payload);

    std::vector<uint8_t> meta_payload(4, 0);  // meta is a FullBox
    const std::vector<uint8_t> pitm_box = box("pitm", pitm);
    meta_payload.insert(meta_payload.end(), pitm_box.begin(), pitm_box.end());
    meta_payload.insert(meta_payload.end(), iprp.begin(), iprp.end());
    const std::vector<uint8_t> meta = box("meta", meta_payload);

    std::vector<uint8_t> bytes = box("ftyp", ftyp);
    bytes.insert(bytes.end(), meta.begin(), meta.end());
    return bytes;
}

// PNG signature, IHDR chunk with the given size, no image data: the guard reads
// the header and refuses before a decoder ever sees it.
std::vector<uint8_t> png_header(uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    append_be32(bytes, 13);  // IHDR length
    const char type[4] = {'I', 'H', 'D', 'R'};
    bytes.insert(bytes.end(), type, type + 4);
    append_be32(bytes, width);
    append_be32(bytes, height);
    bytes.push_back(8);  // bit depth
    bytes.push_back(2);  // colour type: RGB
    bytes.push_back(0);  // compression
    bytes.push_back(0);  // filter
    bytes.push_back(0);  // interlace
    // The CRC is not read before the refusal, but a real chunk has one.
    append_be32(bytes, 0);
    return bytes;
}

}  // namespace

TEST_CASE("limits: a 70000x1000 PNG is refused by its header, without decoding") {
    const std::vector<uint8_t> png = png_header(70000, 1000);
    const splat::Result<splat::Image> result =
        decode_photo(png.data(), png.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(result.ok());
    CAPTURE(result.message());
    CHECK(result.message().find("70.0 MP") != std::string::npos);
    CHECK(result.message().find("64 MP") != std::string::npos);
    CHECK(result.message().find("70000x1000") != std::string::npos);
}

TEST_CASE("limits: a body over 512 MB is refused before any allocation") {
    // The pointer is never dereferenced: the size check comes first, so a
    // claimed 600 MB is safe to hand over.
    static const uint8_t tiny[8] = {0};
    const splat::Result<splat::Image> result =
        decode_photo(tiny, 600ull * 1024 * 1024, DecodeOptions{}, nullptr);
    REQUIRE_FALSE(result.ok());
    CAPTURE(result.message());
    CHECK(result.message().find("512 MB") != std::string::npos);

    // The same holds through the options: a smaller cap refuses earlier.
    DecodeOptions strict;
    strict.max_bytes = 4;
    const splat::Result<splat::Image> small =
        decode_photo(tiny, sizeof(tiny), strict, nullptr);
    REQUIRE_FALSE(small.ok());
    CHECK(small.message().find("4 bytes") != std::string::npos);
}

TEST_CASE("limits: a 2x2 PNG decodes and reports its size") {
    const uint8_t rgba[2 * 2 * 4] = {
        255, 0,   0,   255, 0,   255, 0,   255,
        0,   0,   255, 255, 255, 255, 0,   255,
    };
    const splat::Result<std::vector<uint8_t>> encoded = splat::encode_png(rgba, 2, 2);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());

    DecodeInfo info;
    const splat::Result<splat::Image> decoded =
        decode_photo(encoded.value().data(), encoded.value().size(), DecodeOptions{}, &info);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    CHECK(info.width == 2);
    CHECK(info.height == 2);
    CHECK(info.source_width == 2);
    CHECK(info.source_height == 2);
    CHECK(info.format == splat::ImageFormat::png);
    CHECK(info.exif_orientation == 1);
    CHECK(decoded.value().width == 2);
    CHECK(decoded.value().height == 2);
    CHECK(decoded.value().pixel(0, 0)[0] == 255);
    CHECK(decoded.value().pixel(0, 0)[1] == 0);
    CHECK(decoded.value().pixel(1, 1)[0] == 255);
    CHECK(decoded.value().pixel(1, 1)[1] == 255);
}

TEST_CASE("limits: the HEIF guard reads the primary item's ispe, not the first one") {
    const std::vector<uint8_t> heic = heic_header_with_thumbnail_first();
    const splat::image::PhotoSize probed =
        splat::image::probe_photo_size(heic.data(), heic.size());
    CHECK(probed.format == splat::ImageFormat::heif);
    CHECK(probed.width == 4000);
    CHECK(probed.height == 3000);
}

TEST_CASE("limits: read_file_bytes refuses an oversized file before reading it") {
    splat_test::TempDir dir{"bytes-limit"};
    const splat::Path path = dir.path() / "photo.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> bytes(1024, 0x5A);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(static_cast<bool>(out));
    }
    // The file size is checked first: a cap of 100 bytes refuses a 1 KiB file.
    const splat::Result<std::vector<uint8_t>> refused =
        splat::read_file_bytes(path, 100);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.message().find("100 bytes") != std::string::npos);
    // Under the cap the same read succeeds (the guard is a cap, not a filter).
    const splat::Result<std::vector<uint8_t>> read = splat::read_file_bytes(path, 2048);
    REQUIRE(read.ok());
    CHECK(read.value().size() == 1024);
    CHECK(splat::read_file_bytes(path).ok());  // 0 = no limit
}

TEST_CASE("limits: the header probe reads each format's own field") {
    const std::vector<uint8_t> png = png_header(70000, 1000);
    const splat::image::PhotoSize probed = splat::image::probe_photo_size(png.data(), png.size());
    CHECK(probed.format == splat::ImageFormat::png);
    CHECK(probed.width == 70000);
    CHECK(probed.height == 1000);

    // A GIF logical screen descriptor.
    std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 0x20, 0x00, 0x10, 0x00};
    gif.resize(16, 0);
    const splat::image::PhotoSize gif_size = splat::image::probe_photo_size(gif.data(), gif.size());
    CHECK(gif_size.width == 32);
    CHECK(gif_size.height == 16);

    // A BMP info header width/height.
    std::vector<uint8_t> bmp(64, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[18] = 0xC8;  // 200
    bmp[22] = 0x64;  // 100
    const splat::image::PhotoSize bmp_size = splat::image::probe_photo_size(bmp.data(), bmp.size());
    CHECK(bmp_size.width == 200);
    CHECK(bmp_size.height == 100);

    // A WebP VP8X canvas size is stored minus one.
    std::vector<uint8_t> webp(32, 0);
    std::memcpy(webp.data(), "RIFF", 4);
    std::memcpy(webp.data() + 8, "WEBP", 4);
    std::memcpy(webp.data() + 12, "VP8X", 4);
    webp[24] = 0x1F;  // width - 1 = 31
    webp[27] = 0x0F;  // height - 1 = 15
    const splat::image::PhotoSize webp_size =
        splat::image::probe_photo_size(webp.data(), webp.size());
    CHECK(webp_size.width == 32);
    CHECK(webp_size.height == 16);
}
