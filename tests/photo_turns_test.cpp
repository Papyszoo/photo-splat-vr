// A photo's turn as a file action (DECISIONS.md, "Change 2026-11-05"): a JPEG
// or baseline TIFF is rewritten in place (its Orientation tag, never a pixel),
// everything else keeps its source and stores the turn. A camera RAW is never
// written to. The same rules run over a fake network adapter.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/exif.h"
#include "common/image.h"
#include "common/path.h"
#include "gallery/photo_turns.h"
#include "jobs/image_io.h"
#include "test_util.h"
#include "vfs/io.h"

namespace {

using splat::Path;
namespace vfs = splat::vfs;

std::vector<uint8_t> read_bytes(const Path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

void write_bytes(const Path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(out));
}

std::vector<uint8_t> jpeg_bytes(int width, int height) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(x * 255 / width);
            pixel[1] = static_cast<uint8_t>(y * 255 / height);
            pixel[2] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    const auto encoded = splat::encode_jpeg(image, 90);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    return encoded.value();
}

std::vector<uint8_t> png_bytes(int width, int height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 120);
    const auto encoded = splat::encode_png(rgba.data(), width, height);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    return encoded.value();
}

int orientation_of(const std::vector<uint8_t>& bytes) {
    const auto data = splat::parse_exif(bytes.data(), bytes.size());
    REQUIRE_MESSAGE(data.ok(), data.message());
    return data.value().orientation.value_or(1);
}

// The bytes after the first APP1 segment, i.e. the compressed image and later
// markers: a tag rewrite must copy these verbatim.
std::vector<uint8_t> after_first_app1(const std::vector<uint8_t>& jpeg) {
    size_t pos = 2;
    while (pos + 4 <= jpeg.size()) {
        if (jpeg[pos] != 0xFF) break;
        const uint8_t marker = jpeg[pos + 1];
        const uint16_t length = static_cast<uint16_t>((jpeg[pos + 2] << 8) | jpeg[pos + 3]);
        if (marker == 0xE1 && length >= 8) {
            return std::vector<uint8_t>(jpeg.begin() + static_cast<std::ptrdiff_t>(pos + 2 + length),
                                        jpeg.end());
        }
        pos += 2 + length;
    }
    return {};
}

// An in-memory network location. `can_rename` false is a share that refuses the
// replace step: the source must then stay whole and the turn must be saved.
class FakeShare : public vfs::IFileSystem {
public:
    explicit FakeShare(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative, size_t) override {
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        files[relative] = std::vector<uint8_t>(data, data + size);
        return splat::Status::success();
    }
    splat::Status list(const std::string&, std::vector<vfs::DirEntry>* entries) override {
        for (const auto& file : files) {
            vfs::DirEntry entry;
            const std::size_t slash = file.first.find_last_of('/');
            entry.name = slash == std::string::npos ? file.first : file.first.substr(slash + 1);
            entry.size = file.second.size();
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, vfs::FileStat* out) override {
        *out = vfs::FileStat{};
        const auto found = files.find(relative);
        if (found == files.end()) return splat::Status::success();
        out->exists = true;
        out->regular = true;
        out->size = found->second.size();
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string& relative) override {
        files.erase(relative);
        return splat::Status::success();
    }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }
    splat::Status rename(const std::string& from, const std::string& to) override {
        if (!can_rename) return splat::Status::error("the share refuses a rename");
        const auto found = files.find(from);
        if (found == files.end()) return splat::Status::error("fake: no such file: " + from);
        // A server that honours SMB2 ReplaceIfExists=0: the name must be free.
        if (!replaces && files.find(to) != files.end()) {
            return splat::Status::error("fake: the target exists: " + to);
        }
        files[to] = std::move(found->second);
        files.erase(from);
        return splat::Status::success();
    }
    bool rename_replaces() const override { return replaces; }

    std::map<std::string, std::vector<uint8_t>> files;
    bool can_rename = true;
    bool replaces = true;

private:
    std::string id_;
};

struct Mounted {
    explicit Mounted(std::shared_ptr<FakeShare> share) : share_(std::move(share)) {
        vfs::file_systems().add(share_);
    }
    ~Mounted() { vfs::file_systems().remove(share_->id()); }
    std::shared_ptr<FakeShare> share_;
};

}  // namespace

TEST_CASE("photo turn: right then left restores a JPEG's picture, orientation and image bytes") {
    splat_test::TempDir temp("photo-turn-jpeg");
    const Path photo = temp.path() / "a.jpg";
    write_bytes(photo, jpeg_bytes(64, 32));
    const std::vector<uint8_t> original = read_bytes(photo);
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto original_image = splat::decode_image(photo);
    REQUIRE_MESSAGE(original_image.ok(), original_image.message());

    const auto right = splat::rotate_photo_file(photo, 1, store, true);
    REQUIRE_MESSAGE(right.ok(), right.message());
    CHECK(right.value().route == splat::PhotoTurnResult::Route::rewritten);
    CHECK(right.value().quarters == 0);
    CHECK(right.value().orientation == 6);
    const std::vector<uint8_t> turned = read_bytes(photo);
    CHECK(orientation_of(turned) == 6);
    // The pixels were not re-encoded: the image after APP1 is identical to the
    // source's bytes after its two SOI bytes (the source had no APP1).
    CHECK(after_first_app1(turned) ==
          std::vector<uint8_t>(original.begin() + 2, original.end()));

    const auto left = splat::rotate_photo_file(photo, -1, store, true);
    REQUIRE_MESSAGE(left.ok(), left.message());
    CHECK(left.value().route == splat::PhotoTurnResult::Route::rewritten);
    CHECK(left.value().orientation == 1);
    CHECK(orientation_of(read_bytes(photo)) == 1);

    const auto restored = splat::decode_image(photo);
    REQUIRE_MESSAGE(restored.ok(), restored.message());
    CHECK(restored.value().width == original_image.value().width);
    CHECK(restored.value().height == original_image.value().height);
    CHECK(restored.value().rgb == original_image.value().rgb);
    // The tag patch round-trips the picture and the orientation; a JPEG that
    // had no APP1 keeps the minimal one this writer added (orientation 1 is
    // the same picture), so the file bytes need not be identical.
    CHECK(orientation_of(read_bytes(photo)) == 1);
}

TEST_CASE("photo turn: a JPEG with no Exif gains a tag and still converts with one turn") {
    splat_test::TempDir temp("photo-turn-noexif");
    const Path photo = temp.path() / "b.jpg";
    write_bytes(photo, jpeg_bytes(64, 32));
    const std::vector<uint8_t> original = read_bytes(photo);
    REQUIRE(orientation_of(original) == 1);
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto turned = splat::rotate_photo_file(photo, 1, store, true);
    REQUIRE_MESSAGE(turned.ok(), turned.message());
    CHECK(turned.value().route == splat::PhotoTurnResult::Route::rewritten);
    const std::vector<uint8_t> written = read_bytes(photo);
    CHECK(orientation_of(written) == 6);
    CHECK(written.size() > original.size());  // the tag was added, no pixel copy
    CHECK(after_first_app1(written) ==
          std::vector<uint8_t>(original.begin() + 2, original.end()));
    CHECK(store.load(photo) == 0);  // the file's tag is the whole turn
}

TEST_CASE("photo turn: a RAW is never read for a rewrite and its bytes never change") {
    splat_test::TempDir temp("photo-turn-raw");
    const Path raw = temp.path() / "phone.dng";
    // Deliberately not a real DNG: the extension alone is enough to say "never
    // write this", and the test proves the bytes are untouched without needing
    // a camera file.
    const std::vector<uint8_t> bytes = {'I', 'I', 0x2A, 0, 8, 0, 0, 0, 1, 2, 3, 4};
    write_bytes(raw, bytes);
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto right = splat::rotate_photo_file(raw, 1, store, true);
    REQUIRE_MESSAGE(right.ok(), right.message());
    CHECK(right.value().route == splat::PhotoTurnResult::Route::saved);
    CHECK(right.value().quarters == 1);
    CHECK(read_bytes(raw) == bytes);
    CHECK(store.load(raw) == 1);

    const auto left = splat::rotate_photo_file(raw, -1, store, true);
    REQUIRE_MESSAGE(left.ok(), left.message());
    CHECK(left.value().quarters == 0);
    CHECK(read_bytes(raw) == bytes);
    CHECK(store.load(raw) == 0);
}

TEST_CASE("photo turn: a failed replace leaves the original whole and saves the turn") {
    splat_test::TempDir temp("photo-turn-failed");
    auto share = std::make_shared<FakeShare>("pt0");
    Mounted mounted(share);
    const std::vector<uint8_t> original = jpeg_bytes(32, 24);
    share->files["photos/a.jpg"] = original;
    share->can_rename = false;
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto turned = splat::rotate_photo_file(Path("pt0:/photos/a.jpg"), 1, store, true);
    REQUIRE_MESSAGE(turned.ok(), turned.message());
    CHECK(turned.value().route == splat::PhotoTurnResult::Route::saved);
    CHECK(turned.value().quarters == 1);
    CHECK_FALSE(turned.value().note.empty());
    // The source is still exactly the bytes it was; the tag was never applied.
    CHECK(share->files["photos/a.jpg"] == original);
    CHECK(orientation_of(share->files["photos/a.jpg"]) == 1);
    CHECK(store.load(Path("pt0:/photos/a.jpg")) == 1);
    // The temporary the failed replace left beside it is gone.
    CHECK(share->files.find("photos/a.jpg.rotate-tmp") == share->files.end());
}

TEST_CASE("photo turn: a saved turn survives a new store, a relaunch and a re-list") {
    splat_test::TempDir temp("photo-turn-saved");
    const Path png = temp.path() / "a.png";
    const std::vector<uint8_t> bytes = png_bytes(8, 4);
    write_bytes(png, bytes);
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto right = splat::rotate_photo_file(png, 1, store, true);
    REQUIRE_MESSAGE(right.ok(), right.message());
    CHECK(right.value().route == splat::PhotoTurnResult::Route::saved);
    CHECK(right.value().quarters == 1);
    CHECK(read_bytes(png) == bytes);

    // A fresh store over the same state directory - a relaunch - reads it back,
    // and the loader a re-list uses sees the same quarter.
    const splat::PhotoTurnStore relaunched(temp.path() / "state");
    CHECK(relaunched.load(png) == 1);
    CHECK(relaunched.load_all().at(vfs::path_key(png).string()) == 1);

    // A new turn replaces the old one rather than accumulating a second entry.
    const auto right_again = splat::rotate_photo_file(png, 1, store, true);
    REQUIRE_MESSAGE(right_again.ok(), right_again.message());
    CHECK(right_again.value().quarters == 2);
    CHECK(store.load_all().size() == 1);
    CHECK(read_bytes(png) == bytes);
}

TEST_CASE("photo turn: a share whose rename cannot replace still overrides the photo") {
    // Every SMB rename carries ReplaceIfExists=0 (libsmb2 6.3), so the temp
    // cannot be renamed over the JPEG directly; the VFS publishes through a
    // sidecar instead. The tag must still land and no sidecar may be left.
    splat_test::TempDir temp("photo-turn-no-replace");
    auto share = std::make_shared<FakeShare>("pt2");
    Mounted mounted(share);
    share->replaces = false;
    const std::vector<uint8_t> original = jpeg_bytes(48, 24);
    share->files["photos/net.jpg"] = original;
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto turned = splat::rotate_photo_file(Path("pt2:/photos/net.jpg"), 1, store, true);
    REQUIRE_MESSAGE(turned.ok(), turned.message());
    CHECK(turned.value().route == splat::PhotoTurnResult::Route::rewritten);
    CHECK(turned.value().file_changed);
    CHECK(orientation_of(share->files["photos/net.jpg"]) == 6);
    CHECK(store.load(Path("pt2:/photos/net.jpg")) == 0);
    for (const auto& file : share->files) {
        CHECK(file.first.find(".rotate-tmp") == std::string::npos);
        CHECK(file.first.find(".replace-old") == std::string::npos);
    }
}

TEST_CASE("photo turn: an unreadable source is an error, not a phantom saved turn") {
    splat_test::TempDir temp("photo-turn-unreadable");
    auto share = std::make_shared<FakeShare>("pt3");
    Mounted mounted(share);
    const splat::PhotoTurnStore store(temp.path() / "state");
    // A stale network listing: the picker shows a JPEG the Mac already deleted.
    const Path photo("pt3:/photos/gone.jpg");

    const auto turned = splat::rotate_photo_file(photo, 1, store, true);
    CHECK_FALSE(turned.ok());
    CHECK(turned.message().find("no such file") != std::string::npos);
    // No turn was invented: nothing in the store, so the conversion cannot
    // apply a turn to a file that is not there.
    CHECK(store.load(photo) == 0);
    CHECK(store.load_all().empty());
}

TEST_CASE("photo turn: a replace interrupted between renames is restored on the next read") {
    // The sidecar is what an SMB replace leaves if the process dies between the
    // two renames. The old bytes are whole under that name; a read of the real
    // name must put them back instead of reporting the photo lost.
    splat_test::TempDir temp("photo-turn-recovery");
    auto share = std::make_shared<FakeShare>("pt4");
    Mounted mounted(share);
    share->replaces = false;
    const std::vector<uint8_t> original = jpeg_bytes(24, 16);
    share->files["photos/a.jpg.replace-old"] = original;

    const auto bytes = vfs::read_file(Path("pt4:/photos/a.jpg"));
    REQUIRE_MESSAGE(bytes.ok(), bytes.message());
    CHECK(bytes.value() == original);
    CHECK(share->files.count("photos/a.jpg") == 1);
    CHECK(share->files.count("photos/a.jpg.replace-old") == 0);

    // A leftover sidecar is not a file the user may pick: a listing hides it.
    share->files["photos/b.jpg.replace-old"] = original;
    std::vector<vfs::DirEntry> entries;
    REQUIRE_MESSAGE(vfs::list(Path("pt4:/photos"), &entries).ok(), "list");
    for (const vfs::DirEntry& entry : entries) {
        CHECK(entry.name.find(".replace-old") == std::string::npos);
    }
}

TEST_CASE("photo turn: a fake network JPEG is rewritten through its adapter") {
    splat_test::TempDir temp("photo-turn-network");
    auto share = std::make_shared<FakeShare>("pt1");
    Mounted mounted(share);
    const std::vector<uint8_t> original = jpeg_bytes(48, 24);
    share->files["photos/net.jpg"] = original;
    const splat::PhotoTurnStore store(temp.path() / "state");

    const auto right =
        splat::rotate_photo_file(Path("pt1:/photos/net.jpg"), 1, store, true);
    REQUIRE_MESSAGE(right.ok(), right.message());
    CHECK(right.value().route == splat::PhotoTurnResult::Route::rewritten);
    CHECK(orientation_of(share->files["photos/net.jpg"]) == 6);
    CHECK(after_first_app1(share->files["photos/net.jpg"]) ==
          std::vector<uint8_t>(original.begin() + 2, original.end()));
    CHECK(store.load(Path("pt1:/photos/net.jpg")) == 0);
    // The key is the location id plus the path inside it, not a local spelling.
    CHECK(vfs::path_key(Path("pt1:/photos/net.jpg")) == Path("pt1:photos/net.jpg"));
}

TEST_CASE("photo turn: a capture's file is never written to") {
    splat_test::TempDir temp("photo-turn-capture");
    const Path capture = temp.path() / "shot.jpg";
    const std::vector<uint8_t> bytes = jpeg_bytes(32, 24);
    write_bytes(capture, bytes);
    const splat::PhotoTurnStore store(temp.path() / "state");

    // The picker passes allow_rewrite=false for the Captures tab: the helper
    // owns those files, so the turn is stored even for a JPEG.
    const auto turned = splat::rotate_photo_file(capture, 1, store, false);
    REQUIRE_MESSAGE(turned.ok(), turned.message());
    CHECK(turned.value().route == splat::PhotoTurnResult::Route::saved);
    CHECK(turned.value().quarters == 1);
    CHECK(read_bytes(capture) == bytes);
}
