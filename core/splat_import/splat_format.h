// The splat file formats this build can read, and the one place that decides
// which reader owns a file.
//
// Content first: a .spz is a gzip stream or an NGSP header, a .psplat starts
// with PSPLAT, a .ply starts with "ply", and SuperSplat's compressed variant is
// a .ply too (its chunk element is what tells it apart). Only .splat has no
// magic at all, so its extension plus the fixed 32-byte record size decide.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/path.h"

namespace splat {

enum class SplatFormat { unknown, psplat, ply, splat, spz, compressed_ply };

// "unknown", "psplat", "ply", "splat", "spz", "compressed_ply". This is also the
// token meta.json stores as the scene's origin format.
const char* splat_format_name(SplatFormat format);

// One .splat record: three fp32 positions, three fp32 linear scales, four u8
// sRGB/alpha bytes and four u8 quaternion bytes (Appendix A.1).
inline constexpr size_t kSplatRecordBytes = 32;

// The format of `data`/`size`, ignoring the extension except for .splat. `path`
// may be empty for a buffer that is not a file (the upload path sniffs bytes).
SplatFormat sniff_splat_format(const Path& path, const uint8_t* data, size_t size);

// True for the extensions a splat file may carry: .psplat .ply .splat .spz.
// A .compressed.ply answers true through .ply.
bool is_splat_file(const Path& path);

}  // namespace splat
