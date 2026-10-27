// W31: the bundled sample scenes are copied into the gallery exactly once.
//
// The seed logic itself is desktop code (core/gallery/samples.cpp); these tests
// drive it over temp folders, so they need no package and no console.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "data_dir.h"
#include "gallery/gallery.h"
#include "gallery/samples.h"
#include "scene/scene_folder.h"
#include "test_util.h"

using splat::Path;

namespace {

namespace fs = std::filesystem;

void write_text(const Path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const Path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool path_exists(const Path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

// A minimal scene folder: the gallery only checks that scene.psplat is a
// regular file, and the copy is byte-for-byte, so arbitrary contents are fine.
Path write_bundle_scene(const Path& bundle, const std::string& name) {
    const Path folder = bundle / name;
    fs::create_directories(folder);
    write_text(folder / "scene.psplat", "splat-" + name);
    write_text(folder / "meta.json", "{\"backend\":\"depth-anything-v2-small\"}");
    write_text(folder / "original.jpg", "jpeg-" + name);
    return folder;
}

}  // namespace

TEST_CASE("samples seed copies every bundled scene once and writes the marker") {
    splat_test::TempDir temp("samples-seed");
    const Path bundle = temp.path() / "bundle";
    const Path scenes = temp.path() / "scenes";
    const Path marker = temp.path() / "data" / "state" / "samples.seeded";
    write_bundle_scene(bundle, "Living room");
    write_bundle_scene(bundle, "York street");

    const splat::SamplesSeed first = splat::seed_samples(bundle, scenes, marker);
    CHECK(first.ok());
    CHECK_FALSE(first.already_seeded);
    CHECK(first.scenes == 2);
    CHECK(path_exists(marker));

    // The gallery sees one album, "Samples", holding both scenes.
    const std::vector<splat::AlbumEntry> albums = splat::list_albums(scenes);
    REQUIRE(albums.size() == 2);  // Unsorted first, then the album
    CHECK(albums[1].name == splat::kSamplesAlbumName);
    CHECK(albums[1].scene_count == 2);

    const std::vector<splat::SceneEntry> seeded = splat::list_scenes(scenes, splat::kSamplesAlbumName);
    REQUIRE(seeded.size() == 2);
    CHECK(seeded[0].name == "Living room");
    CHECK(seeded[1].name == "York street");
    // Every file came along, byte for byte.
    CHECK(read_text(scenes / splat::kSamplesAlbumName / "Living room" / "scene.psplat") ==
          "splat-Living room");
    CHECK(read_text(scenes / splat::kSamplesAlbumName / "York street" / "original.jpg") ==
          "jpeg-York street");

    // A second start is a no-op.
    const splat::SamplesSeed second = splat::seed_samples(bundle, scenes, marker);
    CHECK(second.ok());
    CHECK(second.already_seeded);
    CHECK(second.scenes == 0);
}

TEST_CASE("samples seed does not bring a deleted sample back") {
    splat_test::TempDir temp("samples-delete");
    const Path bundle = temp.path() / "bundle";
    const Path scenes = temp.path() / "scenes";
    const Path marker = temp.path() / "data" / "samples.seeded";
    write_bundle_scene(bundle, "Living room");
    write_bundle_scene(bundle, "Sailboats");

    REQUIRE(splat::seed_samples(bundle, scenes, marker).scenes == 2);
    // The user removes one scene, then the whole album; neither returns.
    REQUIRE(splat::delete_scene(scenes, scenes / splat::kSamplesAlbumName / "Sailboats").ok());
    const splat::SamplesSeed after_scene = splat::seed_samples(bundle, scenes, marker);
    CHECK(after_scene.already_seeded);
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName / "Sailboats"));
    CHECK(splat::list_scenes(scenes, splat::kSamplesAlbumName).size() == 1);

    REQUIRE(splat::delete_scene(scenes, scenes / splat::kSamplesAlbumName / "Living room").ok());
    REQUIRE(splat::delete_album(scenes, splat::kSamplesAlbumName).ok());
    const splat::SamplesSeed after_album = splat::seed_samples(bundle, scenes, marker);
    CHECK(after_album.already_seeded);
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName));
}

TEST_CASE("samples seed writes no marker when the package has no samples") {
    splat_test::TempDir temp("samples-none");
    const Path scenes = temp.path() / "scenes";
    const Path marker = temp.path() / "data" / "samples.seeded";

    const splat::SamplesSeed seed = splat::seed_samples(temp.path() / "missing", scenes, marker);
    CHECK(seed.ok());
    CHECK_FALSE(seed.already_seeded);
    CHECK(seed.scenes == 0);
    CHECK_FALSE(path_exists(marker));
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName));

    // A package that gains samples later still seeds on the next start.
    write_bundle_scene(temp.path() / "missing", "Living room");
    const splat::SamplesSeed later = splat::seed_samples(temp.path() / "missing", scenes, marker);
    CHECK(later.ok());
    CHECK(later.scenes == 1);
    CHECK(path_exists(marker));
}

TEST_CASE("samples seed never overwrites an existing scene folder") {
    splat_test::TempDir temp("samples-dup");
    const Path bundle = temp.path() / "bundle";
    const Path scenes = temp.path() / "scenes";
    const Path marker = temp.path() / "data" / "samples.seeded";
    write_bundle_scene(bundle, "Living room");
    write_bundle_scene(bundle, "York street");

    // The user already has a folder of that name in the Samples album: its
    // contents win, and only the other scene is copied.
    const Path mine = scenes / splat::kSamplesAlbumName / "Living room";
    fs::create_directories(mine);
    write_text(mine / "scene.psplat", "user-owned");

    const splat::SamplesSeed seed = splat::seed_samples(bundle, scenes, marker);
    CHECK(seed.ok());
    CHECK(seed.scenes == 1);
    CHECK(read_text(mine / "scene.psplat") == "user-owned");
    CHECK(path_exists(scenes / splat::kSamplesAlbumName / "York street" / "scene.psplat"));
}

TEST_CASE("samples seed copies only folders the gallery calls a scene") {
    splat_test::TempDir temp("samples-junk");
    const Path bundle = temp.path() / "bundle";
    const Path scenes = temp.path() / "scenes";
    const Path marker = temp.path() / "data" / "samples.seeded";
    write_bundle_scene(bundle, "Living room");
    // A stray folder (or file) in the bundle is not a scene and stays behind.
    fs::create_directories(bundle / "README");
    write_text(bundle / "CREDITS.md", "# credits");

    const splat::SamplesSeed seed = splat::seed_samples(bundle, scenes, marker);
    CHECK(seed.ok());
    CHECK(seed.scenes == 1);
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName / "README"));
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName / "CREDITS.md"));
}

TEST_CASE("samples seed does not reseed when the marker cannot be examined") {
    splat_test::TempDir temp("samples-marker-read");
    const Path bundle = temp.path() / "bundle";
    const Path scenes = temp.path() / "scenes";
    const Path state = temp.path() / "data";
    const Path marker = state / "samples.seeded";
    write_bundle_scene(bundle, "Living room");
    fs::create_directories(state);

    // A directory nobody can read: asking for the marker fails (EACCES) rather
    // than answering "absent". That must not be read as "no marker yet", or
    // every start would copy the bundle again. Root bypasses the mode, so the
    // case says nothing there and is skipped.
    using fs::perms;
    fs::permissions(state, perms::none);
    {
        std::error_code probe_error;
        if (fs::exists(marker, probe_error) || !probe_error) {
            fs::permissions(state, perms::owner_all);
            MESSAGE("the state directory is readable for this user; skipped");
            return;
        }
    }

    const splat::SamplesSeed first = splat::seed_samples(bundle, scenes, marker);
    CHECK(first.already_seeded);
    CHECK_FALSE(first.ok());
    CHECK(first.scenes == 0);
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName));

    // The next start reaches the same verdict: still no copy, no crash.
    const splat::SamplesSeed second = splat::seed_samples(bundle, scenes, marker);
    CHECK(second.already_seeded);
    CHECK(second.scenes == 0);
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName));

    // Once the marker can be examined and written, the seed runs normally.
    fs::permissions(state, perms::owner_all);
    const splat::SamplesSeed later = splat::seed_samples(bundle, scenes, marker);
    CHECK(later.ok());
    CHECK(later.scenes == 1);
    CHECK(path_exists(marker));
}

TEST_CASE("samples seed copies nothing when the marker cannot be written") {
    splat_test::TempDir temp("samples-marker-write");
    const Path bundle = temp.path() / "bundle";
    const Path scenes = temp.path() / "scenes";
    const Path state = temp.path() / "data" / "state";
    const Path marker = state / "samples.seeded";
    write_bundle_scene(bundle, "Living room");
    write_bundle_scene(bundle, "York street");
    fs::create_directories(state);

    // A directory nobody can write: the marker's own open has to fail. If the
    // process can write anyway (root), the case says nothing and is skipped.
    using fs::perms;
    fs::permissions(state, perms::owner_read | perms::owner_exec | perms::group_read |
                               perms::group_exec | perms::others_read | perms::others_exec);
    {
        std::ofstream probe(state / "probe");
        if (probe) {
            probe.close();
            fs::remove(state / "probe");
            fs::permissions(state, perms::owner_all);
            MESSAGE("the state directory is writable for this user; skipped");
            return;
        }
    }

    // The claim is taken before the copy, so an unwritable marker costs a
    // failure and no scene: nothing is seeded behind a marker that cannot exist.
    const splat::SamplesSeed refused = splat::seed_samples(bundle, scenes, marker);
    CHECK_FALSE(refused.ok());
    CHECK(refused.scenes == 0);
    CHECK_FALSE(path_exists(marker));
    CHECK_FALSE(path_exists(scenes / splat::kSamplesAlbumName));

    // Once the directory takes the marker, the next start seeds normally.
    fs::permissions(state, perms::owner_all);
    const splat::SamplesSeed later = splat::seed_samples(bundle, scenes, marker);
    CHECK(later.ok());
    CHECK(later.scenes == 2);
    CHECK(path_exists(marker));
}

TEST_CASE("the bundled sample scenes, when generated, seed and load") {
    // The material is external (AGENTS.md): the build machine has it under
    // $SPLAT_DATA_DIR/assets/samples/out; a checkout without it skips.
    const Path bundle = splat_test::data_path("assets/samples/out");
    if (!fs::is_directory(bundle)) return;

    splat_test::TempDir temp("samples-real");
    const Path scenes = temp.path() / "scenes";
    const Path marker = temp.path() / "data" / "samples.seeded";
    const splat::SamplesSeed seed = splat::seed_samples(bundle, scenes, marker);
    REQUIRE_MESSAGE(seed.ok(), seed.error);
    REQUIRE(seed.scenes >= 1);

    const std::vector<splat::SceneEntry> seeded = splat::list_scenes(scenes, splat::kSamplesAlbumName);
    REQUIRE(seeded.size() == seed.scenes);
    for (const splat::SceneEntry& entry : seeded) {
        const splat::Result<splat::LoadedScene> loaded = splat::read_scene_folder(entry.folder);
        const std::string message = entry.name + ": " + loaded.message();
        REQUIRE_MESSAGE(loaded.ok(), message);
        CHECK(loaded.value().scene.count() > 0);
        CHECK(loaded.value().meta.backend == "depth-anything-v2-small");
    }
}
