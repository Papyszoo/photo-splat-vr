// The one TIFF/Exif directory reader. Four copies of a fixed-endian u16/u32
// reader and three copies of an IFD0 walker existed before this header (the
// format sniffer, the size guard, the EXIF parser and the TIFF decoder), and
// they disagreed: the sniffer saw DNGVersion that libtiff's copy did not, and
// every copy had its own truncated-file behaviour. Everything that reads a
// TIFF header now goes through these functions.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace splat {
namespace tiff {

// The file's byte order comes from its "II" / "MM" marker.
uint16_t read_u16(const uint8_t* p, bool big_endian);
uint32_t read_u32(const uint8_t* p, bool big_endian);
// The same, for the one writer (common/exif.cpp's orientation tag).
void write_u16(uint8_t* p, uint16_t value, bool big_endian);
void write_u32(uint8_t* p, uint32_t value, bool big_endian);

// One 12-byte directory entry. `value` points at the entry's 4-byte value
// field; for a value longer than four bytes it holds an offset, which the
// caller resolves (common/exif.cpp's read_numeric does the numeric cases).
struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    const uint8_t* value = nullptr;
};

// True for the TIFF magic "II*\0" / "MM\0*" at offset 0.
bool is_tiff(const uint8_t* data, size_t size);

// The IFD0 offset from the header, 0 when the header cannot be read.
uint32_t ifd0_offset(const uint8_t* data, size_t size);

// The entry count of the IFD at `offset`, 0 when it is out of range.
uint16_t entry_count(const uint8_t* data, size_t size, bool big_endian, size_t offset);

// The `index`-th entry of the IFD at `offset`; false when it is out of range.
bool entry_at(const uint8_t* data, size_t size, bool big_endian, size_t offset, uint16_t index,
              Entry* out);

// True when IFD0 lists `tag` at all (any type). DNGVersion is BYTE[4].
bool has_ifd0_tag(const uint8_t* data, size_t size, uint16_t tag);

// The value of IFD0's `tag` when it is a single SHORT or LONG (the types every
// size/compression/orientation tag uses).
bool read_ifd0_uint(const uint8_t* data, size_t size, uint16_t tag, uint32_t* out);

}  // namespace tiff
}  // namespace splat
