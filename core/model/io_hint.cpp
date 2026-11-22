// See io_hint.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "model/io_hint.h"

#include <fcntl.h>
#include <unistd.h>

namespace splat {
namespace {

// posix_fadvise takes off_t; both the console's FreeBSD-derived SDK and macOS
// have a 64-bit off_t, so the ranges of a 2.8 GB file fit. The cast is written
// out because the SDK's prototype is the plain POSIX one.
//
// The console keeps fsync only: its SDK's fcntl.h declares posix_fadvise, but
// no library it links exports the symbol (prospero-lld: undefined symbol
// posix_fadvise), and the SDK has no F_NOCACHE equivalent. The fsync still
// bounds the dirty pages - the 1.5 GB dirty-page wall of
// runs/issue-1-crash/sharp-slow-report.md §2.6 - only the clean-page drop is
// desktop-only.
bool advise_dont_need(int fd, uint64_t offset, uint64_t bytes) {
#if defined(POSIX_FADV_DONTNEED) && !defined(__PROSPERO__)
    if (fd < 0 || bytes == 0) return false;
    return posix_fadvise(fd, static_cast<off_t>(offset), static_cast<off_t>(bytes),
                         POSIX_FADV_DONTNEED) == 0;
#else
    (void)fd;
    (void)offset;
    (void)bytes;
    return false;  // no page-cache advice on this platform: fsync only
#endif
}

}  // namespace

bool sync_and_drop(int fd, uint64_t offset, uint64_t bytes) {
    if (fd < 0) return false;
    // The result the caller must act on: a file whose fsync failed is not on
    // the disk, so a download or a conversion that ignored it could be verified,
    // renamed and marked ready from page cache alone.
    if (::fsync(fd) != 0) return false;
    advise_dont_need(fd, offset, bytes);  // page-cache hint only: best effort
    return true;
}

bool drop_file_cache(const Path& path, uint64_t offset, uint64_t bytes) {
#if defined(__PROSPERO__)
    (void)path;
    (void)offset;
    (void)bytes;
    return false;  // no posix_fadvise on the console; fsync-only batches
#else
    if (bytes == 0) return true;
    const int fd = ::open(path.string().c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool dropped = advise_dont_need(fd, offset, bytes);
    ::close(fd);
    return dropped;
#endif
}

}  // namespace splat
