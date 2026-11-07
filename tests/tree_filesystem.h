// A file system as a tree of in-memory files, for the tests that need to see
// every directory listing a screen asks for. Registered under an id in
// vfs::file_systems() it stands in for a network share ("smb0:/photos"); the
// picker tests also use it as the Inbox or a USB stick, because a local folder
// cannot report which listings were made of it.
//
// A folder exists when a file lives below it. Every list() call is recorded
// (a listing worker makes them, hence the lock), and a folder named in
// `unreadable` answers with an error instead of its entries.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/status.h"
#include "vfs/io.h"

namespace splat_test {

class TreeFileSystem : public splat::vfs::IFileSystem {
public:
    explicit TreeFileSystem(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return false; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative, size_t) override {
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file");
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::error("fake: read-only");
    }
    splat::Status list(const std::string& relative,
                       std::vector<splat::vfs::DirEntry>* entries) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            list_calls_.push_back(relative);
        }
        if (unreadable.count(relative) != 0) {
            return splat::Status::error(id_ + ": cannot list " + relative + ": access denied");
        }
        const std::string prefix = relative.empty() ? std::string() : relative + "/";
        std::set<std::string> folders;
        for (const auto& file : files) {
            if (file.first.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rest = file.first.substr(prefix.size());
            const std::size_t slash = rest.find('/');
            splat::vfs::DirEntry entry;
            if (slash == std::string::npos) {
                entry.name = rest;
                entry.size = file.second.size();
                entries->push_back(entry);
            } else if (folders.insert(rest.substr(0, slash)).second) {
                entry.name = rest.substr(0, slash);
                entry.directory = true;
                entries->push_back(entry);
            }
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        const auto found = files.find(relative);
        if (found != files.end()) {
            out->exists = true;
            out->regular = true;
            out->size = found->second.size();
            return splat::Status::success();
        }
        const std::string prefix = relative.empty() ? std::string() : relative + "/";
        for (const auto& file : files) {
            if (file.first.compare(0, prefix.size(), prefix) == 0) {
                out->exists = true;
                out->directory = true;
                break;
            }
        }
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }

    std::vector<std::string> list_calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return list_calls_;
    }
    void forget_list_calls() {
        std::lock_guard<std::mutex> lock(mutex_);
        list_calls_.clear();
    }

    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> unreadable;

private:
    std::string id_;
    mutable std::mutex mutex_;
    std::vector<std::string> list_calls_;
};

}  // namespace splat_test
