// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/atomic_write.h"

#include <cstdio>
#include <utility>

#include "vfs/io.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace splat::vfs {
namespace {

// Flushes a directory entry so a rename survives a power cut, not just a
// process crash. Best effort: a filesystem that cannot do it still gets the
// atomic rename.
void sync_directory(const Path& directory) {
#if !defined(_WIN32)
    if (directory.empty()) return;
    const int fd = ::open(directory.string().c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)directory;
#endif
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

}  // namespace

bool write_text_atomically(const Path& path, const std::string& text, std::string* error,
                           int mode) {
    if (error != nullptr) error->clear();

    Path temporary = path;
    temporary += ".tmp";
    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
        set_error(error, "cannot open " + temporary.string() + " for writing");
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fflush(file) != 0) ok = false;
#if !defined(_WIN32)
    // The permission goes on the temporary, before the rename publishes it, so
    // the destination never exists with the wrong mode.
    if (ok && mode != 0 && ::fchmod(::fileno(file), static_cast<mode_t>(mode)) != 0) ok = false;
    // fflush only reaches the OS; fsync reaches the device, which is what makes
    // the rename below safe across a power cut.
    if (::fsync(::fileno(file)) != 0) ok = false;
#else
    (void)mode;
#endif
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        // Best effort: the write already failed, and the error below names the
        // file the caller must look at.
        (void)remove_file(temporary);
        set_error(error, "failed while writing " + path.string());
        return false;
    }

    // The publish goes through the VFS so this stays the same writer the rest
    // of core uses (and a test's fake file system can see the rename).
    const Status renamed = rename_file(temporary, path);
    if (!renamed.ok()) {
        (void)remove_file(temporary);
        set_error(error, renamed.message());
        return false;
    }
    sync_directory(path.parent_path());
    return true;
}

}  // namespace splat::vfs
