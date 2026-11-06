#include "common/exif.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/tiff_tags.h"

namespace splat {
namespace {

using tiff::read_u16;
using tiff::read_u32;

constexpr uint16_t kTagOrientation = 0x0112;
constexpr uint16_t kTagExifIfdPointer = 0x8769;
constexpr uint16_t kTagFocalLength = 0x920A;
constexpr uint16_t kTagFocalLength35mm = 0xA405;

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
                ExifData& data, uint32_t& exif_ifd_offset) {
    if (static_cast<size_t>(offset) + 2 > tiff_size) return Status::error("EXIF IFD offset out of range");
    const uint16_t entry_count = tiff::entry_count(tiff, tiff_size, big_endian, offset);
    for (uint16_t i = 0; i < entry_count; ++i) {
        tiff::Entry entry;
        if (!tiff::entry_at(tiff, tiff_size, big_endian, offset, i, &entry)) {
            return Status::error("EXIF IFD entries out of range");
        }
        const uint16_t tag = entry.tag;
        const uint16_t type = entry.type;
        const uint32_t count = entry.count;
        const uint8_t* value_field = entry.value;

        if (tag == kTagExifIfdPointer) {
            double value = 0.0;
            // The value arrives as a double; a crafted RATIONAL can put
            // anything in it, and the double -> uint32 conversion is undefined
            // outside the uint32 range.
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value > 0.0 && value <= 4294967295.0) {
                exif_ifd_offset = static_cast<uint32_t>(value);
            }
        } else if (tag == kTagOrientation) {
            double value = 0.0;
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value >= 1.0 && value <= 8.0) {
                data.orientation = static_cast<int>(value);
            }
        } else if (tag == kTagFocalLength) {
            double value = 0.0;
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value > 0.0 && std::isfinite(value)) {
                data.focal.focal_length_mm = value;
            }
        } else if (tag == kTagFocalLength35mm) {
            double value = 0.0;
            if (read_numeric(tiff, tiff_size, big_endian, type, count, value_field, value) &&
                value > 0.0 && std::isfinite(value)) {
                data.focal.focal_length_35mm = value;
            }
        }
    }
    return Status::success();
}

Result<ExifData> parse_tiff(const uint8_t* tiff, size_t size) {
    if (size < 8) return Result<ExifData>::failure("EXIF TIFF header truncated");

    bool big_endian = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        big_endian = false;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
        big_endian = true;
    } else {
        return Result<ExifData>::failure("EXIF TIFF byte order marker missing");
    }
    if (read_u16(tiff + 2, big_endian) != 42) {
        return Result<ExifData>::failure("EXIF TIFF magic is not 42");
    }
    const uint32_t ifd0_offset = read_u32(tiff + 4, big_endian);

    ExifData data;
    uint32_t exif_ifd_offset = 0;
    Status status = read_ifd(tiff, size, big_endian, ifd0_offset, data, exif_ifd_offset);
    if (!status.ok()) return Result<ExifData>::failure(status.message());

    if (exif_ifd_offset != 0) {
        uint32_t ignored = 0;
        status = read_ifd(tiff, size, big_endian, exif_ifd_offset, data, ignored);
        if (!status.ok()) return Result<ExifData>::failure(status.message());
    }
    return Result<ExifData>::success(data);
}

// Where a JPEG's Exif APP1 segment sits: its payload (the bytes the 2-byte
// length field counts, starting at "Exif\0\0") and the offset of that length
// field. Absent when the file is a valid JPEG with no Exif APP1.
struct ExifSegment {
    bool found = false;
    size_t length_offset = 0;   // the 2-byte segment length field
    size_t payload_offset = 0;  // first payload byte ("Exif\0\0")
    size_t payload_size = 0;
};

Status locate_exif_segment(const uint8_t* data, size_t size, ExifSegment* out) {
    *out = ExifSegment{};
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return Status::error("not a JPEG (missing SOI)");
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

        if (pos + 2 > size) return Status::error("JPEG segment length truncated");
        const uint16_t length = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        if (length < 2 || pos + length > size) {
            return Status::error("JPEG segment length out of range");
        }

        const uint8_t* payload = data + pos + 2;
        const size_t payload_size = static_cast<size_t>(length) - 2;
        if (marker == 0xE1 && payload_size >= 6 &&
            std::equal(payload, payload + 6, reinterpret_cast<const uint8_t*>("Exif\0\0"))) {
            out->found = true;
            out->length_offset = pos;
            out->payload_offset = pos + 2;
            out->payload_size = payload_size;
            return Status::success();
        }
        pos += length;
    }
    return Status::success();
}

Result<ExifData> find_exif_segment(const uint8_t* data, size_t size) {
    ExifSegment segment;
    const Status located = locate_exif_segment(data, size, &segment);
    if (!located.ok()) return Result<ExifData>::failure(located.message());
    if (!segment.found) return Result<ExifData>::success(ExifData{});
    return parse_tiff(data + segment.payload_offset + 6, segment.payload_size - 6);
}

// A TIFF/RAW file has no JPEG segments: its IFD0 is the header.
bool looks_like_tiff(const uint8_t* data, size_t size) { return tiff::is_tiff(data, size); }

}  // namespace

Result<ExifData> parse_exif(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return Result<ExifData>::failure("empty image buffer");
    if (looks_like_tiff(data, size)) return parse_tiff(data, size);
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) return find_exif_segment(data, size);
    // A PNG/WebP/HEIF container this reader does not walk carries no EXIF it
    // can use; that is not an error, the caller keeps orientation 1.
    return Result<ExifData>::success(ExifData{});
}

int turn_exif_orientation(int orientation, int quarters) {
    if (orientation < 1 || orientation > 8) orientation = 1;
    // Clockwise 90 degrees composed with each of the eight transforms:
    // 1->6, 2->7, 3->8, 4->5, 5->2, 6->3, 7->4, 8->1. The table is the whole
    // dihedral group, so a turn never invents a ninth value.
    constexpr int kClockwise[9] = {0, 6, 7, 8, 5, 2, 3, 4, 1};
    const int turns = ((quarters % 4) + 4) % 4;
    for (int i = 0; i < turns; ++i) orientation = kClockwise[orientation];
    return orientation;
}

namespace {

constexpr uint16_t kTagOrientationValue = 0x0112;

// Sets the Orientation entry of an in-memory TIFF block. An existing SHORT or
// LONG value is patched; a missing one is added by appending a copy of IFD0
// (same entries, same out-of-line value offsets, same next-IFD pointer) and
// pointing the header at the copy, so no existing byte moves and the pixel
// strips/tiles keep their offsets. Appending rather than inserting is what
// makes this safe for every baseline TIFF: nothing has to be re-based.
Status set_tiff_orientation(std::vector<uint8_t>* tiff, int orientation) {
    if (tiff == nullptr) return Status::error("no TIFF buffer");
    if (orientation < 1 || orientation > 8) return Status::error("orientation outside 1..8");
    if (!tiff->empty() && tiff->size() < 8) return Status::error("TIFF header truncated");
    uint8_t* data = tiff->data();
    const size_t size = tiff->size();
    if (!tiff::is_tiff(data, size)) return Status::error("not a baseline TIFF");

    const bool big_endian = data[0] == 'M';
    const uint32_t ifd0 = tiff::ifd0_offset(data, size);
    if (ifd0 == 0 || static_cast<size_t>(ifd0) + 2 > size) {
        return Status::error("TIFF IFD0 offset out of range");
    }
    const uint16_t count = tiff::entry_count(data, size, big_endian, ifd0);
    const size_t entries = static_cast<size_t>(ifd0) + 2;
    if (entries + static_cast<size_t>(count) * 12 + 4 > size) {
        return Status::error("TIFF IFD0 entries out of range");
    }

    for (uint16_t index = 0; index < count; ++index) {
        tiff::Entry entry;
        if (!tiff::entry_at(data, size, big_endian, ifd0, index, &entry)) {
            return Status::error("TIFF IFD0 entry out of range");
        }
        if (entry.tag != kTagOrientationValue) continue;
        const size_t value_offset = static_cast<size_t>(entry.value - data);
        if (entry.count == 1 && entry.type == 3) {  // SHORT
            tiff::write_u16(data + value_offset, static_cast<uint16_t>(orientation), big_endian);
            return Status::success();
        }
        if (entry.count == 1 && entry.type == 4) {  // LONG
            tiff::write_u32(data + value_offset, static_cast<uint32_t>(orientation), big_endian);
            return Status::success();
        }
        return Status::error("TIFF Orientation tag has an unsupported type");
    }

    // No Orientation entry: append a copy of IFD0 with it added in tag order.
    if (count == 0xFFFFu) return Status::error("TIFF IFD0 has no room for one more entry");
    std::vector<std::vector<uint8_t>> rows;
    rows.reserve(static_cast<size_t>(count) + 1);
    for (uint16_t index = 0; index < count; ++index) {
        const size_t start = entries + static_cast<size_t>(index) * 12;
        rows.emplace_back(data + start, data + start + 12);
    }
    std::vector<uint8_t> added(12, 0);
    tiff::write_u16(added.data(), kTagOrientationValue, big_endian);
    tiff::write_u16(added.data() + 2, 3, big_endian);       // SHORT
    tiff::write_u32(added.data() + 4, 1, big_endian);       // count
    tiff::write_u16(added.data() + 8, static_cast<uint16_t>(orientation), big_endian);
    const auto position = std::upper_bound(
        rows.begin(), rows.end(), kTagOrientationValue,
        [big_endian](uint16_t tag, const std::vector<uint8_t>& row) {
            return tag < tiff::read_u16(row.data(), big_endian);
        });
    rows.insert(position, std::move(added));

    const uint32_t next_ifd =
        tiff::read_u32(data + entries + static_cast<size_t>(count) * 12, big_endian);
    std::vector<uint8_t> block(2, 0);
    tiff::write_u16(block.data(), static_cast<uint16_t>(rows.size()), big_endian);
    for (const std::vector<uint8_t>& row : rows) block.insert(block.end(), row.begin(), row.end());
    const size_t next_at = block.size();
    block.resize(next_at + 4);
    tiff::write_u32(block.data() + next_at, next_ifd, big_endian);

    // TIFF values are word-aligned; pad one byte if the block would start odd.
    if ((size & 1u) != 0) tiff->push_back(0);
    const size_t new_offset = tiff->size();
    if (new_offset > 0xFFFFFFFFu) return Status::error("TIFF is too large to extend");
    tiff->insert(tiff->end(), block.begin(), block.end());
    tiff::write_u32(tiff->data() + 4, static_cast<uint32_t>(new_offset), big_endian);
    return Status::success();
}

}  // namespace

Result<std::vector<uint8_t>> set_exif_orientation(const std::vector<uint8_t>& file,
                                                  int orientation) {
    if (orientation < 1 || orientation > 8) {
        return Result<std::vector<uint8_t>>::failure("orientation outside 1..8");
    }
    if (file.empty()) return Result<std::vector<uint8_t>>::failure("empty file");

    const uint8_t* data = file.data();
    const size_t size = file.size();
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        ExifSegment segment;
        const Status located = locate_exif_segment(data, size, &segment);
        if (!located.ok()) return Result<std::vector<uint8_t>>::failure(located.message());

        if (!segment.found) {
            // A JPEG with no EXIF at all gains a minimal II TIFF with one
            // IFD0 entry: Orientation, SHORT, count 1, value inline.
            const std::vector<uint8_t> tiff = {
                'I', 'I', 0x2A, 0x00, 8, 0, 0, 0,  // header, IFD0 at 8
                1, 0,                             // one entry
                0x12, 0x01, 3, 0, 1, 0, 0, 0,     // tag 0x0112, SHORT, count 1
                static_cast<uint8_t>(orientation), 0, 0, 0,
                0, 0, 0, 0                        // no next IFD
            };
            const size_t length = 2 + 6 + tiff.size();
            if (length > 0xFFFFu) {
                return Result<std::vector<uint8_t>>::failure("EXIF segment would be too large");
            }
            std::vector<uint8_t> out;
            out.reserve(size + 2 + length);
            out.insert(out.end(), file.begin(), file.begin() + 2);  // SOI
            out.push_back(0xFF);
            out.push_back(0xE1);
            out.push_back(static_cast<uint8_t>(length >> 8));
            out.push_back(static_cast<uint8_t>(length & 0xFFu));
            const char exif_header[] = {'E', 'x', 'i', 'f', 0, 0};
            out.insert(out.end(), exif_header, exif_header + 6);
            out.insert(out.end(), tiff.begin(), tiff.end());
            out.insert(out.end(), file.begin() + 2, file.end());
            const Result<ExifData> check = parse_exif(out.data(), out.size());
            if (!check.ok() || check.value().orientation.value_or(1) != orientation) {
                return Result<std::vector<uint8_t>>::failure("cannot verify the new EXIF tag");
            }
            return Result<std::vector<uint8_t>>::success(std::move(out));
        }

        std::vector<uint8_t> tiff(file.begin() + static_cast<std::ptrdiff_t>(segment.payload_offset + 6),
                                  file.begin() + static_cast<std::ptrdiff_t>(segment.payload_offset +
                                                                            segment.payload_size));
        const Status written = set_tiff_orientation(&tiff, orientation);
        if (!written.ok()) return Result<std::vector<uint8_t>>::failure(written.message());
        const size_t length = 2 + 6 + tiff.size();
        if (length > 0xFFFFu) {
            return Result<std::vector<uint8_t>>::failure("EXIF segment would be too large");
        }
        std::vector<uint8_t> out;
        out.reserve(file.size() + tiff.size());
        out.insert(out.end(), file.begin(),
                   file.begin() + static_cast<std::ptrdiff_t>(segment.payload_offset));
        const char exif_header[] = {'E', 'x', 'i', 'f', 0, 0};
        out.insert(out.end(), exif_header, exif_header + 6);
        out.insert(out.end(), tiff.begin(), tiff.end());
        out.insert(out.end(),
                   file.begin() + static_cast<std::ptrdiff_t>(segment.payload_offset +
                                                              segment.payload_size),
                   file.end());
        // The length field did not move: it precedes the payload in the prefix.
        tiff::write_u16(out.data() + segment.length_offset, static_cast<uint16_t>(length), true);
        const Result<ExifData> check = parse_exif(out.data(), out.size());
        if (!check.ok() || check.value().orientation.value_or(1) != orientation) {
            return Result<std::vector<uint8_t>>::failure("cannot verify the new EXIF tag");
        }
        return Result<std::vector<uint8_t>>::success(std::move(out));
    }

    if (tiff::is_tiff(data, size)) {
        std::vector<uint8_t> out = file;
        const Status written = set_tiff_orientation(&out, orientation);
        if (!written.ok()) return Result<std::vector<uint8_t>>::failure(written.message());
        const Result<ExifData> check = parse_exif(out.data(), out.size());
        if (!check.ok() || check.value().orientation.value_or(1) != orientation) {
            return Result<std::vector<uint8_t>>::failure("cannot verify the new EXIF tag");
        }
        return Result<std::vector<uint8_t>>::success(std::move(out));
    }

    return Result<std::vector<uint8_t>>::failure(
        "only a JPEG or a baseline TIFF can be rewritten in place");
}

Result<ExifFocal> parse_jpeg_exif_focal(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return Result<ExifFocal>::failure("empty JPEG buffer");
    const Result<ExifData> data_result = find_exif_segment(data, size);
    if (!data_result.ok()) return Result<ExifFocal>::failure(data_result.message());
    return Result<ExifFocal>::success(data_result.value().focal);
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
