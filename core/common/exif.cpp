#include "common/exif.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace splat {
namespace {

constexpr uint16_t kTagExifIfdPointer = 0x8769;
constexpr uint16_t kTagFocalLength = 0x920A;
constexpr uint16_t kTagFocalLength35mm = 0xA405;

uint16_t read_u16(const uint8_t* p, bool big_endian) {
    return big_endian ? static_cast<uint16_t>((p[0] << 8) | p[1])
                      : static_cast<uint16_t>((p[1] << 8) | p[0]);
}

uint32_t read_u32(const uint8_t* p, bool big_endian) {
    return big_endian ? (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                            (static_cast<uint32_t>(p[2]) << 8) | p[3]
                      : (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                            (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

constexpr size_t kIfdEntrySize = 12;

// Reads one numeric EXIF value (types BYTE, SHORT, LONG, RATIONAL, SRATIONAL).
// Values that do not fit in the entry's 4-byte field are stored at an offset.
bool read_numeric(const uint8_t* tiff, size_t tiff_size, bool big_endian, uint16_t type,
                  uint32_t count, const uint8_t* value_field, double& out) {
    if (count == 0) return false;
    size_t element_size = 0;
    switch (type) {
        case 1: element_size = 1; break;   // BYTE
        case 3: element_size = 2; break;   // SHORT
        case 4: element_size = 4; break;   // LONG
        case 5:                            // RATIONAL
        case 10: element_size = 8; break;  // SRATIONAL
        default: return false;
    }

    const uint8_t* value = value_field;
    if (static_cast<size_t>(count) * element_size > 4) {
        const uint32_t offset = read_u32(value_field, big_endian);
        if (static_cast<size_t>(offset) + element_size > tiff_size) return false;
        value = tiff + offset;
    }
    if (static_cast<size_t>(value - tiff) + element_size > tiff_size) return false;

    switch (type) {
        case 1:
            out = value[0];
            return true;
        case 3:
            out = read_u16(value, big_endian);
            return true;
        case 4:
            out = read_u32(value, big_endian);
            return true;
        case 5: {
            const uint32_t numerator = read_u32(value, big_endian);
            const uint32_t denominator = read_u32(value + 4, big_endian);
            if (denominator == 0) return false;
            out = static_cast<double>(numerator) / static_cast<double>(denominator);
            return true;
        }
        case 10: {
            const int32_t numerator = static_cast<int32_t>(read_u32(value, big_endian));
            const int32_t denominator = static_cast<int32_t>(read_u32(value + 4, big_endian));
            if (denominator == 0) return false;
            out = static_cast<double>(numerator) / static_cast<double>(denominator);
            return true;
        }
        default:
            return false;
    }
}

// Reads the tags we care about from one IFD and returns the Exif SubIFD offset
// (0 when the pointer is absent).
Status read_ifd(const uint8_t* tiff, size_t tiff_size, bool big_endian, uint32_t offset,
                ExifFocal& focal, uint32_t& exif_ifd_offset) {
    if (static_cast<size_t>(offset) + 2 > tiff_size) return Status::error("EXIF IFD offset out of range");
    const uint16_t entry_count = read_u16(tiff + offset, big_endian);
    const size_t entries_offset = static_cast<size_t>(offset) + 2;
    if (entries_offset + static_cast<size_t>(entry_count) * kIfdEntrySize > tiff_size) {
        return Status::error("EXIF IFD entries out of range");
    }

    for (uint16_t i = 0; i < entry_count; ++i) {
        const uint8_t* entry = tiff + entries_offset + static_cast<size_t>(i) * kIfdEntrySize;
        const uint16_t tag = read_u16(entry, big_endian);
        const uint16_t type = read_u16(entry + 2, big_endian);
        const uint32_t count = read_u32(entry + 4, big_endian);
        const uint8_t* value_field = entry + 8;

        if (tag == kTagExifIfdPointer) {
            double value = 0.0;
            // The value arrives as a double; a crafted RATIONAL can put
            // anything in it, and the double -> uint32 conversion is undefined
            // outside the uint32 range.
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value > 0.0 && value <= 4294967295.0) {
                exif_ifd_offset = static_cast<uint32_t>(value);
            }
        } else if (tag == kTagFocalLength) {
            double value = 0.0;
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value > 0.0 && std::isfinite(value)) {
                focal.focal_length_mm = value;
            }
        } else if (tag == kTagFocalLength35mm) {
            double value = 0.0;
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value > 0.0 && std::isfinite(value)) {
                focal.focal_length_35mm = value;
            }
        }
    }
    return Status::success();
}

Result<ExifFocal> parse_tiff(const uint8_t* tiff, size_t size) {
    if (size < 8) return Result<ExifFocal>::failure("EXIF TIFF header truncated");

    bool big_endian = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        big_endian = false;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
        big_endian = true;
    } else {
        return Result<ExifFocal>::failure("EXIF TIFF byte order marker missing");
    }
    if (read_u16(tiff + 2, big_endian) != 42) {
        return Result<ExifFocal>::failure("EXIF TIFF magic is not 42");
    }
    const uint32_t ifd0_offset = read_u32(tiff + 4, big_endian);

    ExifFocal focal;
    uint32_t exif_ifd_offset = 0;
    Status status = read_ifd(tiff, size, big_endian, ifd0_offset, focal, exif_ifd_offset);
    if (!status.ok()) return Result<ExifFocal>::failure(status.message());

    if (exif_ifd_offset != 0) {
        uint32_t ignored = 0;
        status = read_ifd(tiff, size, big_endian, exif_ifd_offset, focal, ignored);
        if (!status.ok()) return Result<ExifFocal>::failure(status.message());
    }
    return Result<ExifFocal>::success(focal);
}

Result<ExifFocal> find_exif_segment(const uint8_t* data, size_t size) {
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return Result<ExifFocal>::failure("not a JPEG (missing SOI)");
    }

    size_t pos = 2;
    while (pos < size) {
        // Markers may be preceded by any number of 0xFF fill bytes.
        while (pos < size && data[pos] != 0xFF) ++pos;
        while (pos < size && data[pos] == 0xFF) ++pos;
        if (pos >= size) break;

        const uint8_t marker = data[pos++];
        if (marker == 0xD9) break;  // EOI
        if (marker == 0xDA) break;  // SOS: entropy-coded data follows, no more metadata
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;

        if (pos + 2 > size) return Result<ExifFocal>::failure("JPEG segment length truncated");
        const uint16_t length = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        if (length < 2 || pos + length > size) {
            return Result<ExifFocal>::failure("JPEG segment length out of range");
        }

        const uint8_t* payload = data + pos + 2;
        const size_t payload_size = static_cast<size_t>(length) - 2;
        if (marker == 0xE1 && payload_size >= 6 &&
            std::equal(payload, payload + 6, reinterpret_cast<const uint8_t*>("Exif\0\0"))) {
            return parse_tiff(payload + 6, payload_size - 6);
        }
        pos += length;
    }

    return Result<ExifFocal>::success(ExifFocal{});
}

}  // namespace

Result<ExifFocal> parse_jpeg_exif_focal(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return Result<ExifFocal>::failure("empty JPEG buffer");
    return find_exif_segment(data, size);
}

Result<ExifFocal> parse_jpeg_exif_focal(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<ExifFocal>::failure("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_jpeg_exif_focal(bytes.data(), bytes.size());
}

float focal_px_from_mm(double focal_mm, int width, int height) {
    const double diagonal = std::sqrt(static_cast<double>(width) * width +
                                      static_cast<double>(height) * height);
    return static_cast<float>(focal_mm * diagonal / std::sqrt(36.0 * 36.0 + 24.0 * 24.0));
}

Result<float> focal_px_from_exif(const ExifFocal& exif, int width, int height) {
    if (width <= 0 || height <= 0) return Result<float>::failure("image dimensions must be positive");

    double focal_mm = kDefaultFocal35mm;
    if (exif.focal_length_35mm.has_value() && *exif.focal_length_35mm >= 1.0) {
        focal_mm = *exif.focal_length_35mm;
    } else if (exif.focal_length_mm.has_value() && *exif.focal_length_mm > 0.0) {
        focal_mm = *exif.focal_length_mm;
        if (focal_mm < 10.0) focal_mm *= kFocalBelow10mmScale;
    }
    return Result<float>::success(focal_px_from_mm(focal_mm, width, height));
}

Result<float> focal_px_from_jpeg(const uint8_t* data, size_t size, int width, int height) {
    const Result<ExifFocal> exif = parse_jpeg_exif_focal(data, size);
    if (!exif.ok()) return Result<float>::failure(exif.message());
    return focal_px_from_exif(exif.value(), width, height);
}

Result<float> focal_px_from_jpeg(const Path& path, int width, int height) {
    const Result<ExifFocal> exif = parse_jpeg_exif_focal(path);
    if (!exif.ok()) return Result<float>::failure(exif.message());
    return focal_px_from_exif(exif.value(), width, height);
}

}  // namespace splat
