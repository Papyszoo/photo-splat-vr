// The one photo format list (core/image/format.h): extensions, and the magic
// byte vectors every entry point sniffs with.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "image/format.h"

using splat::ImageFormat;
using splat::Path;

namespace {

const std::vector<uint8_t>& bytes_of(const std::string& text) {
    static thread_local std::vector<uint8_t> bytes;
    bytes.assign(text.begin(), text.end());
    return bytes;
}

// 16-byte header vectors: enough for the sniffer, nothing else.
std::vector<uint8_t> header(const std::string& magic, size_t size = 16) {
    std::vector<uint8_t> data(magic.begin(), magic.end());
    data.resize(size, 0);
    return data;
}

}  // namespace

TEST_CASE("image format: the photo extensions, case-insensitively") {
    const std::vector<std::string>& extensions = splat::photo_extensions();
    for (const char* expected : {".jpg", ".jpeg", ".png", ".webp", ".heic", ".heif", ".avif",
                                 ".tif", ".tiff", ".bmp", ".gif", ".tga", ".dng", ".cr2",
                                 ".cr3", ".nef", ".arw", ".raf", ".orf", ".rw2", ".pef", ".srw"}) {
        CAPTURE(expected);
        CHECK(std::find(extensions.begin(), extensions.end(), expected) != extensions.end());
    }
    CHECK(extensions.size() == 22);

    CHECK(splat::is_photo_file(Path("x.JPG")));
    CHECK(splat::is_photo_file(Path("x.Jpeg")));
    CHECK(splat::is_photo_file(Path("x.heic")));
    CHECK(splat::is_photo_file(Path("x.dng")));
    CHECK(splat::is_photo_file(Path("x.tga")));
    CHECK(splat::is_photo_file(Path("/data/inbox/IMG_0001.PNG")));
    CHECK_FALSE(splat::is_photo_file(Path("x.txt")));
    CHECK_FALSE(splat::is_photo_file(Path("x.psplat")));
    CHECK_FALSE(splat::is_photo_file(Path("x")));
    CHECK_FALSE(splat::is_photo_file(Path("x.jpg.tmp")));

    CHECK(splat::image_format_from_extension(Path("a.jpg")) == ImageFormat::jpeg);
    CHECK(splat::image_format_from_extension(Path("a.JPEG")) == ImageFormat::jpeg);
    CHECK(splat::image_format_from_extension(Path("a.png")) == ImageFormat::png);
    CHECK(splat::image_format_from_extension(Path("a.webp")) == ImageFormat::webp);
    CHECK(splat::image_format_from_extension(Path("a.heic")) == ImageFormat::heif);
    CHECK(splat::image_format_from_extension(Path("a.heif")) == ImageFormat::heif);
    CHECK(splat::image_format_from_extension(Path("a.avif")) == ImageFormat::avif);
    CHECK(splat::image_format_from_extension(Path("a.tif")) == ImageFormat::tiff);
    CHECK(splat::image_format_from_extension(Path("a.tiff")) == ImageFormat::tiff);
    CHECK(splat::image_format_from_extension(Path("a.bmp")) == ImageFormat::bmp);
    CHECK(splat::image_format_from_extension(Path("a.gif")) == ImageFormat::gif);
    CHECK(splat::image_format_from_extension(Path("a.tga")) == ImageFormat::tga);
    CHECK(splat::image_format_from_extension(Path("a.dng")) == ImageFormat::raw);
    CHECK(splat::image_format_from_extension(Path("a.cr2")) == ImageFormat::raw);
    CHECK(splat::image_format_from_extension(Path("a.txt")) == ImageFormat::unknown);

    CHECK(std::string(splat::image_format_name(ImageFormat::jpeg)) == "jpeg");
    CHECK(std::string(splat::image_format_name(ImageFormat::heif)) == "heif");
    CHECK(std::string(splat::image_format_name(ImageFormat::unknown)) == "unknown");
}

TEST_CASE("image format: magic bytes decide the format") {
    // JPEG: SOI plus the first APPn marker byte.
    const std::vector<uint8_t> jpeg = header("\xFF\xD8\xFF\xE0");
    CHECK(splat::sniff_image_format(jpeg.data(), jpeg.size()) == ImageFormat::jpeg);

    const std::vector<uint8_t> png = header("\x89PNG\r\n\x1A\n");
    CHECK(splat::sniff_image_format(png.data(), png.size()) == ImageFormat::png);

    const std::vector<uint8_t> gif = header("GIF89a");
    CHECK(splat::sniff_image_format(gif.data(), gif.size()) == ImageFormat::gif);

    const std::vector<uint8_t> bmp = header("BM");
    CHECK(splat::sniff_image_format(bmp.data(), bmp.size()) == ImageFormat::bmp);

    // A little-endian TIFF whose IFD0 is empty: plain TIFF.
    std::vector<uint8_t> tiff = header("II\x2A\x00");
    tiff[4] = 8;  // IFD0 at offset 8
    tiff[8] = 0;  // no entries
    tiff[9] = 0;
    tiff[10] = 0;  // no next IFD
    tiff[11] = 0;
    CHECK(splat::sniff_image_format(tiff.data(), tiff.size()) == ImageFormat::tiff);

    // The same header with a DNGVersion tag in IFD0 is RAW.
    std::vector<uint8_t> dng = tiff;
    dng[8] = 1;  // one entry
    dng.resize(32, 0);
    dng[10] = 0x12;  // tag 0xC612, little-endian
    dng[11] = 0xC6;
    dng[12] = 1;   // type BYTE
    dng[14] = 4;   // count 4
    dng[18] = 1;   // value: DNG 1.1.0.0
    dng[19] = 1;
    dng[22] = 0;  // next IFD
    CHECK(splat::sniff_image_format(dng.data(), dng.size()) == ImageFormat::raw);

    // RIFF + WEBP.
    std::vector<uint8_t> webp = header("RIFF");
    webp[4] = 0x24;
    webp[8] = 'W';
    webp[9] = 'E';
    webp[10] = 'B';
    webp[11] = 'P';
    CHECK(splat::sniff_image_format(webp.data(), webp.size()) == ImageFormat::webp);

    // ISO-BMFF: size, "ftyp", major brand. Built byte by byte because the
    // box size starts with NULs.
    const auto isobmff = [](const char brand[5]) {
        std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p'};
        data.insert(data.end(), brand, brand + 4);
        data.resize(16, 0);
        return data;
    };
    const std::vector<uint8_t> heic = isobmff("heic");
    CHECK(splat::sniff_image_format(heic.data(), heic.size()) == ImageFormat::heif);
    const std::vector<uint8_t> heif = isobmff("mif1");
    CHECK(splat::sniff_image_format(heif.data(), heif.size()) == ImageFormat::heif);
    const std::vector<uint8_t> avif = isobmff("avif");
    CHECK(splat::sniff_image_format(avif.data(), avif.size()) == ImageFormat::avif);

    // Not images at all.
    const std::vector<uint8_t> text = header("hello, world");
    CHECK(splat::sniff_image_format(text.data(), text.size()) == ImageFormat::unknown);
    // An MP4 brand is not an image brand.
    const std::vector<uint8_t> mp4 = isobmff("mp42");
    CHECK(splat::sniff_image_format(mp4.data(), mp4.size()) == ImageFormat::unknown);
    // Short and null buffers are unknown, not a read past the end.
    CHECK(splat::sniff_image_format(nullptr, 0) == ImageFormat::unknown);
    const std::vector<uint8_t> one = {0xFF};
    CHECK(splat::sniff_image_format(one.data(), one.size()) == ImageFormat::unknown);
}
