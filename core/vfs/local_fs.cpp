// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/local_fs.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace splat::vfs {
namespace {

// std::filesystem::last_write_time has its own clock in C++17; anchor it once
// to the system clock so DirEntry::mtime and FileStat::mtime are Unix seconds
// (the same conversion core/gallery/photo_sources.cpp used before L3).
int64_t mtime_seconds(const std::filesystem::path& path) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return 0;
    static const auto offset =
        std::chrono::system_clock::now().time_since_epoch() -
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::filesystem::file_time_type::clock::now().time_since_epoch());
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(written.time_since_epoch()) +
        offset;
    return std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count();
}

}  // namespace

Result<std::vector<uint8_t>> LocalFileSystem::read(const std::string& path, size_t max_bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<std::vector<uint8_t>>::failure("cannot open " + path);

    if (max_bytes != 0) {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (!error && size > max_bytes) {
            return Result<std::vector<uint8_t>>::failure(
                path + " is " + std::to_string(size) + " bytes, larger than the " +
                std::to_string(max_bytes) + " bytes limit");
        }
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (file.bad()) return Result<std::vector<uint8_t>>::failure("failed while reading " + path);
    return Result<std::vector<uint8_t>>::success(std::move(bytes));
}

Result<std::vector<uint8_t>> LocalFileSystem::read_range(const std::string& path, uint64_t offset,
                                                        size_t length) {
    if (length == 0) return Result<std::vector<uint8_t>>::success({});
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<std::vector<uint8_t>>::failure("cannot open " + path);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) return Result<std::vector<uint8_t>>::success({});
    std::vector<uint8_t> bytes(length);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    bytes.resize(static_cast<size_t>(file.gcount()));
    return Result<std::vector<uint8_t>>::success(std::move(bytes));
}

Status LocalFileSystem::write(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::error("cannot open " + path + " for writing");
    if (size != 0) out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) return Status::error("failed while writing " + path);
    return Status::success();
}

Status LocalFileSystem::list(const std::string& path, std::vector<DirEntry>* entries) {
    if (entries == nullptr) return Status::error("no output vector for listing " + path);

    std::error_code error;
    std::filesystem::directory_iterator it(
        path, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) return Status::error("cannot list " + path + ": " + error.message());

    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code item_error;
        DirEntry entry;
        entry.name = it->path().filename().string();
        // The type the directory read reported: is_symlink() on a
        // directory_entry uses readdir's d_type and needs no lstat.
        entry.symlink = it->is_symlink(item_error);
        if (item_error) item_error.clear();
        entry.directory = !entry.symlink && it->is_directory(item_error) && !item_error;
        const bool regular = !entry.symlink && it->is_regular_file(item_error) && !item_error;
        if (regular) {
            std::error_code size_error;
            const std::uintmax_t size = it->file_size(size_error);
            entry.size = size_error ? 0 : static_cast<uint64_t>(size);
            entry.mtime = mtime_seconds(it->path());
        }
        entries->push_back(std::move(entry));
        it.increment(error);
        if (error) break;  // a partial listing is what the directory gave us
    }
    return Status::success();
}

Status LocalFileSystem::stat(const std::string& path, FileStat* out) {
    if (out == nullptr) return Status::error("no output for stat " + path);

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) {
        *out = FileStat{};
        return Status::success();
    }
    FileStat result;
    result.exists = true;
    result.directory = std::filesystem::is_directory(status);
    result.regular = std::filesystem::is_regular_file(status);
    if (result.regular) {
        std::error_code size_error;
        const std::uintmax_t size = std::filesystem::file_size(path, size_error);
        result.size = size_error ? 0 : static_cast<uint64_t>(size);
        result.mtime = mtime_seconds(path);
    }
    *out = result;
    return Status::success();
}

Status LocalFileSystem::make_dirs(const std::string& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    // An existing directory is success: every writer calls this on a folder it
    // may already have made (a retried job).
    if (error && !std::filesystem::is_directory(path, error)) {
        return Status::error("cannot create " + path + ": " + error.message());
    }
    return Status::success();
}

Status LocalFileSystem::remove_file(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::remove(path, error) && error) {
        return Status::error("cannot remove " + path + ": " + error.message());
    }
    return Status::success();
}

Status LocalFileSystem::remove_tree(const std::string& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) return Status::error("cannot remove " + path + ": " + error.message());
    return Status::success();
}

Status LocalFileSystem::rename(const std::string& from, const std::string& to) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) {
        return Status::error("cannot rename " + from + " to " + to + ": " + error.message());
    }
    return Status::success();
}

}  // namespace splat::vfs
