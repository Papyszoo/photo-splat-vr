// RAW through LibRaw (L1-C).
//
// The always-on cases are byte vectors: garbage, a truncated DNG header, and a
// .dng whose content is a PNG (which the dispatcher must refuse by name). The
// camera file is external ($SPLAT_DATA_DIR/golden/raw/sample.dng, never
// committed); when it is absent those checks skip, as the repo's rule says.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/path.h"
#include "data_dir.h"
#include "test_util.h"
#if defined(SPLAT_HAVE_RAW)
#include <libraw/libraw.h>
#endif
#include "image/decode.h"
#include "jobs/image_io.h"

using splat::Image;
using splat::Path;
using splat::image::DecodeInfo;
using splat::image::DecodeOptions;

namespace {

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

// Little-endian TIFF header + an IFD0 that says DNG, then nothing: the strip
// data the tags point at is missing, so this is a truncated DNG.
std::vector<uint8_t> truncated_dng() {
    std::vector<uint8_t> bytes = {'I', 'I'};
    put_u16(bytes, 42);
    put_u32(bytes, 8);  // IFD0 at 8
    put_u16(bytes, 3);  // three entries
    // DNGVersion = 1.4.0.0
    put_u16(bytes, 0xC612);
    put_u16(bytes, 1);  // BYTE
    put_u32(bytes, 4);
    bytes.push_back(1);
    bytes.push_back(4);
    bytes.push_back(0);
    bytes.push_back(0);
    // ImageWidth / ImageLength pointing at a strip that is not there.
    put_u16(bytes, 0x0100);
    put_u16(bytes, 4);  // LONG
    put_u32(bytes, 1);
    put_u32(bytes, 64);
    put_u16(bytes, 0x0101);
    put_u16(bytes, 4);
    put_u32(bytes, 1);
    put_u32(bytes, 48);
    put_u32(bytes, 0);  // no next IFD
    return bytes;
}

// Where the camera file may be: `$SPLAT_RAW_SAMPLE` first, then the repo's
// documented golden paths, then the CC0 downloads kept outside the repo in
// runs/v0.2-library/raw-samples/ (raw-samples/README.md records each file's
// source URL, sha256 and licence). A RAW file is parsed by LibRaw only and is
// never executed, so an untrusted download is safe to decode.
std::vector<std::filesystem::path> camera_raw_paths() {
    std::vector<std::filesystem::path> found;
    if (const char* given = std::getenv("SPLAT_RAW_SAMPLE")) {
        if (given[0] != '\0' && std::filesystem::exists(given)) found.emplace_back(given);
    }
    for (const char* relative :
         {"golden/raw/sample.dng", "golden/raw/sample.CR2", "golden/raw/sample.cr2"}) {
        const std::filesystem::path candidate = splat_test::data_path(relative);
        if (std::filesystem::exists(candidate)) found.push_back(candidate);
    }
    const std::filesystem::path samples = splat_test::data_path("runs/v0.2-library/raw-samples");
    std::error_code error;
    std::vector<std::filesystem::path> downloaded;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(samples, error)) {
        if (!entry.is_regular_file()) continue;
        std::string extension = entry.path().extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension == ".dng" || extension == ".cr2") downloaded.push_back(entry.path());
    }
    // Sorted, so the run order (and a failure's name) does not depend on the
    // file system.
    std::sort(downloaded.begin(), downloaded.end());
    found.insert(found.end(), downloaded.begin(), downloaded.end());
    return found;
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

TEST_CASE("raw: garbage bytes and a truncated DNG header are refused, not crashed") {
    // 4 KB of garbage: not a container at all.
    std::vector<uint8_t> garbage(4096);
    for (size_t index = 0; index < garbage.size(); ++index) {
        garbage[index] = static_cast<uint8_t>((index * 37 + 11) & 0xFF);
    }
    const splat::Result<Image> from_garbage =
        splat::image::decode_photo(garbage.data(), garbage.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(from_garbage.ok());
    CAPTURE(from_garbage.message());
    CHECK(from_garbage.message().find("cannot decode") != std::string::npos);

    // A DNG header whose pixel data is missing.
    const std::vector<uint8_t> dng = truncated_dng();
    const splat::Result<Image> from_dng =
        splat::image::decode_photo(dng.data(), dng.size(), DecodeOptions{}, nullptr);
    REQUIRE_FALSE(from_dng.ok());
    CAPTURE(from_dng.message());
    CHECK_FALSE(from_dng.message().empty());

#if defined(SPLAT_HAVE_RAW)
    // The sniff really does see it as RAW, so the refusal came from LibRaw.
    CHECK(splat::sniff_image_format(dng.data(), dng.size()) == splat::ImageFormat::raw);
#else
    WARN("SPLAT_HAVE_RAW is not defined: LibRaw is not in this build");
#endif
}

TEST_CASE("raw: a .dng whose content is a PNG is refused by name") {
    splat_test::TempDir dir("raw-not-raw");
    // A real 2x2 PNG written under a .dng name.
    const uint8_t rgba[2 * 2 * 4] = {
        255, 0,   0,   255, 0,   255, 0,   255,
        0,   0,   255, 255, 255, 255, 0,   255,
    };
    const splat::Result<std::vector<uint8_t>> png = splat::encode_png(rgba, 2, 2);
    REQUIRE_MESSAGE(png.ok(), png.message());
    const Path path = dir.path() / "photo.dng";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(png.value().data()),
              static_cast<std::streamsize>(png.value().size()));
    out.close();

    const splat::Result<Image> decoded =
        splat::image::decode_photo(path, DecodeOptions{}, nullptr);
    REQUIRE_FALSE(decoded.ok());
    CAPTURE(decoded.message());
    CHECK(decoded.message().find("not a RAW file") != std::string::npos);
    CHECK(decoded.message().find("png") != std::string::npos);
}

TEST_CASE("raw: a camera file decodes when an external sample is present") {
    const std::vector<std::filesystem::path> samples = camera_raw_paths();
    if (samples.empty()) {
        MESSAGE("no camera RAW sample under " << splat_test::data_dir().string()
                                              << ": the camera-file check is skipped");
        return;
    }
#if defined(SPLAT_HAVE_RAW)
    for (const std::filesystem::path& sample : samples) {
        SUBCASE(sample.filename().string().c_str()) {
            MESSAGE("camera RAW sample: " << sample.string());
            DecodeInfo info;
            const splat::Result<Image> decoded =
                splat::image::decode_photo(Path(sample.string()), DecodeOptions{}, &info);
            REQUIRE_MESSAGE(decoded.ok(), decoded.message());
            CHECK(decoded.value().width > 0);
            CHECK(decoded.value().height > 0);
            CHECK(info.width == decoded.value().width);
            CHECK(info.height == decoded.value().height);
            CHECK(info.format == splat::ImageFormat::raw);
            MESSAGE("decoded " << info.width << "x" << info.height << " preview=" << info.raw_preview
                               << " note=" << info.note);
            // Either the preview ran or the half-size demosaic did; the note
            // says which, and A1's pass criterion is that it says one of them.
            if (!info.raw_preview) {
                CHECK(info.note.find("half-size") != std::string::npos);
            }

            // Compare against the embedded preview's own mean when there is one.
            libraw_data_t* raw = libraw_init(0);
            REQUIRE(raw != nullptr);
            const std::vector<uint8_t> bytes = [&sample] {
                std::ifstream file(sample, std::ios::binary);
                return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
            }();
            REQUIRE(libraw_open_buffer(raw, const_cast<uint8_t*>(bytes.data()), bytes.size()) ==
                    LIBRAW_SUCCESS);
            if (libraw_unpack_thumb(raw) == LIBRAW_SUCCESS &&
                raw->thumbnail.tformat == LIBRAW_THUMBNAIL_JPEG && raw->thumbnail.tlength > 0) {
                const splat::Result<Image> preview = splat::image::decode_stb_rgb(
                    reinterpret_cast<const uint8_t*>(raw->thumbnail.thumb),
                    raw->thumbnail.tlength);
                REQUIRE_MESSAGE(preview.ok(), preview.message());
                for (int channel = 0; channel < 3; ++channel) {
                    CAPTURE(channel);
                    CHECK(std::abs(channel_mean(decoded.value(), channel) -
                                   channel_mean(preview.value(), channel)) <= 25.0);
                }
            } else {
                MESSAGE("the sample has no JPEG preview: the mean comparison is skipped");
            }
            libraw_close(raw);
        }
    }
#else
    WARN("SPLAT_HAVE_RAW is not defined: LibRaw is not in this build");
#endif
}
