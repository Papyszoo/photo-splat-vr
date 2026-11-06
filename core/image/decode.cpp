// SPDX-License-Identifier: GPL-3.0-or-later

#include "image/decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/exif.h"
#include "common/tiff_tags.h"
#include "image/orientation.h"

// Static implementations keep the stb symbols local to this translation unit;
// core/jobs/image_io.cpp includes the same headers for its own purposes.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace splat {
namespace image {
namespace {

uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint16_t read_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_u24_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}

uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t read_u32_le(const uint8_t* p) {
    return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

// SOF markers carry the frame size. C4/C8/CC are DHT/JPG/DAC, not frames.
bool is_sof(uint8_t marker) {
    return (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
           (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
}

PhotoSize probe_jpeg(const uint8_t* data, size_t size) {
    PhotoSize result;
    size_t pos = 2;
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) {
            ++pos;
            continue;
        }
        while (pos < size && data[pos] == 0xFF) ++pos;
        if (pos >= size) break;
        const uint8_t marker = data[pos++];
        if (marker == 0xD9 || marker == 0xDA) break;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (pos + 2 > size) break;
        const uint16_t length = read_u16_be(data + pos);
        if (length < 2 || pos + length > size) break;
        if (is_sof(marker) && length >= 7) {
            result.height = read_u16_be(data + pos + 3);
            result.width = read_u16_be(data + pos + 5);
            break;
        }
        pos += length;
    }
    return result;
}

PhotoSize probe_tiff(const uint8_t* data, size_t size) {
    PhotoSize result;
    if (!tiff::is_tiff(data, size)) return result;
    const bool big_endian = data[0] == 'M';
    const size_t ifd0 = tiff::ifd0_offset(data, size);
    const uint16_t count = tiff::entry_count(data, size, big_endian, ifd0);
    for (uint16_t i = 0; i < count; ++i) {
        tiff::Entry entry;
        if (!tiff::entry_at(data, size, big_endian, ifd0, i, &entry)) return result;
        if (entry.tag != 0x0100 && entry.tag != 0x0101) continue;
        int value = 0;
        if (entry.type == 3) {
            value = tiff::read_u16(entry.value, big_endian);
        } else if (entry.type == 4) {
            value = static_cast<int>(tiff::read_u32(entry.value, big_endian));
        } else {
            continue;
        }
        if (entry.tag == 0x0100) {
            result.width = value;
        } else {
            result.height = value;
        }
    }
    return result;
}

PhotoSize probe_webp(const uint8_t* data, size_t size) {
    PhotoSize result;
    if (size < 20) return result;
    const std::string fourcc(reinterpret_cast<const char*>(data + 12), 4);
    if (fourcc == "VP8X") {
        if (size < 30) return result;
        result.width = static_cast<int>(read_u24_le(data + 24)) + 1;
        result.height = static_cast<int>(read_u24_le(data + 27)) + 1;
    } else if (fourcc == "VP8L") {
        if (size < 25) return result;
        const uint32_t bits = read_u32_le(data + 21);
        result.width = static_cast<int>(bits & 0x3FFF) + 1;
        result.height = static_cast<int>((bits >> 14) & 0x3FFF) + 1;
    } else if (fourcc == "VP8 ") {
        // 3-byte frame tag, then the 3-byte keyframe start code, then 14-bit
        // width and height.
        if (size < 30 || data[23] != 0x9D || data[24] != 0x01 || data[25] != 0x2A) return result;
        result.width = static_cast<int>(read_u16_le(data + 26) & 0x3FFF);
        result.height = static_cast<int>(read_u16_le(data + 28) & 0x3FFF);
    }
    return result;
}

// One ISO-BMFF box: its type, where its payload starts and where the next box
// starts. `size` 1 means a 64-bit largesize follows the type; 0 means "to the
// end of the file", as the format allows.
struct Box {
    char type[5] = {0, 0, 0, 0, 0};
    size_t payload = 0;
    size_t end = 0;
};

bool next_box(const uint8_t* data, size_t size, size_t offset, Box* box) {
    if (offset + 8 > size) return false;
    uint64_t length = read_u32_be(data + offset);
    std::memcpy(box->type, data + offset + 4, 4);
    box->type[4] = '\0';
    size_t header = 8;
    if (length == 1) {
        if (offset + 16 > size) return false;
        length = (static_cast<uint64_t>(read_u32_be(data + offset + 8)) << 32) |
                 read_u32_be(data + offset + 12);
        header = 16;
    } else if (length == 0) {
        length = size - offset;
    }
    if (length < header || length > size - offset) return false;
    box->payload = offset + header;
    box->end = offset + static_cast<size_t>(length);
    return true;
}

// The ispe payload is version+flags, then width and height, both big-endian.
bool read_ispe(const uint8_t* data, size_t size, const Box& box, PhotoSize* out) {
    if (box.payload + 12 > box.end || box.end > size) return false;
    const uint32_t width = read_u32_be(data + box.payload + 4);
    const uint32_t height = read_u32_be(data + box.payload + 8);
    if (width == 0 || height == 0 || width >= 0x7FFFFFFFu || height >= 0x7FFFFFFFu) return false;
    out->width = static_cast<int>(width);
    out->height = static_cast<int>(height);
    return true;
}

// Walks the meta box to the primary item's `ispe`. Scanning the file for the
// first literal "ispe" is wrong twice over: a thumbnail item has its own ispe
// (so the real image would escape the limit), and Exif/XMP payloads can contain
// the four bytes. Falls back to the largest ispe property in ipco when the
// item association is unreadable - the guard must over-estimate, never under.
PhotoSize probe_isobmff(const uint8_t* data, size_t size) {
    PhotoSize result;
    Box meta;
    size_t offset = 0;
    while (next_box(data, size, offset, &meta)) {
        if (std::memcmp(meta.type, "meta", 4) == 0) break;
        offset = meta.end;
    }
    if (std::memcmp(meta.type, "meta", 4) != 0) return result;

    // meta is a FullBox: 4 bytes of version+flags precede its children.
    size_t child = meta.payload + 4;
    uint32_t primary_id = 0;
    PhotoSize largest;
    std::vector<PhotoSize> properties;  // index + 1 is the property index
    bool has_ispe = false;
    size_t iprp_start = 0;
    size_t iprp_end = 0;
    Box box;
    while (next_box(data, size, child, &box) && box.end <= meta.end) {
        child = box.end;
        if (std::memcmp(box.type, "pitm", 4) == 0) {
            if (box.payload + 6 <= box.end) {
                const uint8_t version = data[box.payload];
                primary_id = version == 0 ? read_u16_be(data + box.payload + 4)
                                          : read_u32_be(data + box.payload + 4);
            }
        } else if (std::memcmp(box.type, "iprp", 4) == 0) {
            iprp_start = box.payload;
            iprp_end = box.end;
        }
    }
    if (iprp_start == 0) return result;

    size_t ipco_start = 0;
    size_t ipco_end = 0;
    size_t ipma_start = 0;
    size_t ipma_end = 0;
    child = iprp_start;
    while (next_box(data, size, child, &box) && box.end <= iprp_end) {
        child = box.end;
        if (std::memcmp(box.type, "ipco", 4) == 0) {
            ipco_start = box.payload;
            ipco_end = box.end;
        } else if (std::memcmp(box.type, "ipma", 4) == 0) {
            ipma_start = box.payload;
            ipma_end = box.end;
        }
    }
    if (ipco_start == 0) return result;

    // Property boxes are counted in order, starting at 1.
    child = ipco_start;
    while (next_box(data, size, child, &box) && box.end <= ipco_end) {
        child = box.end;
        PhotoSize property;
        if (std::memcmp(box.type, "ispe", 4) == 0 && read_ispe(data, size, box, &property)) {
            has_ispe = true;
            // Area, not per-coordinate: the guard needs the largest raster.
            if (static_cast<int64_t>(property.width) * property.height >
                static_cast<int64_t>(largest.width) * largest.height) {
                largest = property;
            }
        }
        properties.push_back(property);
    }

    // The primary item's association list picks its ispe; without one the
    // largest ispe in ipco is the safe answer.
    if (ipma_start != 0 && has_ispe && primary_id != 0) {
        const uint8_t version = data[ipma_start];
        const bool wide = (data[ipma_start + 3] & 1) != 0;  // flags low bit: 16-bit indexes
        PhotoSize primary;
        size_t pos = ipma_start + 4;
        if (pos + 4 <= ipma_end) {
            const uint32_t entries = read_u32_be(data + pos);
            pos += 4;
            for (uint32_t entry = 0; entry < entries; ++entry) {
                if (pos + (version == 0 ? 2u : 4u) + 1 > ipma_end) break;
                const uint32_t item = version == 0 ? read_u16_be(data + pos) : read_u32_be(data + pos);
                pos += version == 0 ? 2 : 4;
                const uint8_t associations = data[pos++];
                for (uint8_t index = 0; index < associations; ++index) {
                    if (pos + (wide ? 2u : 1u) > ipma_end) break;
                    const uint32_t property =
                        wide ? (read_u16_be(data + pos) & 0x7FFFu) : (data[pos] & 0x7Fu);
                    pos += wide ? 2 : 1;
                    if (item != primary_id || property == 0 || property > properties.size()) continue;
                    const PhotoSize& candidate = properties[property - 1];
                    // A file should associate one ispe; if it names several,
                    // the guard takes the largest, never a thumbnail.
                    if (static_cast<int64_t>(candidate.width) * candidate.height >
                        static_cast<int64_t>(primary.width) * primary.height) {
                        primary = candidate;
                    }
                }
            }
        }
        if (primary.width > 0 && primary.height > 0) return primary;
    }
    return largest;
}

double megapixels(int width, int height) {
    return static_cast<double>(width) * static_cast<double>(height) / 1'000'000.0;
}

std::string bytes_label(size_t bytes) {
    if (bytes >= (1ull << 20)) return std::to_string(bytes >> 20) + " MB";
    return std::to_string(bytes) + " bytes";
}

std::string size_refusal(const PhotoSize& size, const DecodeOptions& options) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "photo is %dx%d (%.1f MP); the limit is %d MP - export a smaller copy",
                  size.width, size.height, megapixels(size.width, size.height),
                  options.max_pixels / 1'000'000);
    return text;
}

std::string edge_refusal(const PhotoSize& size, const DecodeOptions& options) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "photo is %dx%d; the longest edge limit is %d px - export a smaller copy",
                  size.width, size.height, options.max_edge);
    return text;
}

}  // namespace

Result<Image> decode_stb_rgb(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return Result<Image>::failure("image buffer is empty");
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 3);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        return Result<Image>::failure(std::string("cannot decode image: ") +
                                      (reason != nullptr ? reason : "unknown format"));
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return Result<Image>::failure("cannot decode image: the file has no pixels");
    }
    Image image;
    image.resize(width, height);
    std::copy(pixels, pixels + image.byte_size(), image.rgb.begin());
    stbi_image_free(pixels);
    return Result<Image>::success(std::move(image));
}

PhotoSize probe_photo_size(const uint8_t* data, size_t size) {
    PhotoSize result;
    if (data == nullptr || size < 8) return result;
    result.format = sniff_image_format(data, size);

    PhotoSize measured;
    switch (result.format) {
        case ImageFormat::jpeg: measured = probe_jpeg(data, size); break;
        case ImageFormat::png:
            if (size >= 24 && std::memcmp(data + 12, "IHDR", 4) == 0) {
                measured.width = static_cast<int>(read_u32_be(data + 16));
                measured.height = static_cast<int>(read_u32_be(data + 20));
            }
            break;
        case ImageFormat::gif:
            if (size >= 10) {
                measured.width = read_u16_le(data + 6);
                measured.height = read_u16_le(data + 8);
            }
            break;
        case ImageFormat::bmp:
            if (size >= 26) {
                measured.width = static_cast<int>(
                    static_cast<int32_t>(read_u32_le(data + 18)));
                // A negative height is a top-down bitmap.
                const int32_t stored = static_cast<int32_t>(read_u32_le(data + 22));
                measured.height = stored < 0 ? -stored : stored;
            }
            break;
        case ImageFormat::tiff:
        case ImageFormat::raw: measured = probe_tiff(data, size); break;
        case ImageFormat::webp: measured = probe_webp(data, size); break;
        case ImageFormat::heif:
        case ImageFormat::avif: measured = probe_isobmff(data, size); break;
        case ImageFormat::tga:
        case ImageFormat::unknown: break;
    }
    result.width = measured.width;
    result.height = measured.height;
    return result;
}

Result<Image> decode_photo(const uint8_t* data, size_t size, const DecodeOptions& options,
                           DecodeInfo* info_out) {
    DecodeInfo info;
    if (data == nullptr || size == 0) return Result<Image>::failure("photo buffer is empty");

    // Guards first: no allocation happens for a photo that will not be decoded.
    if (size > options.max_bytes) {
        return Result<Image>::failure("photo is larger than " + bytes_label(options.max_bytes));
    }

    const PhotoSize probe = probe_photo_size(data, size);
    info.format = probe.format;
    // A RAW file's content is a TIFF (or, for .cr3/.raf, a container the
    // sniffer does not know); only the extension says so, and a .nef sent to
    // libtiff would decode a raw mosaic or fail on its compression. A real
    // photo under a RAW extension is refused, not decoded.
    if (options.hint == ImageFormat::raw) {
        switch (info.format) {
            case ImageFormat::jpeg:
            case ImageFormat::png:
            case ImageFormat::gif:
            case ImageFormat::bmp:
            case ImageFormat::webp:
            case ImageFormat::heif:
            case ImageFormat::avif:
                return Result<Image>::failure(std::string("not a RAW file: the content is ") +
                                              image_format_name(info.format));
            case ImageFormat::tiff:
            case ImageFormat::unknown: info.format = ImageFormat::raw; break;
            default: break;
        }
    }
    if (probe.width > 0 && probe.height > 0) {
        info.source_width = probe.width;
        info.source_height = probe.height;
        const double pixels = static_cast<double>(probe.width) * probe.height;
        if (pixels > static_cast<double>(options.max_pixels)) {
            return Result<Image>::failure(size_refusal(probe, options));
        }
        if (std::max(probe.width, probe.height) > options.max_edge) {
            return Result<Image>::failure(edge_refusal(probe, options));
        }
    }

    Result<Image> decoded = Result<Image>::failure("unsupported photo format");
    switch (info.format) {
        case ImageFormat::webp:
#if defined(SPLAT_HAVE_WEBP)
            decoded = webp_decode_rgb(data, size, &info.note);
#else
            return Result<Image>::failure(
                "cannot decode image: WebP support is not built into this version");
#endif
            break;
        case ImageFormat::tiff:
#if defined(SPLAT_HAVE_TIFF)
            decoded = tiff_decode_rgb(data, size, &info.note);
#else
            return Result<Image>::failure(
                "cannot decode image: TIFF support is not built into this version");
#endif
            break;
        case ImageFormat::heif:
        case ImageFormat::avif:
#if defined(SPLAT_HAVE_HEIF)
            decoded = heif_decode_rgb(data, size, &info);
#else
            return Result<Image>::failure(std::string("cannot decode image: ") +
                                          image_format_name(info.format) +
                                          " support is not built into this version");
#endif
            break;
        case ImageFormat::raw:
#if defined(SPLAT_HAVE_RAW)
            decoded = raw_decode_rgb(data, size, &info);
#else
            return Result<Image>::failure(
                "cannot decode image: RAW support is not built into this version");
#endif
            break;
        case ImageFormat::jpeg:
        case ImageFormat::png:
        case ImageFormat::gif:
        case ImageFormat::bmp:
        case ImageFormat::tga: decoded = decode_stb_rgb(data, size); break;
        case ImageFormat::unknown:
            // TGA has no magic bytes, so a path with a .tga extension arrives
            // here with an unknown sniff and stb is the only decoder for it.
            decoded = decode_stb_rgb(data, size);
            break;
    }
    if (!decoded.ok()) return decoded;

    Image image = decoded.take();
    if (info.source_width <= 0 || info.source_height <= 0) {
        info.source_width = image.width;
        info.source_height = image.height;
    }
    const double pixels = static_cast<double>(image.width) * image.height;
    if (pixels > static_cast<double>(options.max_pixels)) {
        PhotoSize size_only;
        size_only.width = image.width;
        size_only.height = image.height;
        return Result<Image>::failure(size_refusal(size_only, options));
    }

    // EXIF (JPEG APP1 and TIFF IFD0; a HEIF decoder fills info.exif_orientation
    // itself because libheif already applied irot/imir to the pixels).
    if (info.exif_orientation == 1 && options.apply_orientation) {
        const Result<ExifData> exif = parse_exif(data, size);
        if (exif.ok() && exif.value().orientation.has_value()) {
            info.exif_orientation = *exif.value().orientation;
        }
    }
    if (info.exif_orientation < 1 || info.exif_orientation > 8) info.exif_orientation = 1;
    if (options.apply_orientation) apply_exif_orientation(image, info.exif_orientation);
    info.rotate_quarters = options.rotate_quarters;
    rotate_quarters(image, options.rotate_quarters);

    info.width = image.width;
    info.height = image.height;
    if (info_out != nullptr) *info_out = info;
    return Result<Image>::success(std::move(image));
}

Result<Image> decode_photo(const Path& path, const DecodeOptions& options, DecodeInfo* info_out) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) return Result<Image>::failure("cannot open " + path.string());
    std::vector<uint8_t> bytes;
    uint8_t chunk[64 * 1024];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + read);
        if (bytes.size() > options.max_bytes) {
            std::fclose(file);
            return Result<Image>::failure("photo is larger than " + bytes_label(options.max_bytes));
        }
    }
    std::fclose(file);
    if (bytes.empty()) return Result<Image>::failure("photo buffer is empty");

    // A RAW extension over a real photo says so instead of quietly decoding
    // the PNG, which would give a scene with unusable intrinsics. TIFF and
    // unknown content are what a genuine RAW looks like, so those go to
    // LibRaw with the extension as the hint.
    const ImageFormat by_extension = image_format_from_extension(path);
    const ImageFormat by_content = sniff_image_format(bytes.data(), bytes.size());
    if (by_extension == ImageFormat::raw) {
        switch (by_content) {
            case ImageFormat::jpeg:
            case ImageFormat::png:
            case ImageFormat::gif:
            case ImageFormat::bmp:
            case ImageFormat::webp:
            case ImageFormat::heif:
            case ImageFormat::avif:
                return Result<Image>::failure("not a RAW file: " + path.filename().string() +
                                              " contains " + image_format_name(by_content));
            default: break;
        }
    }
    DecodeOptions hinted = options;
    hinted.hint = by_extension;
    return decode_photo(bytes.data(), bytes.size(), hinted, info_out);
}

}  // namespace image
}  // namespace splat
