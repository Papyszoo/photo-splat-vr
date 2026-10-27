#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ingest/upload_store.h"
#include "jobs/inbox_watcher.h"
#include "test_util.h"

namespace {

std::string jpeg_bytes(size_t size = 256) {
    std::string data;
    data.push_back(static_cast<char>(0xff));
    data.push_back(static_cast<char>(0xd8));
    data.push_back(static_cast<char>(0xff));
    data.append(size, 'j');
    data.push_back(static_cast<char>(0xff));
    data.push_back(static_cast<char>(0xd9));
    return data;
}

std::string png_bytes(size_t size = 256) {
    const unsigned char magic[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    std::string data(reinterpret_cast<const char*>(magic), sizeof(magic));
    data.append(size, 'p');
    return data;
}

std::string read_file(const splat::Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<splat::Path> inbox_entries(const splat::Path& inbox) {
    std::vector<splat::Path> entries;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(inbox, error)) {
        entries.push_back(entry.path());
    }
    return entries;
}

}  // namespace

TEST_CASE("JPEG and PNG are accepted by magic bytes, not by extension") {
    splat_test::TempDir dir("ingest-upload-accept");
    const splat::Path inbox = dir.path() / "inbox";
    splat::UploadStore store(inbox);

    const auto jpeg = store.store("IMG_0001.JPG", jpeg_bytes());
    REQUIRE_MESSAGE(jpeg.ok(), jpeg.message());
    CHECK(jpeg.value().size() > 4);
    CHECK(jpeg.value().substr(jpeg.value().size() - 4) == ".jpg");

    // A .txt name with PNG content still lands as a PNG.
    const auto png = store.store("screenshot.txt", png_bytes());
    REQUIRE_MESSAGE(png.ok(), png.message());
    CHECK(png.value().substr(png.value().size() - 4) == ".png");

    const auto listed = inbox_entries(inbox);
    CHECK(listed.size() == 2);
    for (const splat::Path& path : listed) {
        CHECK(splat::InboxWatcher::is_photo_file(path));
    }
}

TEST_CASE("content that is not JPEG or PNG is rejected") {
    splat_test::TempDir dir("ingest-upload-magic");
    splat::UploadStore store(dir.path() / "inbox");

    CHECK_FALSE(store.store("photo.jpg", "plain text pretending to be a photo").ok());
    CHECK_FALSE(store.store("photo.jpg", "").ok());
    // Truncated PNG signature and a JPEG-looking prefix that is too short.
    CHECK_FALSE(store.store("photo.png", std::string("\x89PNG", 4)).ok());
    CHECK_FALSE(store.store("photo.jpg", std::string("\xff\xd8", 2)).ok());
    // MP4 ftyp header, a common camera-roll mistake.
    CHECK_FALSE(store.store("clip.jpg", std::string("\x00\x00\x00\x18" "ftypmp42", 12)).ok());

    CHECK(inbox_entries(dir.path() / "inbox").empty());
}

TEST_CASE("uploads over the size limit are rejected") {
    splat_test::TempDir dir("ingest-upload-size");
    splat::UploadStore store(dir.path() / "inbox", 64);
    CHECK(store.max_bytes() == 64);

    const auto too_large = store.store("photo.jpg", jpeg_bytes(128));
    CHECK_FALSE(too_large.ok());
    CHECK(too_large.message().find("larger") != std::string::npos);

    // Exactly at the limit is allowed.
    std::string exact = jpeg_bytes(0);
    exact.resize(64, 'x');
    const auto accepted = store.store("photo.jpg", exact);
    CHECK(accepted.ok());
    CHECK(inbox_entries(dir.path() / "inbox").size() == 1);
}

TEST_CASE("path traversal and hidden names are rejected") {
    splat_test::TempDir dir("ingest-upload-names");
    const splat::Path inbox = dir.path() / "inbox";
    splat::UploadStore store(inbox);

    for (const std::string name : {"../evil.jpg", "..\\evil.jpg", "/tmp/evil.jpg",
                                   "sub/dir/photo.jpg", "..", ".", ".hidden.jpg", ""}) {
        CAPTURE(name);
        const auto result = store.store(name, jpeg_bytes());
        CHECK_FALSE(result.ok());
        CHECK(result.message().find("unsafe") != std::string::npos);
    }
    CHECK(inbox_entries(inbox).empty());

    // Unsafe bytes inside an otherwise plain name are replaced, not used.
    const auto cleaned = store.store("my photo!.jpg", jpeg_bytes());
    REQUIRE(cleaned.ok());
    CHECK(cleaned.value().find("my_photo_") == 0);
    CHECK(cleaned.value().find('/') == std::string::npos);
}

TEST_CASE("an upload lands atomically and the inbox watcher picks it up") {
    splat_test::TempDir dir("ingest-upload-atomic");
    const splat::Path inbox = dir.path() / "inbox";
    splat::UploadStore store(inbox);
    splat::InboxWatcher watcher({inbox});

    const std::string data = jpeg_bytes(4096);
    const auto stored = store.store("IMG_4242.jpg", data);
    REQUIRE_MESSAGE(stored.ok(), stored.message());

    // Only the final file exists: no temp or .part file is left behind.
    const auto entries = inbox_entries(inbox);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().filename().string() == stored.value());
    CHECK(entries.front().filename().string().find(".part") == std::string::npos);
    CHECK(read_file(entries.front()) == data);

    // The watcher needs two stable polls, then hands out exactly that path.
    CHECK(watcher.poll().empty());
    const std::vector<splat::Path> ready = watcher.poll();
    REQUIRE(ready.size() == 1);
    CHECK(ready.front().filename().string() == stored.value());

    // Two uploads with the same original name do not overwrite each other.
    const auto second = store.store("IMG_4242.jpg", png_bytes());
    REQUIRE(second.ok());
    CHECK(second.value() != stored.value());
    CHECK(inbox_entries(inbox).size() == 2);
}
