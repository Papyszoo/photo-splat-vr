// core/vfs: the path grammar, the free functions over a registered fake file
// system, the local adapter, and copy_tree local -> fake.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "test_util.h"
#include "vfs/io.h"

using splat::Path;
using splat::Status;
namespace vfs = splat::vfs;

namespace {

// An in-memory file system registered under "fake". Files live in one map keyed
// by relative path; a directory is anything that prefixes a file or was made.
class FakeFileSystem : public vfs::IFileSystem {
public:
    std::string id() const override { return "fake"; }
    std::string describe() const override { return "fake:/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative,
                                             size_t max_bytes) override {
        ++read_calls;
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
        }
        if (max_bytes != 0 && found->second.size() > max_bytes) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: file is too large");
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }

    Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        files[relative] = std::vector<uint8_t>(data, data + size);
        return Status::success();
    }

    Status list(const std::string& relative, std::vector<vfs::DirEntry>* entries) override {
        if (!directories.empty() && directories.count(relative) == 0 &&
            relative.find("/") == std::string::npos) {
            return Status::error("fake: no such directory: " + relative);
        }
        for (const std::string& name : directories) {
            if (!under(relative, name)) continue;
            vfs::DirEntry entry;
            entry.name = leaf(name);
            entry.directory = true;
            entries->push_back(entry);
        }
        for (const auto& file : files) {
            if (!under(relative, file.first)) continue;
            vfs::DirEntry entry;
            entry.name = leaf(file.first);
            entry.directory = false;
            entry.size = file.second.size();
            entries->push_back(entry);
        }
        return Status::success();
    }

    Status stat(const std::string& relative, vfs::FileStat* out) override {
        const auto found = files.find(relative);
        if (found == files.end()) {
            *out = vfs::FileStat{};
            if (directories.count(relative) == 0) return Status::success();
            out->exists = true;
            out->directory = true;
            return Status::success();
        }
        out->exists = true;
        out->regular = true;
        out->size = found->second.size();
        return Status::success();
    }

    Status make_dirs(const std::string& relative) override {
        mkdir_calls.push_back(relative);
        directories.insert(relative);
        return Status::success();
    }

    Status remove_file(const std::string& relative) override {
        removed.push_back(relative);
        if (files.erase(relative) == 0) return Status::error("fake: no such file: " + relative);
        return Status::success();
    }

    Status remove_tree(const std::string& relative) override {
        removed.push_back(relative);
        for (auto it = files.begin(); it != files.end();) {
            it = under(relative, it->first) ? files.erase(it) : std::next(it);
        }
        return Status::success();
    }

    // One level below `parent`: "a/b" for parent "" or "a" for parent "a".
    static bool under(const std::string& parent, const std::string& path) {
        const std::string prefix = parent.empty() ? std::string() : parent + "/";
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return false;
        return path.find('/', prefix.size()) == std::string::npos;
    }
    static std::string leaf(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> directories;
    std::vector<std::string> mkdir_calls;
    std::vector<std::string> removed;
    int read_calls = 0;
};

struct FakeRegistration {
    std::shared_ptr<FakeFileSystem> fake = std::make_shared<FakeFileSystem>();

    FakeRegistration() {
        vfs::file_systems().add(fake);
        fake->files["a/b"] = {1, 2, 3, 4};
        fake->files["scenes/one/scene.psplat"] = {9, 9};
        fake->files["scenes/one/thumb.jpg"] = {8, 8, 8};
        fake->directories.insert("a");
    }
    ~FakeRegistration() { vfs::file_systems().remove("fake"); }
};

void write_file(const Path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(static_cast<bool>(out));
}

}  // namespace

TEST_CASE("vfs reads and writes through a registered file system") {
    FakeRegistration registered;
    const splat::Result<std::vector<uint8_t>> bytes = vfs::read_file(Path("fake:/a/b"));
    REQUIRE(bytes.ok());
    CHECK(bytes.value() == std::vector<uint8_t>{1, 2, 3, 4});

    const uint8_t data[] = {7, 7};
    REQUIRE(vfs::write_file(Path("fake:/a/c"), data, sizeof(data)).ok());
    CHECK(registered.fake->files["a/c"] == std::vector<uint8_t>{7, 7});

    vfs::FileStat status;
    REQUIRE(vfs::stat(Path("fake:/a/b"), &status).ok());
    CHECK(status.exists);
    CHECK(status.regular);
    CHECK(status.size == 4);

    REQUIRE(vfs::stat(Path("fake:/missing"), &status).ok());
    CHECK_FALSE(status.exists);

    std::vector<vfs::DirEntry> entries;
    REQUIRE(vfs::list(Path("fake:/a"), &entries).ok());
    CHECK(entries.size() == 2);  // "b" and "c"
}

TEST_CASE("vfs names an unknown location instead of reading locally") {
    const splat::Result<std::vector<uint8_t>> bytes = vfs::read_file(Path("nosuch:/a"));
    REQUIRE_FALSE(bytes.ok());
    CHECK(bytes.message().find("nosuch") != std::string::npos);
}

TEST_CASE("vfs path grammar splits and joins") {
    std::string id;
    std::string relative;
    REQUIRE(vfs::split(Path("smb0:/a/b"), &id, &relative));
    CHECK(id == "smb0");
    CHECK(relative == "a/b");
    CHECK(vfs::join("smb0", "a/b") == Path("smb0:/a/b"));
    CHECK(vfs::join("smb0", "/a/b") == Path("smb0:/a/b"));
    CHECK(vfs::join("smb0", "") == Path("smb0:/"));
    CHECK(vfs::location_id(Path("smb0:/a/b")) == "smb0");

    // A plain OS path is local, and so is anything the grammar refuses.
    CHECK(vfs::is_remote(Path("/tmp/x")) == false);
    CHECK_FALSE(vfs::is_remote(Path("C:/Users/x")));
    CHECK_FALSE(vfs::is_remote(Path("relative/dir")));
    CHECK(vfs::location_id(Path("/tmp/x")).empty());
    CHECK_FALSE(vfs::split(Path("/tmp/x"), &id, &relative));
}

TEST_CASE("vfs serves local paths from the local adapter") {
    splat_test::TempDir dir("vfs-local");
    const Path file = dir.path() / "x.txt";
    write_file(file, "hello");

    const splat::Result<std::vector<uint8_t>> bytes = vfs::read_file(file);
    REQUIRE(bytes.ok());
    CHECK(std::string(bytes.value().begin(), bytes.value().end()) == "hello");

    const splat::Result<std::vector<uint8_t>> prefix = vfs::read_range(file, 1, 3);
    REQUIRE(prefix.ok());
    CHECK(std::string(prefix.value().begin(), prefix.value().end()) == "ell");

    vfs::FileStat status;
    REQUIRE(vfs::stat(file, &status).ok());
    CHECK(status.exists);
    CHECK(status.regular);
    CHECK(status.size == 5);

    std::vector<vfs::DirEntry> entries;
    REQUIRE(vfs::list(dir.path(), &entries).ok());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "x.txt");
    CHECK_FALSE(entries[0].symlink);
}

TEST_CASE("vfs read_range slices a file larger than the window it asks for") {
    FakeRegistration registered;
    // The fake refuses a read with max_bytes smaller than the file, exactly like
    // the local and SMB adapters: the default read_range must not ask for
    // offset+length, or a short window of a big file would fail.
    const splat::Result<std::vector<uint8_t>> window = vfs::read_range(Path("fake:/a/b"), 1, 2);
    REQUIRE(window.ok());
    CHECK(window.value() == std::vector<uint8_t>{2, 3});
    CHECK(registered.fake->read_calls == 1);

    const splat::Result<std::vector<uint8_t>> tail = vfs::read_range(Path("fake:/a/b"), 3, 100);
    REQUIRE(tail.ok());
    CHECK(tail.value() == std::vector<uint8_t>{4});
}

TEST_CASE("vfs remove_file reaches the fake") {
    FakeRegistration registered;
    REQUIRE(vfs::remove_file(Path("fake:/a/b")).ok());
    CHECK(registered.fake->files.count("a/b") == 0);
    REQUIRE_FALSE(registered.fake->removed.empty());
    CHECK(registered.fake->removed.back() == "a/b");
}

TEST_CASE("vfs copy_tree creates the destination first and copies every file") {
    FakeRegistration registered;
    splat_test::TempDir dir("vfs-copy");
    write_file(dir.path() / "one.psplat", "111");
    write_file(dir.path() / "two.jpg", "2222");

    REQUIRE(vfs::copy_tree(dir.path(), Path("fake:/dest/scene")).ok());

    CHECK(registered.fake->files.count("dest/scene/one.psplat") == 1);
    CHECK(registered.fake->files.count("dest/scene/two.jpg") == 1);
    CHECK(registered.fake->files["dest/scene/two.jpg"].size() == 4);
    REQUIRE_FALSE(registered.fake->mkdir_calls.empty());
    CHECK(registered.fake->mkdir_calls.front() == "dest/scene");
}
