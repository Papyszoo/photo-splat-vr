// Importing an already-converted splat file into the in-memory scene the rest
// of the app uses.
//
// The readers are the four formats §3.5 of the L2 spec accepts (.splat, .spz
// v1-v4, SuperSplat .compressed.ply, plus the existing .psplat and .ply code).
// Every one of them returns a SplatScene in the standard 3DGS conventions
// (OpenCV axes, log scales, SH degree 0, logit opacity), so nothing downstream
// knows which file it came from.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/path.h"
#include "common/status.h"
#include "scene/splat_scene.h"
#include "splat_import/splat_format.h"

namespace splat {

// The renderer's budget, not a UI preference (SPEC §3.7). A viewed scene costs
// 84 B per splat on the GPU (28 B fp16 attributes + 48 B instance record + 4 B
// key + 4 B value) and, transiently, 56 B fp32 per splat plus the 28 B file
// bytes while the upload runs. 2,000,000 splats is 168 MiB + 112 MiB + 56 MiB
// = 336 MiB of the 512 MiB "viewing one scene" budget, leaving room for the
// stereo eye targets (~33 MiB), the overlay and the preload slot. 4,000,000
// would be ~680 MiB and blow it, so imports are thinned to this count.
inline constexpr uint32_t kMaxSceneSplats = 2'000'000;
// Sources above this are refused instead of thinned: a file that declares more
// than four times the target is not a splat capture, and reading it would cost
// more memory than the import budget allows.
inline constexpr uint32_t kMaxSourceSplats = 8'000'000;
// The largest file the readers accept, checked on the file size before a byte
// is read.
inline constexpr uint64_t kMaxSplatFileBytes = 512ull * 1024 * 1024;

struct ImportLimits {
    uint64_t max_bytes = kMaxSplatFileBytes;        // 512 MiB
    uint32_t max_source_splats = kMaxSourceSplats;  // 8,000,000
    uint32_t max_splats = kMaxSceneSplats;          // 2,000,000, the thinning target
};

struct ImportReport {
    SplatFormat format = SplatFormat::unknown;
    uint32_t source_count = 0;
    uint32_t kept_count = 0;
    bool thinned = false;
    bool converted = false;
    std::string note;  // "spz v4", "thinned 4,000,000 to 2,000,000 splats"
};

// Reads a splat file already in memory. Guards run first: a `size` above
// limits.max_bytes and a format header declaring more than
// limits.max_source_splats are refused before any scene buffer is allocated.
// import_splat() does not thin - the caller decides that (thin_scene()), so a
// reader test can see the source count unchanged.
Result<SplatScene> import_splat(const Path& path, const uint8_t* data, size_t size,
                                const ImportLimits& limits, ImportReport* report);

// The format readers import_splat() dispatches to. They are public for the
// hand-built byte-vector tests, which exercise them without a file.
Result<SplatScene> import_splat_records(const uint8_t* data, size_t size,
                                        const ImportLimits& limits, ImportReport* report);
Result<SplatScene> import_spz(const uint8_t* data, size_t size, const ImportLimits& limits,
                              ImportReport* report);
Result<SplatScene> import_compressed_ply(const uint8_t* data, size_t size,
                                         const ImportLimits& limits, ImportReport* report);

// "8000000" style count with thousands separators, for the messages the queue
// row and meta.json carry ("imported 1,234,567 splats").
std::string count_label(uint64_t count);

// The count guard every reader runs before it sizes a scene: a header claiming
// more than limits.max_source_splats is refused with the limit named.
Status check_source_count(uint64_t declared, const ImportLimits& limits);

}  // namespace splat
