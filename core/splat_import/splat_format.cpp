// SPDX-License-Identifier: GPL-3.0-or-later

#include "splat_import/splat_format.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "common/path.h"

namespace splat {
namespace {

// A PLY header is ASCII and every real one ends well inside this window; the
// chunk/packed_position search never touches the binary payload.
constexpr size_t kHeaderScan = 4096;

bool starts_with(const uint8_t* data, size_t size, const char* magic, size_t magic_size) {
    return size >= magic_size && std::memcmp(data, magic, magic_size) == 0;
}

}  // namespace

const char* splat_format_name(SplatFormat format) {
    switch (format) {
        case SplatFormat::unknown: return "unknown";
        case SplatFormat::psplat: return "psplat";
        case SplatFormat::ply: return "ply";
        case SplatFormat::splat: return "splat";
        case SplatFormat::spz: return "spz";
        case SplatFormat::compressed_ply: return "compressed_ply";
    }
    return "unknown";
}

SplatFormat sniff_splat_format(const Path& path, const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return SplatFormat::unknown;

    if (starts_with(data, size, "PSPLAT\0\0", 8)) return SplatFormat::psplat;

    // "ply" then a line break, LF or CRLF.
    if (starts_with(data, size, "ply\n", 4) || starts_with(data, size, "ply\r\n", 5)) {
        const std::string header(reinterpret_cast<const char*>(data),
                                 std::min(size, kHeaderScan));
        // SuperSplat's compressed PLY is the same container with a quantised
        // chunk element; the 3DGS PLY keeps float vertex properties.
        const bool chunk = header.find("element chunk") != std::string::npos;
        const bool packed = header.find("packed_position") != std::string::npos;
        if (chunk && packed) return SplatFormat::compressed_ply;
        return SplatFormat::ply;
    }

    // v1-v3 are a gzip stream; v4 is a plaintext 32-byte header with NGSP.
    if (size >= 2 && data[0] == 0x1f && data[1] == 0x8b) return SplatFormat::spz;
    if (starts_with(data, size, "NGSP", 4)) return SplatFormat::spz;

    // The only headerless format: records of a fixed size, so the length is the
    // count. A truncated record means this is not a .splat after all.
    if (lowercase_extension(path) == ".splat" && size % kSplatRecordBytes == 0) {
        return SplatFormat::splat;
    }
    return SplatFormat::unknown;
}

bool is_splat_file(const Path& path) {
    const std::string extension = lowercase_extension(path);
    return extension == ".psplat" || extension == ".ply" || extension == ".splat" ||
           extension == ".spz";
}

}  // namespace splat
