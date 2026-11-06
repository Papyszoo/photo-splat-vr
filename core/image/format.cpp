// SPDX-License-Identifier: GPL-3.0-or-later

#include "image/format.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "common/tiff_tags.h"

namespace splat {
namespace {

constexpr uint16_t kTagDngVersion = 0xC612;

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool starts_with(const uint8_t* data, size_t size, const char* magic, size_t length) {
    return size >= length && std::memcmp(data, magic, length) == 0;
}

// A TIFF whose IFD0 carries DNGVersion is a DNG, which is a RAW file even
// though its header is plain TIFF. Canonical CR2 files say "CR\x02\x00" at
// offset 8. Everything else stays tiff.
bool tiff_is_raw(const uint8_t* data, size_t size) {
    if (size >= 11 && std::memcmp(data + 8, "CR\x02", 3) == 0) return true;
    return tiff::has_ifd0_tag(data, size, kTagDngVersion);
}

// ISO-BMFF: a size, then "ftyp", then the major brand. HEIF and AVIF share the
// container; only the brand tells them apart.
ImageFormat sniff_isobmff(const uint8_t* data, size_t size) {
    if (size < 12) return ImageFormat::unknown;
    if (std::memcmp(data + 4, "ftyp", 4) != 0) return ImageFormat::unknown;
    const char* brand_bytes = reinterpret_cast<const char*>(data + 8);
    // Not "major": the PS5 SDK's <sys/types.h> defines major() as a macro.
    const std::string brand(brand_bytes, 4);
    if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" ||
        brand == "mif1" || brand == "msf1" || brand == "heim" || brand == "heis") {
        return ImageFormat::heif;
    }
    if (brand == "avif" || brand == "avis") return ImageFormat::avif;
    return ImageFormat::unknown;
}

}  // namespace

const char* image_format_name(ImageFormat format) {
    switch (format) {
        case ImageFormat::unknown: return "unknown";
        case ImageFormat::jpeg: return "jpeg";
        case ImageFormat::png: return "png";
        case ImageFormat::webp: return "webp";
        case ImageFormat::heif: return "heif";
        case ImageFormat::avif: return "avif";
        case ImageFormat::tiff: return "tiff";
        case ImageFormat::bmp: return "bmp";
        case ImageFormat::gif: return "gif";
        case ImageFormat::tga: return "tga";
        case ImageFormat::raw: return "raw";
    }
    return "unknown";
}

const std::vector<std::string>& photo_extensions() {
    static const std::vector<std::string> extensions = {
        ".jpg", ".jpeg", ".png", ".webp", ".heic", ".heif", ".avif", ".tif",  ".tiff",
        ".bmp", ".gif",  ".tga", ".dng",  ".cr2",  ".cr3",  ".nef",  ".arw",  ".raf",
        ".orf", ".rw2",  ".pef", ".srw",
    };
    return extensions;
}

bool is_photo_file(const Path& path) {
    return image_format_from_extension(path) != ImageFormat::unknown;
}

ImageFormat image_format_from_extension(const Path& path) {
    const std::string extension = lowercase(path.extension().string());
    if (extension.empty()) return ImageFormat::unknown;
    if (std::find(photo_extensions().begin(), photo_extensions().end(), extension) ==
        photo_extensions().end()) {
        return ImageFormat::unknown;
    }
    if (extension == ".jpg" || extension == ".jpeg") return ImageFormat::jpeg;
    if (extension == ".png") return ImageFormat::png;
    if (extension == ".webp") return ImageFormat::webp;
    if (extension == ".heic" || extension == ".heif") return ImageFormat::heif;
    if (extension == ".avif") return ImageFormat::avif;
    if (extension == ".tif" || extension == ".tiff") return ImageFormat::tiff;
    if (extension == ".bmp") return ImageFormat::bmp;
    if (extension == ".gif") return ImageFormat::gif;
    if (extension == ".tga") return ImageFormat::tga;
    return ImageFormat::raw;
}

ImageFormat sniff_image_format(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 2) return ImageFormat::unknown;
    if (starts_with(data, size, "\xFF\xD8\xFF", 3)) return ImageFormat::jpeg;
    // The full PNG signature, not just the first four bytes: a truncated
    // "\x89PNG" is not a PNG and the upload store refuses it.
    if (starts_with(data, size, "\x89PNG\r\n\x1A\n", 8)) return ImageFormat::png;
    if (starts_with(data, size, "GIF8", 4)) return ImageFormat::gif;
    if (starts_with(data, size, "BM", 2)) return ImageFormat::bmp;
    if ((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M')) {
        if (size < 4 || ((data[0] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
                         (data[0] == 'M' && data[2] == 0x00 && data[3] == 0x2A))) {
            return tiff_is_raw(data, size) ? ImageFormat::raw : ImageFormat::tiff;
        }
        return ImageFormat::unknown;
    }
    if (starts_with(data, size, "RIFF", 4) && size >= 12 &&
        std::memcmp(data + 8, "WEBP", 4) == 0) {
        return ImageFormat::webp;
    }
    return sniff_isobmff(data, size);
}

}  // namespace splat
