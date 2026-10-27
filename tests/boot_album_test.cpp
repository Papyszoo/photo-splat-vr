// W34: what the viewer boots into, decided by the gallery, not by a scan of the
// scenes root (v0.1 exited 2 on a fresh install because its samples live in the
// "Samples" album while the boot playlist read the root only).
//
// The cases are the shapes a data folder can have:
//   (a) no scenes at all - an empty or missing scenes root, and an empty data
//       folder (first start before any seed): the viewer must not fail, it opens
//       its menu over the empty view;
//   (b) only the seeded "Samples" album - the fresh-install case a package
//       creates: the first sample is the scene it shows;
//   (c) loose scenes (Unsorted) plus albums: Unsorted wins, and SCENE= picks a
//       folder wherever it sits and brings its own album as the playlist.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "gallery/gallery.h"
#include "gallery/samples.h"
#include "render/scene_playlist.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "test_util.h"

using splat::Path;

namespace {

namespace fs = std::filesystem;

// A scene folder the gallery accepts: scene.psplat is enough for the listing,
// and the playlist only names the file (nothing reads it here).
Path write_scene(const Path& folder) {
    fs::create_directories(folder);
    std::ofstream out(folder / "scene.psplat", std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << "splat-" << folder.filename().string();
    return folder;
}

std::vector<std::string> names(const std::vector<splat::SceneEntry>& scenes) {
    std::vector<std::string> out;
    for (const splat::SceneEntry& scene : scenes) out.push_back(scene.name);
    return out;
}

// The playlist the viewer builds from one boot album: the same call vr_mode.cpp
// makes (playlist_entries over the gallery's own album listing).
splat::ScenePlaylist playlist_of(const splat::BootAlbum& boot) {
    auto built = splat::ScenePlaylist::from_entries(splat::playlist_entries(boot.scenes));
    REQUIRE_MESSAGE(built.ok(), built.message());
    splat::ScenePlaylist playlist = built.take();
    playlist.set_index(boot.index);
    return playlist;
}

}  // namespace

TEST_CASE("gallery boot has no scene in an empty data folder") {
    splat_test::TempDir temp("boot-empty");
    // A fresh install before the seed ran: the data folder exists, scenes does
    // not. Not a failure - the viewer opens its menu.
    const Path data = temp.path() / "data";
    fs::create_directories(data);
    const splat::BootAlbum boot = splat::boot_album(data / "scenes");
    CHECK(boot.scenes.empty());
    CHECK(boot.album.empty());
    CHECK(boot.index == 0);
    CHECK(splat::list_albums(data / "scenes").empty());

    // An existing but empty scenes root behaves the same way.
    const Path root = temp.path() / "scenes";
    fs::create_directories(root);
    CHECK(splat::boot_album(root).scenes.empty());
    // Even with SCENE= naming a folder nothing listens.
    CHECK(splat::boot_album(root, "immichvr-1").scenes.empty());
}

TEST_CASE("gallery boot finds the first seeded sample on a fresh install") {
    splat_test::TempDir temp("boot-samples");
    const Path bundle = temp.path() / "app0-samples";
    const Path scenes = temp.path() / "data" / "scenes";
    const Path marker = temp.path() / "data" / "state" / "samples.seeded";
    // The three scenes the package carries, seeded into scenes/Samples/ exactly
    // as main.cpp does on the first start.
    write_scene(bundle / "Living room");
    write_scene(bundle / "Sailboats");
    write_scene(bundle / "York street");
    const splat::SamplesSeed seed = splat::seed_samples(bundle, scenes, marker);
    REQUIRE_MESSAGE(seed.ok(), seed.error);
    REQUIRE(seed.scenes == 3);
    // The v0.1 failure: nothing at the root, the samples one level down.
    CHECK(splat::list_scenes(scenes).empty());

    const splat::BootAlbum boot = splat::boot_album(scenes);
    REQUIRE(boot.scenes.size() == 3);
    CHECK(boot.album == splat::kSamplesAlbumName);
    CHECK(boot.index == 0);
    CHECK(names(boot.scenes) ==
          std::vector<std::string>{"Living room", "Sailboats", "York street"});
    CHECK(boot.scenes[0].album == splat::kSamplesAlbumName);
    CHECK(boot.scenes[0].folder == scenes / splat::kSamplesAlbumName / "Living room");

    // The boot playlist is that album: the first sample, and L1/R1 step through
    // the three and wrap, never leaving the album (the gallery's own order).
    splat::ScenePlaylist playlist = playlist_of(boot);
    CHECK(playlist.size() == 3);
    CHECK(playlist.current().folder.filename() == "Living room");
    playlist.next();
    CHECK(playlist.current().folder.filename() == "Sailboats");
    playlist.next();
    CHECK(playlist.current().folder.filename() == "York street");
    playlist.next();
    CHECK(playlist.current().folder.filename() == "Living room");

    // SCENE= still overrides for dev, inside the album.
    const splat::BootAlbum picked = splat::boot_album(scenes, "Sailboats");
    CHECK(picked.album == splat::kSamplesAlbumName);
    REQUIRE(picked.scenes.size() == 3);
    CHECK(picked.index == 1);
    CHECK(playlist_of(picked).current().folder.filename() == "Sailboats");
}

TEST_CASE("gallery boot prefers the loose scenes, then the albums in gallery order") {
    splat_test::TempDir temp("boot-unsorted");
    const Path root = temp.path() / "scenes";
    write_scene(root / "b-loose");
    write_scene(root / "a-loose");
    write_scene(root / "trip" / "z-scene");
    write_scene(root / "trip" / "y-scene");
    write_scene(root / "holiday" / "beach");
    fs::create_directories(root / "empty-album");

    // The gallery's listing: Unsorted first, then the albums by name.
    const std::vector<splat::AlbumEntry> albums = splat::list_albums(root);
    REQUIRE(albums.size() == 4);
    CHECK(albums[0].name == splat::kAllScenesAlbumName);
    CHECK(albums[1].name == "empty-album");
    CHECK(albums[2].name == "holiday");
    CHECK(albums[3].name == "trip");

    // Unsorted has scenes, so that is the boot album; an empty album is skipped
    // rather than chosen and left empty.
    const splat::BootAlbum boot = splat::boot_album(root);
    CHECK(boot.album.empty());  // "" is the loose-scene album
    CHECK(names(boot.scenes) == std::vector<std::string>{"a-loose", "b-loose"});
    CHECK(boot.index == 0);
    splat::ScenePlaylist playlist = playlist_of(boot);
    CHECK(playlist.current().folder.filename() == "a-loose");
    playlist.next();
    CHECK(playlist.current().folder.filename() == "b-loose");  // stays in Unsorted
    playlist.next();
    CHECK(playlist.current().folder.filename() == "a-loose");

    // SCENE= names a folder in the second album: that album becomes the playlist
    // and the named scene its current entry (the gallery's order: y before z).
    const splat::BootAlbum trip = splat::boot_album(root, "z-scene");
    CHECK(trip.album == "trip");
    CHECK(names(trip.scenes) == std::vector<std::string>{"y-scene", "z-scene"});
    CHECK(trip.index == 1);
    splat::ScenePlaylist trip_playlist = playlist_of(trip);
    CHECK(trip_playlist.current().folder.filename() == "z-scene");
    trip_playlist.next();
    CHECK(trip_playlist.current().folder.filename() == "y-scene");

    // A loose scene with SCENE= keeps the loose album.
    const splat::BootAlbum loose = splat::boot_album(root, "b-loose");
    CHECK(loose.album.empty());
    CHECK(loose.index == 1);
    CHECK(playlist_of(loose).current().folder.filename() == "b-loose");

    // SCENE= naming nothing falls back to the gallery's first scene, as the
    // viewer logs, instead of leaving the session without a playlist.
    const splat::BootAlbum missing = splat::boot_album(root, "immichvr-1");
    CHECK(missing.album.empty());
    CHECK(names(missing.scenes) == std::vector<std::string>{"a-loose", "b-loose"});
    CHECK(missing.index == 0);
}

TEST_CASE("gallery boot picks the only album when the root has no loose scene") {
    splat_test::TempDir temp("boot-album-only");
    const Path root = temp.path() / "scenes";
    write_scene(root / "trip" / "s2");
    write_scene(root / "trip" / "s1");

    const splat::BootAlbum boot = splat::boot_album(root);
    CHECK(boot.album == "trip");
    CHECK(names(boot.scenes) == std::vector<std::string>{"s1", "s2"});
    CHECK(boot.index == 0);
    CHECK(playlist_of(boot).size() == 2);
}

// A boot playlist can name a folder whose scene.psplat is garbage, truncated,
// unreadable or in another format. That must not end the session: the loader
// the viewer calls (splat::load_boot_scene) walks on to the next scene.
TEST_CASE("gallery boot skips scenes that will not read and loads the next") {
    splat_test::TempDir temp("boot-corrupt");
    const Path root = temp.path() / "scenes";
    write_scene(root / "trip" / "a-garbage");
    const Path truncated = root / "trip" / "b-truncated";
    fs::create_directories(truncated);
    {
        std::ofstream out(truncated / "scene.psplat", std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << "not a psplat header";
    }
    const Path good = root / "trip" / "c-good";
    fs::create_directories(good);
    REQUIRE(splat::write_psplat(good / "scene.psplat", splat_test::make_test_scene(4)).ok());

    // The gallery lists all three: a broken file is still a scene folder.
    const splat::BootAlbum boot = splat::boot_album(root);
    REQUIRE(boot.scenes.size() == 3);
    CHECK(boot.index == 0);
    std::vector<Path> folders;
    for (const splat::SceneEntry& scene : boot.scenes) folders.push_back(scene.folder);

    const splat::BootSceneLoad load = splat::load_boot_scene(folders, boot.index);
    REQUIRE(load.loaded);
    CHECK(load.index == 2);
    CHECK(load.scene.scene.count() == 4);
    REQUIRE(load.skipped.size() == 2);
    CHECK(load.skipped[0].index == 0);
    CHECK(load.skipped[0].name == "a-garbage");
    CHECK(!load.skipped[0].error.empty());
    CHECK(load.skipped[1].index == 1);
    CHECK(load.skipped[1].name == "b-truncated");
    CHECK(!load.skipped[1].error.empty());

    // The playlist the viewer builds from that album lands on the scene that
    // read, not on the corrupt first one.
    splat::ScenePlaylist playlist = playlist_of(boot);
    playlist.set_index(load.index);
    CHECK(playlist.current().folder.filename() == "c-good");
}

TEST_CASE("gallery boot with every scene unreadable yields the empty-view path") {
    splat_test::TempDir temp("boot-all-corrupt");
    const Path root = temp.path() / "scenes";
    write_scene(root / "trip" / "a-garbage");
    write_scene(root / "trip" / "b-garbage");

    const splat::BootAlbum boot = splat::boot_album(root);
    REQUIRE(boot.scenes.size() == 2);
    std::vector<Path> folders;
    for (const splat::SceneEntry& scene : boot.scenes) folders.push_back(scene.folder);

    const splat::BootSceneLoad load = splat::load_boot_scene(folders, boot.index);
    CHECK_FALSE(load.loaded);
    CHECK(load.index == 0);
    CHECK(load.scene.scene.empty());
    REQUIRE(load.skipped.size() == 2);
    CHECK(load.skipped[0].name == "a-garbage");
    CHECK(load.skipped[1].name == "b-garbage");

    // vr_mode.cpp drops the playlist here, which is exactly the empty-gallery
    // session: the menu opens on the folders home over the empty view and
    // ResumeScene has no scene to return to.
    splat::ScenePlaylist playlist = playlist_of(boot);
    playlist = splat::ScenePlaylist{};
    CHECK(playlist.empty());
}
