#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "jobs/inbox_watcher.h"
#include "test_util.h"

namespace {

void write_bytes(const splat::Path& path, size_t count, char fill = 'x') {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string data(count, fill);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void append_bytes(const splat::Path& path, size_t count, char fill = 'y') {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    const std::string data(count, fill);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::set<std::string> filenames(const std::vector<splat::Path>& paths) {
    std::set<std::string> names;
    for (const splat::Path& path : paths) names.insert(path.filename().string());
    return names;
}

}  // namespace

TEST_CASE("a photo is only released when its size is stable across two polls") {
    splat_test::TempDir dir("jobs-inbox");
    const splat::Path inbox = dir.path() / "inbox";
    std::filesystem::create_directories(inbox);
    splat::InboxWatcher watcher({inbox});

    write_bytes(inbox / "photo.jpg", 128);

    CHECK(watcher.poll().empty());  // first sighting only records the size

    const std::vector<splat::Path> ready = watcher.poll();
    REQUIRE(ready.size() == 1);
    CHECK(ready.front().filename() == "photo.jpg");

    // Already handed out: no double enqueue on later polls.
    CHECK(watcher.poll().empty());
    CHECK(watcher.poll().empty());
}

TEST_CASE("a growing file waits until two polls see the same size") {
    splat_test::TempDir dir("jobs-inbox-grow");
    const splat::Path inbox = dir.path() / "inbox";
    std::filesystem::create_directories(inbox);
    splat::InboxWatcher watcher({inbox});

    write_bytes(inbox / "upload.png", 100);
    CHECK(watcher.poll().empty());

    append_bytes(inbox / "upload.png", 300);
    CHECK(watcher.poll().empty());  // size changed since the last poll

    const std::vector<splat::Path> ready = watcher.poll();
    REQUIRE(ready.size() == 1);
    CHECK(ready.front().filename() == "upload.png");
}

TEST_CASE("the watcher scans several directories recursively and filters extensions") {
    splat_test::TempDir dir("jobs-inbox-scan");
    const splat::Path inbox = dir.path() / "inbox";
    const splat::Path usb = dir.path() / "usb";
    std::filesystem::create_directories(inbox / "nested");
    std::filesystem::create_directories(usb);
    splat::InboxWatcher watcher({inbox, usb});

    write_bytes(inbox / "notes.txt", 10);
    write_bytes(inbox / "PHOTO.JPG", 10);
    write_bytes(inbox / "nested" / "deep.jpeg", 10);
    write_bytes(usb / "drive.png", 10);
    write_bytes(usb / "clip.mp4", 10);

    CHECK(watcher.poll().empty());

    const std::vector<splat::Path> ready = watcher.poll();
    CHECK(ready.size() == 3);
    const std::set<std::string> names = filenames(ready);
    CHECK(names.count("PHOTO.JPG") == 1);
    CHECK(names.count("deep.jpeg") == 1);
    CHECK(names.count("drive.png") == 1);

    // Output is sorted for a deterministic enqueue order.
    CHECK(std::is_sorted(ready.begin(), ready.end()));
}

TEST_CASE("a file copied away and back is ingested again") {
    splat_test::TempDir dir("jobs-inbox-readd");
    const splat::Path inbox = dir.path() / "inbox";
    std::filesystem::create_directories(inbox);
    splat::InboxWatcher watcher({inbox});

    const splat::Path photo = inbox / "photo.jpg";
    write_bytes(photo, 64);
    CHECK(watcher.poll().empty());
    REQUIRE(watcher.poll().size() == 1);

    std::filesystem::remove(photo);
    CHECK(watcher.poll().empty());

    write_bytes(photo, 64);
    CHECK(watcher.poll().empty());
    REQUIRE(watcher.poll().size() == 1);
}

TEST_CASE("empty files are stable like any other and missing dirs are ignored") {
    splat_test::TempDir dir("jobs-inbox-empty");
    const splat::Path inbox = dir.path() / "inbox";
    std::filesystem::create_directories(inbox);
    const splat::Path missing = dir.path() / "not-attached";
    splat::InboxWatcher watcher({inbox, missing});

    write_bytes(inbox / "zero.jpg", 0);
    CHECK(watcher.poll().empty());
    const std::vector<splat::Path> ready = watcher.poll();
    REQUIRE(ready.size() == 1);
    CHECK(ready.front().filename() == "zero.jpg");

    CHECK(splat::InboxWatcher::is_photo_file("a.jpg"));
    CHECK(splat::InboxWatcher::is_photo_file("a.JPEG"));
    CHECK_FALSE(splat::InboxWatcher::is_photo_file("a.txt"));
    CHECK_FALSE(splat::InboxWatcher::is_photo_file("a.jpg.tmp"));
}
