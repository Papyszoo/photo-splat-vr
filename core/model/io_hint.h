// Bounding the model pipeline's dirty pages.
//
// The SHARP-ready investigation (runs/issue-1-crash/sharp-slow-report.md §2-§4)
// measured the console buffering ~1.5 GB of a 2.8 GB download as dirty page
// cache before every write started to cost 15-40 ms, and the frame rate fell
// with it. The download and the converter therefore write in batches: each
// batch is flushed and fsync'ed (so the pages are clean and the disk has the
// bytes), then its range is dropped from the page cache, so the model file
// does not compete with the VR renderer for memory. The download's batch is
// 64 MiB; the GGUF writer's is 8 MiB (model/gguf_writer.h), because in the
// streaming path its sync runs inside the network sink and the stall watchdog
// has to see it as progress. The same drop trails the converter's reads of the
// .pt, which are one forward sweep after the zip-offset ordering change.
//
// POSIX_FADV_DONTNEED is a page-cache hint: dropping clean pages is safe and
// the data is still on disk. The console's SDK declares posix_fadvise but no
// linked library exports it, so there the batches are flush + fsync only (which
// still bounds the dirty pages - the measured wall). The desktop drops the
// range as well. See io_hint.cpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "common/path.h"

namespace splat {

// One batch: the write path syncs and drops every this many bytes.
inline constexpr uint64_t kCacheBatchBytes = 64ull << 20;

// fsync(fd), then drops [offset, offset + bytes) of the same file from the page
// cache. False when the fsync failed: the caller must fail its transfer or
// conversion like a failed write, since the bytes are not on the disk. The drop
// is best effort and cannot fail the call - the data is on the disk either way,
// and the console has no page-cache advice at all.
bool sync_and_drop(int fd, uint64_t offset, uint64_t bytes);

// Drops [offset, offset + bytes) of `path` from the page cache. The converter's
// read side: it opens the file read-only, advises, and closes again (once per
// 64 MiB, so the extra open costs nothing).
bool drop_file_cache(const Path& path, uint64_t offset, uint64_t bytes);

}  // namespace splat
