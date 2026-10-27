#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "gallery/gallery.h"
#include "gallery/photo_sources.h"
#include "gallery/thumbs.h"
#include "jobs/image_io.h"
#include "render/scene_playlist.h"
#include "scene/scene_folder.h"
#include "test_util.h"

using splat::Path;

namespace {

// Scene folder with scene.psplat and meta.json; a dummy JPEG stands in for the
// images, so nothing here needs a real decoder.
Path write_scene(const Path& folder, bool with_thumb = true) {
    std::filesystem::create_directories(folder);
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.intrinsics.width = 64;
    meta.intrinsics.height = 48;
    meta.intrinsics.fx = 50.0f;
    meta.intrinsics.fy = 50.0f;
    meta.intrinsics.cx = 32.0f;
    meta.intrinsics.cy = 24.0f;
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::SplatScene scene = splat_test::make_test_scene(4);
    const auto status = splat::write_scene_folder(folder, scene, meta, jpeg, jpeg);
    REQUIRE_MESSAGE(status.ok(), status.message());
    if (!with_thumb) std::filesystem::remove(splat::scene_files(folder).thumbnail);
    return folder;
}

std::vector<uint8_t> make_jpeg(int width, int height) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(x * 4);
            pixel[1] = static_cast<uint8_t>(y * 4);
            pixel[2] = 128;
        }
    }
    const auto bytes = splat::encode_jpeg(image, 90);
    REQUIRE_MESSAGE(bytes.ok(), bytes.message());
    return bytes.value();
}

void write_file(const Path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(static_cast<bool>(out));
}

void set_mtime(const Path& path, int seconds_from_now) {
    const auto written =
        std::filesystem::file_time_type::clock::now() + std::chrono::seconds(seconds_from_now);
    std::filesystem::last_write_time(path, written);
}

std::vector<std::string> names(const std::vector<splat::PhotoEntry>& photos) {
    std::vector<std::string> out;
    for (const splat::PhotoEntry& photo : photos) out.push_back(photo.name);
    return out;
}

// Restores the working directory at scope exit: the relative-root cases below
// change it, and the other tests expect the harness's directory.
class WorkingDirGuard {
public:
    WorkingDirGuard() : saved_(std::filesystem::current_path()) {}
    ~WorkingDirGuard() {
        std::error_code error;
        std::filesystem::current_path(saved_, error);
    }
    WorkingDirGuard(const WorkingDirGuard&) = delete;
    WorkingDirGuard& operator=(const WorkingDirGuard&) = delete;

private:
    Path saved_;
};

}  // namespace

TEST_CASE("gallery locates, renames and deletes only scene folders under the root") {
    splat_test::TempDir temp("gallery-scene-ops");
    const Path root = temp.path() / "scenes";
    const Path loose = write_scene(root / "loose");
    const Path inside = write_scene(root / "trip" / "s1");
    write_scene(root / "trip" / "s2");
    const Path deep = write_scene(root / "trip" / "nested" / "s3");
    const Path outside = write_scene(temp.path() / "elsewhere");
    std::filesystem::create_directories(root / "trip" / "plain");

    std::string album = "unchanged";
    CHECK(splat::scene_location(root, loose, album));
    CHECK(album.empty());
    CHECK(splat::scene_location(root, inside, album));
    CHECK(album == "trip");
    // Not a scene of this root: deeper nesting, outside it, the root, an album,
    // a folder without scene.psplat, a path with "..".
    CHECK_FALSE(splat::scene_location(root, deep, album));
    CHECK_FALSE(splat::scene_location(root, outside, album));
    CHECK_FALSE(splat::scene_location(root, root, album));
    CHECK_FALSE(splat::scene_location(root, root / "trip", album));
    CHECK_FALSE(splat::scene_location(root, root / "trip" / "plain", album));
    CHECK_FALSE(splat::scene_location(root, root / "trip" / ".." / ".." / "elsewhere", album));
#if !defined(_WIN32)
    // A symlink to a scene is not the scene.
    std::filesystem::create_directory_symlink(outside, root / "link");
    CHECK_FALSE(splat::scene_location(root, root / "link", album));
    CHECK_FALSE(splat::delete_scene(root, root / "link").ok());
    CHECK(std::filesystem::is_directory(outside));
#endif

    // Rename stays in the scene's own album and follows the album-name rules.
    CHECK_FALSE(splat::rename_scene(root, inside, "s2").ok());       // taken
    CHECK_FALSE(splat::rename_scene(root, inside, "../up").ok());    // not a name
    CHECK_FALSE(splat::rename_scene(root, inside, ".hidden").ok());
    CHECK(splat::rename_scene(root, inside, "s1").ok());              // unchanged
    REQUIRE(splat::rename_scene(root, inside, "Market square").ok());
    CHECK(std::filesystem::is_directory(root / "trip" / "Market square"));
    CHECK_FALSE(std::filesystem::exists(inside));
    CHECK_FALSE(splat::rename_scene(root, outside, "x").ok());

    // Delete removes that scene's folder and nothing else.
    REQUIRE(splat::delete_scene(root, root / "trip" / "Market square").ok());
    CHECK_FALSE(std::filesystem::exists(root / "trip" / "Market square"));
    CHECK(std::filesystem::is_directory(root / "trip" / "s2"));
    CHECK(std::filesystem::is_directory(root / "trip"));
    CHECK_FALSE(splat::delete_scene(root, root / "trip").ok());
    CHECK_FALSE(splat::delete_scene(root, root).ok());
    CHECK_FALSE(splat::delete_scene(root, outside).ok());
    CHECK_FALSE(splat::delete_scene(root, deep).ok());
    CHECK(std::filesystem::is_directory(outside));
    CHECK(std::filesystem::is_directory(deep));
}

TEST_CASE("gallery locates scenes by lexical paths, not canonical()") {
    splat_test::TempDir temp("gallery-location");
    const Path root = temp.path() / "scenes";
    const Path loose = write_scene(root / "loose");
    const Path inside = write_scene(root / "trip" / "s1");
    const Path outside = write_scene(temp.path() / "elsewhere");
    write_scene(root / ".hidden-scene");
    write_scene(root / ".hidden-album" / "s2");
    write_scene(root / "trip" / "nested" / "s3");
    std::filesystem::create_directories(root / "trip" / "plain");

    std::string album = "unchanged";

    // A trailing slash names the same folder: root, scene, and both at once.
    CHECK(splat::scene_location(Path(root.string() + "/"), inside, album));
    CHECK(album == "trip");
    CHECK(splat::scene_location(Path(root.string() + "/"), Path(inside.string() + "/"), album));
    CHECK(album == "trip");
    CHECK(splat::scene_location(root, Path(loose.string() + "/"), album));
    CHECK(album.empty());
    CHECK_FALSE(splat::scene_location(Path(root.string() + "/"), Path(root.string() + "/"), album));

    // "." parts and repeated separators are resolved by name.
    CHECK(splat::scene_location(root / ".", root / "." / "trip" / "." / "s1", album));
    CHECK(album == "trip");
    CHECK(splat::scene_location(Path(root.string() + "//"), Path(inside.string() + "/./"), album));
    CHECK(album == "trip");

    // Hidden names and the one-album nesting rule still hold.
    CHECK_FALSE(splat::scene_location(root, root / ".hidden-scene", album));
    CHECK_FALSE(splat::scene_location(root, root / ".hidden-album" / "s2", album));
    CHECK_FALSE(splat::scene_location(root, root / "trip" / "plain", album));
    CHECK_FALSE(splat::scene_location(root, root / "trip" / "nested" / "s3", album));
    CHECK_FALSE(splat::scene_location(root, root / "trip" / ".." / ".." / "elsewhere", album));
    CHECK(std::filesystem::is_directory(outside));

    // A relative root and relative scene resolve against the working
    // directory. The absolute form is taken from the same current_path(), so
    // a symlinked temp prefix (macOS /var) cannot make the two disagree.
    {
        WorkingDirGuard guard;
        std::filesystem::current_path(temp.path());
        const Path here = std::filesystem::current_path();
        CHECK(splat::scene_location(Path("scenes"), Path("scenes/loose"), album));
        CHECK(album.empty());
        CHECK(splat::scene_location(Path("./scenes/"), Path("scenes/trip/s1/"), album));
        CHECK(album == "trip");
        CHECK(splat::scene_location(Path("scenes"), here / "scenes" / "trip" / "s1", album));
        CHECK(album == "trip");
        CHECK(splat::scene_location(here / "scenes", Path("scenes/loose"), album));
        CHECK(album.empty());
    }

#if !defined(_WIN32)
    // Symlinks are still refused, including with a trailing slash (the
    // non-following status runs on the normalised path).
    std::filesystem::create_directory_symlink(inside, root / "scene-link");
    CHECK_FALSE(splat::scene_location(root, Path((root / "scene-link").string() + "/"), album));
    CHECK_FALSE(splat::delete_scene(root, Path((root / "scene-link").string() + "/")).ok());
    CHECK(std::filesystem::is_directory(inside));
    // A symlinked album is refused as well.
    std::filesystem::create_directory_symlink(root / "trip", root / "album-link");
    CHECK_FALSE(splat::scene_location(root, root / "album-link" / "s1", album));
    CHECK_FALSE(splat::delete_scene(root, root / "album-link" / "s1").ok());
    CHECK(std::filesystem::is_directory(inside));
    // The link is seen in the parent's listing (the type the directory read
    // reports), so the same holds for a trailing slash and for a dangling
    // link, and a link inside an album never reaches its target.
    CHECK_FALSE(splat::scene_location(root, Path((root / "album-link").string() + "/"), album));
    std::filesystem::create_directory_symlink(root / "missing-target", root / "dangling");
    CHECK_FALSE(splat::scene_location(root, root / "dangling", album));
    CHECK_FALSE(splat::delete_scene(root, root / "dangling").ok());
    std::filesystem::create_directory_symlink(inside, root / "trip" / "alias");
    CHECK_FALSE(splat::scene_location(root, root / "trip" / "alias", album));
    CHECK_FALSE(splat::delete_scene(root, root / "trip" / "alias").ok());
    CHECK(std::filesystem::is_directory(inside));
#endif
}

TEST_CASE("gallery lists albums with the loose scenes first") {
    splat_test::TempDir temp("gallery-albums");
    const Path root = temp.path() / "scenes";
    std::filesystem::create_directories(root);

    write_scene(root / "loose-a");
    write_scene(root / "loose-b", false);
    write_scene(root / "trip" / "s1");
    write_scene(root / "trip" / "s2");
    std::filesystem::create_directories(root / "empty");
    std::filesystem::create_directories(root / ".hidden");
    write_file(root / "notes.txt", "not a directory");

    const std::vector<splat::AlbumEntry> albums = splat::list_albums(root);
    REQUIRE(albums.size() == 3);

    CHECK(albums[0].name == "Unsorted");
    CHECK(albums[0].dir == root);
    CHECK(albums[0].scene_count == 2);
    CHECK(albums[0].cover == root / "loose-a");

    CHECK(albums[1].name == "empty");
    CHECK(albums[1].dir == root / "empty");
    CHECK(albums[1].scene_count == 0);
    CHECK(albums[1].cover.empty());

    CHECK(albums[2].name == "trip");
    CHECK(albums[2].dir == root / "trip");
    CHECK(albums[2].scene_count == 2);
    CHECK(albums[2].cover == root / "trip" / "s1");

    // A missing root is an empty listing, not an error.
    CHECK(splat::list_albums(temp.path() / "missing").empty());

    // An existing but empty root still offers the implicit album.
    const Path empty_root = temp.path() / "empty-root";
    std::filesystem::create_directories(empty_root);
    const std::vector<splat::AlbumEntry> empty_albums = splat::list_albums(empty_root);
    REQUIRE(empty_albums.size() == 1);
    CHECK(empty_albums[0].name == "Unsorted");
    CHECK(empty_albums[0].scene_count == 0);
}

TEST_CASE("gallery lists the scenes of an album") {
    splat_test::TempDir temp("gallery-scenes");
    const Path root = temp.path() / "scenes";
    write_scene(root / "b-scene");
    write_scene(root / "a-scene", false);
    write_scene(root / "trip" / "z-scene");
    write_scene(root / "trip" / "y-scene");
    // A scene whose meta.json is broken is still listed, with a default meta.
    write_scene(root / "trip" / "broken");
    write_file(splat::scene_files(root / "trip" / "broken").meta, "not json");

    const std::vector<splat::SceneEntry> loose = splat::list_scenes(root);
    REQUIRE(loose.size() == 2);
    CHECK(loose[0].name == "a-scene");
    CHECK(loose[0].album.empty());
    CHECK(loose[0].has_thumb == false);
    CHECK(loose[1].name == "b-scene");
    CHECK(loose[1].album.empty());
    CHECK(loose[1].has_thumb);
    CHECK(loose[1].meta.backend == "sharp");
    CHECK(loose[1].meta.intrinsics.width == 64);

    // The implicit album's display name resolves to the root as well.
    CHECK(splat::list_scenes(root, "Unsorted").size() == loose.size());

    const std::vector<splat::SceneEntry> trip = splat::list_scenes(root, "trip");
    REQUIRE(trip.size() == 3);
    CHECK(trip[0].name == "broken");
    CHECK(trip[0].album == "trip");
    CHECK(trip[0].meta.backend.empty());
    CHECK(trip[1].name == "y-scene");
    CHECK(trip[2].name == "z-scene");

    CHECK(splat::list_scenes(root, "missing").empty());
    CHECK(splat::list_scenes(temp.path() / "missing-root").empty());
}

TEST_CASE("gallery validates album names") {
    CHECK(splat::valid_album_name("a"));
    CHECK(splat::valid_album_name("2026-10-26"));
    CHECK(splat::valid_album_name("my album_1.2"));
    CHECK(splat::valid_album_name(std::string(48, 'a')));
    CHECK_FALSE(splat::valid_album_name(""));
    CHECK_FALSE(splat::valid_album_name(".hidden"));
    CHECK_FALSE(splat::valid_album_name("bad/name"));
    CHECK_FALSE(splat::valid_album_name("back\\slash"));
    CHECK_FALSE(splat::valid_album_name("bad:name"));
    CHECK_FALSE(splat::valid_album_name("tab\tname"));
    CHECK_FALSE(splat::valid_album_name("caf\xC3\xA9"));
    CHECK_FALSE(splat::valid_album_name(std::string(49, 'a')));
}

TEST_CASE("gallery creates, renames and deletes albums") {
    splat_test::TempDir temp("gallery-mutate");
    const Path root = temp.path() / "scenes";

    REQUIRE(splat::create_album(root, "trip").ok());
    CHECK(std::filesystem::is_directory(root / "trip"));
    CHECK_FALSE(splat::create_album(root, "trip").ok());
    CHECK_FALSE(splat::create_album(root, "bad/name").ok());
    CHECK_FALSE(splat::create_album(root, ".hidden").ok());
    CHECK_FALSE(splat::create_album(root, "Unsorted").ok());

    REQUIRE(splat::rename_album(root, "trip", "holiday").ok());
    CHECK_FALSE(std::filesystem::exists(root / "trip"));
    CHECK(std::filesystem::is_directory(root / "holiday"));
    REQUIRE(splat::rename_album(root, "holiday", "holiday").ok());  // unchanged is a no-op
    CHECK_FALSE(splat::rename_album(root, "holiday", "Unsorted").ok());
    CHECK_FALSE(splat::rename_album(root, "../holiday", "x").ok());  // source name is validated too
    CHECK_FALSE(splat::rename_album(root, "holiday", "bad/name").ok());
    CHECK_FALSE(splat::rename_album(root, "missing", "x").ok());
    CHECK_FALSE(splat::rename_album(root, "", "x").ok());
    REQUIRE(splat::create_album(root, "other").ok());
    CHECK_FALSE(splat::rename_album(root, "holiday", "other").ok());
    CHECK(std::filesystem::is_directory(root / "holiday"));

    REQUIRE(splat::delete_album(root, "other").ok());
    CHECK_FALSE(std::filesystem::exists(root / "other"));
    CHECK_FALSE(splat::delete_album(root, "").ok());
    CHECK_FALSE(splat::delete_album(root, "missing").ok());

    // An album holding a scene is refused; so is one holding anything else.
    write_scene(root / "holiday" / "scene-a");
    CHECK_FALSE(splat::delete_album(root, "holiday").ok());
    CHECK(std::filesystem::is_directory(root / "holiday" / "scene-a"));
    REQUIRE(splat::create_album(root, "junk").ok());
    write_file(root / "junk" / "notes.txt", "keep me");
    CHECK_FALSE(splat::delete_album(root, "junk").ok());
    CHECK(std::filesystem::exists(root / "junk" / "notes.txt"));
}

TEST_CASE("gallery moves scenes between albums") {
    splat_test::TempDir temp("gallery-move");
    const Path root = temp.path() / "scenes";
    write_scene(root / "scene-a");
    REQUIRE(splat::create_album(root, "trip").ok());

    REQUIRE(splat::move_scene(root, root / "scene-a", "trip").ok());
    CHECK_FALSE(std::filesystem::exists(root / "scene-a"));
    CHECK(std::filesystem::is_regular_file(root / "trip" / "scene-a" / "scene.psplat"));

    // Moving into the album it is already in changes nothing.
    REQUIRE(splat::move_scene(root, root / "trip" / "scene-a", "trip").ok());
    CHECK(std::filesystem::exists(root / "trip" / "scene-a"));

    REQUIRE(splat::move_scene(root, root / "trip" / "scene-a", "").ok());
    CHECK(std::filesystem::is_regular_file(root / "scene-a" / "scene.psplat"));

    // The target folder name is taken: refuse instead of overwriting.
    write_scene(root / "scene-b");
    REQUIRE(splat::move_scene(root, root / "scene-a", "trip").ok());
    write_scene(root / "trip" / "scene-b");
    write_file(root / "trip" / "scene-b" / "keep.txt", "untouched");
    CHECK_FALSE(splat::move_scene(root, root / "scene-b", "trip").ok());
    CHECK(std::filesystem::is_regular_file(root / "scene-b" / "scene.psplat"));
    CHECK(std::filesystem::is_regular_file(root / "trip" / "scene-b" / "keep.txt"));

    write_file(root / "notes.txt", "not a scene");
    CHECK_FALSE(splat::move_scene(root, root / "scene-b", "missing").ok());
    CHECK_FALSE(splat::move_scene(root, root / "notes.txt", "trip").ok());
    CHECK(std::filesystem::is_regular_file(root / "scene-b" / "scene.psplat"));

    const std::vector<splat::SceneEntry> trip = splat::list_scenes(root, "trip");
    REQUIRE(trip.size() == 2);
    CHECK(trip[0].name == "scene-a");
    CHECK(trip[1].name == "scene-b");
}

TEST_CASE("gallery default album names add a suffix on collision") {
    splat_test::TempDir temp("gallery-name");
    const Path root = temp.path() / "scenes";
    std::filesystem::create_directories(root);

    const std::time_t time = 1700000000;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    const std::string base(buffer);

    CHECK(splat::default_album_name(time, root) == base);
    const std::string first = splat::default_album_name(time, root);
    REQUIRE(splat::create_album(root, first).ok());
    CHECK(splat::default_album_name(time, root) == base + "-2");
    REQUIRE(splat::create_album(root, base + "-2").ok());
    CHECK(splat::default_album_name(time, root) == base + "-3");
}

TEST_CASE("gallery discovers the inbox and usb sources") {
    splat_test::TempDir temp("gallery-sources");
    const Path inbox = temp.path() / "inbox";
    const Path mnt = temp.path() / "mnt";
    std::filesystem::create_directories(inbox);
    write_file(inbox / "one.jpg", "jpeg");
    std::filesystem::create_directories(mnt / "usb0");
    std::filesystem::create_directories(mnt / "usb1");  // empty but mounted
    std::filesystem::create_directories(mnt / "usb10");
    std::filesystem::create_directories(mnt / "other");
    write_file(mnt / "usb2", "not a directory");

    splat::SourceRoots roots;
    roots.inbox = inbox;
    roots.mnt = mnt;
    const std::vector<splat::PhotoSource> sources = splat::discover_sources(roots);
    REQUIRE(sources.size() == 4);
    CHECK(sources[0].id == "inbox");
    CHECK(sources[0].label == "Inbox");
    CHECK(sources[0].dir == inbox);
    CHECK_FALSE(sources[0].removable);
    CHECK(sources[1].id == "usb0");
    CHECK(sources[1].dir == mnt / "usb0");
    CHECK(sources[1].removable);
    CHECK(sources[2].id == "usb1");
    CHECK(sources[3].id == "usb10");  // numeric, not lexicographic

    // Missing mnt: the inbox is still the first and only source, even when the
    // inbox directory does not exist yet.
    splat::SourceRoots bare;
    bare.inbox = temp.path() / "no-inbox";
    bare.mnt = temp.path() / "no-mnt";
    const std::vector<splat::PhotoSource> alone = splat::discover_sources(bare);
    REQUIRE(alone.size() == 1);
    CHECK(alone[0].id == "inbox");
    CHECK(alone[0].dir == bare.inbox);
    CHECK(splat::list_photos(alone[0]).empty());
}

TEST_CASE("gallery offers the console roots for the folder browser") {
    splat_test::TempDir temp("gallery-console");
    const Path data = temp.path() / "data";
    const Path mnt = temp.path() / "mnt";
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(mnt / "ext1");
    std::filesystem::create_directories(mnt / "ext0");
    std::filesystem::create_directories(mnt / "usb0");  // a USB stick is its own tab
    write_file(mnt / "ext2", "not a directory");

    splat::SourceRoots roots;
    roots.mnt = mnt;
    roots.data = data;
    const std::vector<splat::PhotoSource> found = splat::console_roots(roots);
    REQUIRE(found.size() == 3);
    CHECK(found[0].id == "data");
    CHECK(found[0].dir == data);
    CHECK(found[0].label == data.string());
    CHECK(found[1].id == "ext0");
    CHECK(found[1].dir == mnt / "ext0");
    CHECK(found[2].id == "ext1");

    // No data root and no extended storage: nothing to browse.
    splat::SourceRoots bare;
    bare.mnt = temp.path() / "no-mnt";
    CHECK(splat::console_roots(bare).empty());
    bare.data = temp.path() / "no-data";
    CHECK(splat::console_roots(bare).empty());
}

TEST_CASE("gallery lists one folder level: folders, then photos") {
    splat_test::TempDir temp("gallery-folder");
    const Path dir = temp.path() / "photos";
    std::filesystem::create_directories(dir / "Zoo");
    std::filesystem::create_directories(dir / "attic");
    std::filesystem::create_directories(dir / ".thumbs");
    write_file(dir / "b.JPG", "jpeg");
    write_file(dir / "A.png", "png!");
    write_file(dir / "c.jpeg", "jpeg");
    write_file(dir / "notes.txt", "text");
    write_file(dir / ".hidden.jpg", "jpeg");
    write_file(dir / "attic" / "deep.jpg", "jpeg");  // one level only

    const std::vector<splat::FolderEntry> entries = splat::list_folder(dir);
    REQUIRE(entries.size() == 5);
    CHECK(entries[0].name == "attic");
    CHECK(entries[0].folder);
    CHECK(entries[1].name == "Zoo");  // case-insensitive order
    CHECK(entries[1].folder);
    CHECK(entries[2].name == "A.png");
    CHECK_FALSE(entries[2].folder);
    CHECK(entries[2].size == 4);
    CHECK(entries[2].path == dir / "A.png");
    CHECK(entries[3].name == "b.JPG");
    CHECK(entries[4].name == "c.jpeg");

    CHECK(splat::list_folder(temp.path() / "missing").empty());
}

TEST_CASE("gallery lists photos by extension, depth and mtime") {
    splat_test::TempDir temp("gallery-photos");
    const Path dir = temp.path() / "inbox";
    write_file(dir / "a.jpg", "aaaa");
    write_file(dir / "B.JPEG", "bb");
    write_file(dir / "c.PnG", "ccc");
    write_file(dir / "d.txt", "ignored");
    write_file(dir / "e.jxr", "ignored");
    write_file(dir / "sub" / "f.jpg", "fffff");
    write_file(dir / "sub" / "sub2" / "g.png", "gg");
    write_file(dir / "sub" / "sub2" / "sub3" / "h.jpeg", "hhh");
    write_file(dir / "sub" / "sub2" / "sub3" / "sub4" / "i.jpg", "too deep");

    set_mtime(dir / "a.jpg", 60);
    set_mtime(dir / "B.JPEG", 50);
    set_mtime(dir / "c.PnG", 40);
    set_mtime(dir / "sub" / "f.jpg", 30);
    set_mtime(dir / "sub" / "sub2" / "g.png", 20);
    set_mtime(dir / "sub" / "sub2" / "sub3" / "h.jpeg", 10);

    const splat::PhotoSource source{"inbox", "Inbox", dir, false};
    const std::vector<splat::PhotoEntry> photos = splat::list_photos(source);
    REQUIRE(photos.size() == 6);
    CHECK(names(photos) == std::vector<std::string>{"a.jpg", "B.JPEG", "c.PnG", "f.jpg", "g.png",
                                                   "h.jpeg"});
    CHECK(photos[0].path == dir / "a.jpg");
    CHECK(photos[0].size == 4);
    CHECK(photos[0].mtime > 0);
    CHECK(photos[0].mtime > photos[1].mtime);

    CHECK(splat::is_photo_file("x.JPG"));
    CHECK(splat::is_photo_file("x.jpeg"));
    CHECK(splat::is_photo_file("x.png"));
    CHECK_FALSE(splat::is_photo_file("x.jxr"));
    CHECK_FALSE(splat::is_photo_file("x"));

    const splat::PhotoSource missing{"inbox", "Inbox", temp.path() / "missing", false};
    CHECK(splat::list_photos(missing).empty());
}

TEST_CASE("gallery caps a photo listing at 500 entries") {
    splat_test::TempDir temp("gallery-photo-cap");
    const Path dir = temp.path() / "inbox";
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 505; ++i) {
        write_file(dir / ("p" + std::to_string(i) + ".jpg"), "x");
    }
    const splat::PhotoSource source{"inbox", "Inbox", dir, false};
    CHECK(splat::list_photos(source).size() == 500);
}

TEST_CASE("gallery loads photo and scene thumbnails") {
    splat_test::TempDir temp("gallery-thumbs");
    const Path photo = temp.path() / "photo.jpg";
    const std::vector<uint8_t> jpeg = make_jpeg(64, 32);
    {
        std::ofstream out(photo, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(jpeg.data()),
                  static_cast<std::streamsize>(jpeg.size()));
        REQUIRE(static_cast<bool>(out));
    }

    const auto thumb = splat::load_photo_thumb(photo, 16);
    REQUIRE_MESSAGE(thumb.ok(), thumb.message());
    CHECK(thumb.value().w == 16);
    CHECK(thumb.value().h == 8);
    CHECK(thumb.value().rgba.size() == 16u * 8u * 4u);
    CHECK(thumb.value().rgba[3] == 255);

    // Never upscale.
    const auto full = splat::load_photo_thumb(photo, 256);
    REQUIRE_MESSAGE(full.ok(), full.message());
    CHECK(full.value().w == 64);
    CHECK(full.value().h == 32);

    CHECK_FALSE(splat::load_photo_thumb(temp.path() / "missing.jpg", 16).ok());
    CHECK_FALSE(splat::load_photo_thumb(photo, 0).ok());

    const Path folder = temp.path() / "scenes" / "scene-a";
    std::filesystem::create_directories(folder);
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.intrinsics.width = 64;
    meta.intrinsics.height = 48;
    meta.intrinsics.fx = 50.0f;
    const splat::SplatScene scene = splat_test::make_test_scene(3);
    const std::vector<uint8_t> tiny{0xFF, 0xD8, 0xFF, 0xD9};
    REQUIRE(splat::write_scene_folder(folder, scene, meta, tiny, make_jpeg(40, 20)).ok());

    const auto scene_thumb = splat::load_scene_thumb(folder, 10);
    REQUIRE_MESSAGE(scene_thumb.ok(), scene_thumb.message());
    CHECK(scene_thumb.value().w == 10);
    CHECK(scene_thumb.value().h == 5);

    const Path no_thumb = temp.path() / "scenes" / "scene-b";
    write_scene(no_thumb, false);
    const auto placeholder = splat::load_scene_thumb(no_thumb, 10);
    CHECK_FALSE(placeholder.ok());
}

TEST_CASE("gallery scene meta stores optional provenance and reads old files") {
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.intrinsics.width = 1280;
    meta.intrinsics.height = 873;
    meta.intrinsics.fx = 1074.0f;
    meta.seconds_taken = 1.5;
    meta.splat_count = 10;
    meta.captured_at = "2026-10-25T18:04:00Z";
    meta.source = "usb0";

    const std::string json = splat::scene_meta_to_json(meta);
    const nlohmann::json parsed = nlohmann::json::parse(json);
    CHECK(parsed.at("captured_at").get<std::string>() == "2026-10-25T18:04:00Z");
    CHECK(parsed.at("source").get<std::string>() == "usb0");
    const auto loaded = splat::scene_meta_from_json(json);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().captured_at == meta.captured_at);
    CHECK(loaded.value().source == meta.source);

    // Empty provenance is not written at all.
    meta.captured_at.clear();
    meta.source.clear();
    const std::string plain = splat::scene_meta_to_json(meta);
    const nlohmann::json plain_json = nlohmann::json::parse(plain);
    CHECK(plain_json.count("captured_at") == 0);
    CHECK(plain_json.count("source") == 0);

    // A meta.json written before the fields existed still reads.
    const std::string old_json = R"({
      "format_version": 1,
      "backend": "sharp",
      "intrinsics": {"width": 1280, "height": 873, "fx": 1074.0, "fy": 1074.0,
                     "cx": 640.0, "cy": 436.5},
      "seconds_taken": 2.0,
      "splat_count": 7
    })";
    const auto old = splat::scene_meta_from_json(old_json);
    REQUIRE_MESSAGE(old.ok(), old.message());
    CHECK(old.value().backend == "sharp");
    CHECK(old.value().splat_count == 7);
    CHECK(old.value().captured_at.empty());
    CHECK(old.value().source.empty());

    splat_test::TempDir temp("gallery-old-meta");
    const Path folder = write_scene(temp.path() / "scene-a", false);
    write_file(splat::scene_files(folder).meta, old_json);
    const auto read_back = splat::read_scene_meta(splat::scene_files(folder).meta);
    REQUIRE_MESSAGE(read_back.ok(), read_back.message());
    CHECK(read_back.value().splat_count == 7);
    CHECK(read_back.value().captured_at.empty());
    CHECK(read_back.value().source.empty());
}

TEST_CASE("gallery playlist from album scene entries") {
    splat_test::TempDir temp("gallery-playlist");
    const Path root = temp.path() / "scenes";
    write_scene(root / "trip" / "b-scene");
    write_scene(root / "trip" / "a-scene");

    const std::vector<splat::SceneEntry> scenes = splat::list_scenes(root, "trip");
    REQUIRE(scenes.size() == 2);
    const std::vector<splat::PlaylistEntry> entries = splat::playlist_entries(scenes);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].folder == root / "trip" / "a-scene");
    CHECK(entries[0].scene_file == root / "trip" / "a-scene" / "scene.psplat");

    const auto playlist = splat::ScenePlaylist::from_entries(entries);
    REQUIRE_MESSAGE(playlist.ok(), playlist.message());
    CHECK(playlist.value().size() == 2);
    CHECK(playlist.value().current().folder.filename() == "a-scene");
    splat::ScenePlaylist source = playlist.value();
    source.next();
    CHECK(source.current().folder.filename() == "b-scene");
    source.next();  // wraps
    CHECK(source.current().folder.filename() == "a-scene");

    CHECK_FALSE(splat::ScenePlaylist::from_entries({}).ok());
}
