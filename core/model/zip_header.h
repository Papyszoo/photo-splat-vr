// The zip local file header, decoded in one place.
//
// The file path reads it from a seekable archive (core/model/zip_archive.cpp);
// the streaming converter decodes the same header from the network body
// (core/model/stream_converter.cpp). Both need the signature, the general
// purpose flags, the method, the name/extra lengths and (when there is no data
// descriptor) the declared payload size.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace splat {

// "PK\x03\x04" as a little-endian uint32, the form zip writers store.
inline constexpr uint32_t kZipLocalHeaderSignature = 0x04034b50u;
// The fixed part before the name and the extra field.
inline constexpr size_t kZipLocalHeaderBytes = 30;
// Bit 0: the entry is encrypted. Bit 3: crc and sizes follow the payload as a
// data descriptor, so the local header's own sizes are 0.
inline constexpr uint32_t kZipFlagEncrypted = 0x1u;
inline constexpr uint32_t kZipFlagDataDescriptor = 0x8u;

inline uint32_t zip_little_u16(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
}

inline uint32_t zip_little_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

struct ZipLocalHeader {
    uint32_t flags = 0;
    uint32_t method = 0;            // 0 = stored, 8 = deflate
    uint64_t compressed_bytes = 0;  // the header's size; 0 with a data descriptor
    uint32_t name_bytes = 0;
    uint32_t extra_bytes = 0;
};

// Decodes the 30 fixed bytes. The caller has them and waits for (or reads)
// name_bytes + extra_bytes after them; the signature check stays at the call
// site because it decides whether these bytes are a header at all.
inline ZipLocalHeader parse_zip_local_header(const uint8_t* header) {
    ZipLocalHeader local;
    local.flags = zip_little_u16(header + 6);
    local.method = zip_little_u16(header + 8);
    local.compressed_bytes = zip_little_u32(header + 18);
    local.name_bytes = zip_little_u16(header + 26);
    local.extra_bytes = zip_little_u16(header + 28);
    return local;
}

}  // namespace splat
