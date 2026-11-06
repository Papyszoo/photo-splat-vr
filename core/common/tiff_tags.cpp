// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/tiff_tags.h"

namespace splat {
namespace tiff {
namespace {

constexpr size_t kHeaderSize = 8;
constexpr size_t kEntrySize = 12;

}  // namespace

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

void write_u16(uint8_t* p, uint16_t value, bool big_endian) {
    if (big_endian) {
        p[0] = static_cast<uint8_t>(value >> 8);
        p[1] = static_cast<uint8_t>(value & 0xFFu);
    } else {
        p[0] = static_cast<uint8_t>(value & 0xFFu);
        p[1] = static_cast<uint8_t>(value >> 8);
    }
}

void write_u32(uint8_t* p, uint32_t value, bool big_endian) {
    if (big_endian) {
        p[0] = static_cast<uint8_t>(value >> 24);
        p[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        p[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        p[3] = static_cast<uint8_t>(value & 0xFFu);
    } else {
        p[0] = static_cast<uint8_t>(value & 0xFFu);
        p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        p[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        p[3] = static_cast<uint8_t>(value >> 24);
    }
}

bool is_tiff(const uint8_t* data, size_t size) {
    return size >= 8 &&
           ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
            (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A));
}

uint32_t ifd0_offset(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kHeaderSize) return 0;
    const bool big_endian = data[0] == 'M';
    return read_u32(data + 4, big_endian);
}

uint16_t entry_count(const uint8_t* data, size_t size, bool big_endian, size_t offset) {
    if (data == nullptr || offset + 2 > size) return 0;
    return read_u16(data + offset, big_endian);
}

bool entry_at(const uint8_t* data, size_t size, bool big_endian, size_t offset, uint16_t index,
              Entry* out) {
    if (data == nullptr || out == nullptr) return false;
    const uint16_t count = entry_count(data, size, big_endian, offset);
    if (index >= count) return false;
    const size_t entry = offset + 2 + static_cast<size_t>(index) * kEntrySize;
    if (entry + kEntrySize > size) return false;
    out->tag = read_u16(data + entry, big_endian);
    out->type = read_u16(data + entry + 2, big_endian);
    out->count = read_u32(data + entry + 4, big_endian);
    out->value = data + entry + 8;
    return true;
}

bool has_ifd0_tag(const uint8_t* data, size_t size, uint16_t tag) {
    if (!is_tiff(data, size)) return false;
    const bool big_endian = data[0] == 'M';
    const size_t offset = ifd0_offset(data, size);
    const uint16_t count = entry_count(data, size, big_endian, offset);
    for (uint16_t index = 0; index < count; ++index) {
        Entry entry;
        if (!entry_at(data, size, big_endian, offset, index, &entry)) return false;
        if (entry.tag == tag) return true;
    }
    return false;
}

bool read_ifd0_uint(const uint8_t* data, size_t size, uint16_t tag, uint32_t* out) {
    if (!is_tiff(data, size) || out == nullptr) return false;
    const bool big_endian = data[0] == 'M';
    const size_t offset = ifd0_offset(data, size);
    const uint16_t count = entry_count(data, size, big_endian, offset);
    for (uint16_t index = 0; index < count; ++index) {
        Entry entry;
        if (!entry_at(data, size, big_endian, offset, index, &entry)) return false;
        if (entry.tag != tag || entry.count != 1) continue;
        if (entry.type == 3) {
            *out = read_u16(entry.value, big_endian);
            return true;
        }
        if (entry.type == 4) {
            *out = read_u32(entry.value, big_endian);
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace tiff
}  // namespace splat
