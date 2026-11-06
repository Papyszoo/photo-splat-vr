// The local file system: the one adapter that wraps std::filesystem.
//
// Every other module in core/gallery, core/scene and core/jobs goes through
// vfs::, so a network adapter and the local disk behave identically from their
// point of view. The entry types come from the directory read (readdir's
// d_type), because a PS5 title is refused lstat (see gallery.h's scene_location
// note): an entry's symlink flag is reported here and nowhere else.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "vfs/io.h"

namespace splat::vfs {

class LocalFileSystem : public IFileSystem {
public:
    std::string id() const override { return "local"; }
    std::string describe() const override { return "This console"; }
    bool writable() const override { return true; }

    Result<std::vector<uint8_t>> read(const std::string& path, size_t max_bytes) override;
    Result<std::vector<uint8_t>> read_range(const std::string& path, uint64_t offset,
                                            size_t length) override;
    Status write(const std::string& path, const uint8_t* data, size_t size) override;
    Status list(const std::string& path, std::vector<DirEntry>* entries) override;
    Status stat(const std::string& path, FileStat* out) override;
    Status make_dirs(const std::string& path) override;
    Status remove_file(const std::string& path) override;
    Status remove_tree(const std::string& path) override;
    Status rename(const std::string& from, const std::string& to) override;
};

}  // namespace splat::vfs
