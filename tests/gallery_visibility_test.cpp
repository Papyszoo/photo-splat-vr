// The gallery under a visible-folders policy: which albums and scenes a listing
// returns, what the count and the cover mean when scenes are hidden, and that a
// scene follows the album or location it is listed under (stage U1: source
// folders no longer decide), plus the boot playlist.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "common/path.h"
#include "gallery/gallery.h"
#include "gallery/visibility.h"
#include "render/scene_playlist.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "test_util.h"
#include "vfs/io.h"

namespace {

using splat::Path;
using splat::SceneMeta;
using splat::Status;
using splat::Visibility;
using splat::VisibilityContext;
using splat::VisibilityKey;
namespace vfs = splat::vfs;

// A scene folder that a listing accepts: scene.psplat plus meta.json. The
// source key and the imported marker are the two things a visibility test sets.
void make_scene(const Path& folder, const std::string& source_folder, bool imported,
                bool readable = true) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const splat::SplatScene scene = splat_test::make_test_scene(3);
    REQUIRE(splat::write_psplat(splat::scene_files(folder).splat, scene, 100.0f, 64, 48).ok());
    if (!readable) {
        // A meta.json that cannot be parsed: "other", never the album.
        const std::string text = "{ not json";
        REQUIRE(splat::write_bytes_file(splat::scene_files(folder).meta,
                                        std::vector<uint8_t>(text.begin(), text.end()))
                    .ok());
        return;
    }
    SceneMeta meta;
    meta.backend = "test";
    meta.intrinsics = splat::Intrinsics{64, 48, 100.0f, 100.0f, 32.0f, 24.0f};
    meta.splat_count = static_cast<uint32_t>(scene.count());
    meta.source_folder = source_folder;
    if (imported) {
        meta.origin_format = "splat";
        meta.origin_count = 3;
    }
    REQUIRE(splat::write_scene_meta(splat::scene_files(folder).meta, meta).ok());
}

void add_thumb(const Path& folder) {
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    REQUIRE(splat::write_bytes_file(splat::scene_files(folder).thumbnail,
                                    std::vector<uint8_t>(jpeg, jpeg + sizeof jpeg))
                .ok());
}

Visibility all_visible(const Path& root) {
    VisibilityContext context;
    context.scenes_root = root;
    return Visibility::all_visible(context);
}

// A minimal in-memory file system under "fake" for the network-library case:
// only reading, stating and listing are needed.
class FakeFileSystem : public vfs::IFileSystem {
public:
    std::string id() const override { return "fake"; }
    std::string describe() const override { return "fake:/"; }
    bool writable() const override { return false; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative,
                                             size_t /*max_bytes*/) override {
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }

    Status write(const std::string& /*relative*/, const uint8_t* /*data*/,
                 size_t /*size*/) override {
        return Status::error("fake: read only");
    }

    Status list(const std::string& relative, std::vector<vfs::DirEntry>* entries) override {
        const std::string prefix = relative.empty() ? std::string() : relative + "/";
        std::set<std::string> seen;
        for (const auto& file : files) {
            if (file.first.size() <= prefix.size() ||
                file.first.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const std::string rest = file.first.substr(prefix.size());
            const std::size_t slash = rest.find('/');
            const std::string name = slash == std::string::npos ? rest : rest.substr(0, slash);
            if (!seen.insert(name).second) continue;
            vfs::DirEntry entry;
            entry.name = name;
            entry.directory = slash != std::string::npos;
            if (!entry.directory) entry.size = file.second.size();
            entries->push_back(entry);
        }
        return Status::success();
    }

    Status stat(const std::string& relative, vfs::FileStat* out) override {
        *out = vfs::FileStat{};
        const auto found = files.find(relative);
        if (found != files.end()) {
            out->exists = true;
            out->regular = true;
            out->size = found->second.size();
            return Status::success();
        }
        // Anything that prefixes a file is a directory.
        const std::string prefix = relative.empty() ? std::string() : relative + "/";
        for (const auto& file : files) {
            if (file.first.compare(0, prefix.size(), prefix) == 0) {
                out->exists = true;
                out->directory = true;
                break;
            }
        }
        return Status::success();
    }

    Status make_dirs(const std::string& /*relative*/) override {
        return Status::error("fake: read only");
    }
    Status remove_file(const std::string& /*relative*/) override {
        return Status::error("fake: read only");
    }
    Status remove_tree(const std::string& /*relative*/) override {
        return Status::error("fake: read only");
    }

    std::map<std::string, std::vector<uint8_t>> files;
};

struct Fixture {
    splat_test::TempDir dir{"gallery-vis"};
    Path root = dir.path() / "scenes";

    Fixture() {
        std::error_code error;
        std::filesystem::create_directories(root, error);
    }

    // Installs a policy that ticks exactly `keys`.
    void install(std::initializer_list<VisibilityKey> keys, bool enabled = true) {
        Visibility state = all_visible(root);
        state.enabled = enabled;
        for (const VisibilityKey& key : keys) state.allowed.push_back(key);
        std::sort(state.allowed.begin(), state.allowed.end());
        // The previous scope goes first: its destructor restores the policy that
        // was there before it, which would undo this install.
        scope_.reset();
        scope_ = std::make_unique<splat::VisibilityScope>(state);
    }

private:
    std::unique_ptr<splat::VisibilityScope> scope_;
};

std::vector<std::string> albums_of(const std::vector<splat::AlbumEntry>& albums) {
    std::vector<std::string> names;
    for (const splat::AlbumEntry& album : albums) names.push_back(album.name);
    return names;
}

std::vector<std::string> scenes_of(const std::vector<splat::SceneEntry>& scenes) {
    std::vector<std::string> names;
    for (const splat::SceneEntry& scene : scenes) names.push_back(scene.name);
    return names;
}

}  // namespace

TEST_CASE("gallery_visibility omits an unticked album entirely") {
    Fixture fixture;
    make_scene(fixture.root / "Family A" / "scene-a", "album:Family A", false);
    make_scene(fixture.root / "Family B" / "scene-b", "album:Family B", false);
    fixture.install({VisibilityKey::album("Family A")});

    const std::vector<splat::AlbumEntry> albums = splat::list_albums(fixture.root);
    CHECK(albums_of(albums) == std::vector<std::string>{"Family A"});
    // Unsorted is not ticked, so it is gone too.
    CHECK(scenes_of(splat::list_scenes(fixture.root, "Family B")).empty());
    CHECK(scenes_of(splat::list_scenes(fixture.root, "Family A")) ==
          std::vector<std::string>{"scene-a"});
}

TEST_CASE("gallery_visibility counts and covers only the visible scenes") {
    Fixture fixture;
    // Family A: a hidden scene (in an unticked album, with a thumb) and a
    // visible one. The count is 1 and the cover is the visible scene, never the
    // hidden picture. Family C is ticked and empty: count 0 and no cover, drawn
    // as an empty folder.
    make_scene(fixture.root / "Family B" / "a-hidden", "album:Family B", false);
    add_thumb(fixture.root / "Family B" / "a-hidden");
    make_scene(fixture.root / "Family A" / "a-visible", "album:Family A", false);
    std::error_code error;
    std::filesystem::create_directories(fixture.root / "Family C", error);  // ticked, empty
    fixture.install({VisibilityKey::album("Family A"), VisibilityKey::album("Family C")});

    const std::vector<splat::AlbumEntry> albums = splat::list_albums(fixture.root);
    REQUIRE(albums.size() == 2);
    CHECK(albums[0].name == "Family A");
    CHECK(albums[0].scene_count == 1);
    CHECK(albums[0].cover.filename().string() == "a-visible");
    CHECK(albums[1].name == "Family C");
    CHECK(albums[1].scene_count == 0);
    CHECK(albums[1].cover.empty());

    // The wide list still sees every album and every scene: the Visible folders
    // screen must offer the hidden ones.
    const std::vector<splat::AlbumEntry> everything = splat::list_albums_all(fixture.root);
    CHECK(albums_of(everything).size() == 4);  // Unsorted, Family A, B, C
    CHECK(everything[0].name == splat::kAllScenesAlbumName);
}

TEST_CASE("gallery_visibility a moved scene follows the album it is listed under") {
    Fixture fixture;
    make_scene(fixture.root / "Family A" / "scene-a", "album:Family A", false);
    // The scene was moved into Family B; its meta.json still names Family A.
    make_scene(fixture.root / "Family B" / "moved", "album:Family A", false);
    fixture.install({VisibilityKey::album("Family B")});

    CHECK(scenes_of(splat::list_scenes(fixture.root, "Family B")) ==
          std::vector<std::string>{"moved"});
    // Family A is unticked, so its scene is hidden even though the moved
    // scene's meta.json names Family A.
    CHECK(scenes_of(splat::list_scenes(fixture.root, "Family A")).empty());
}

TEST_CASE("gallery_visibility imported, old and damaged-meta scenes follow their album") {
    Fixture fixture;
    // An imported scene, a converted scene with no source key and a scene whose
    // meta.json cannot be read: all three are listed under the album they sit
    // in (stage U1 dropped the source and origin rules).
    make_scene(fixture.root / "Album" / "imported", "", true);
    make_scene(fixture.root / "Album" / "converted", "", false);
    make_scene(fixture.root / "Album" / "broken", "", false, false);

    fixture.install({VisibilityKey::album("Album")});
    const std::vector<splat::SceneEntry> all = splat::list_scenes(fixture.root, "Album");
    CHECK(scenes_of(all) == (std::vector<std::string>{"broken", "converted", "imported"}));
    // meta_readable still reports the read, for the display fields.
    REQUIRE(all.size() == 3);
    CHECK_FALSE(all[0].meta_readable);
    CHECK(all[1].meta_readable);
    CHECK(all[2].meta_readable);

    // An unticked album hides all three.
    fixture.install({VisibilityKey::album("Other")});
    CHECK(scenes_of(splat::list_scenes(fixture.root, "Album")).empty());
}

TEST_CASE("gallery_visibility a scene on a network library follows its location") {
    auto fake = std::make_shared<FakeFileSystem>();
    const std::string meta_hidden =
        R"({"format_version": 1, "backend": "test", "intrinsics": {"width": 64,
            "height": 48, "fx": 100, "fy": 100, "cx": 32, "cy": 24},
            "seconds_taken": 1.0, "splat_count": 3, "source_folder": "album:Family B"})";
    const std::string meta_old =
        R"({"format_version": 1, "backend": "test", "intrinsics": {"width": 64,
            "height": 48, "fx": 100, "fy": 100, "cx": 32, "cy": 24},
            "seconds_taken": 1.0, "splat_count": 3})";
    const std::string meta_imported =
        R"({"format_version": 1, "backend": "import", "intrinsics": {"width": 64,
            "height": 48, "fx": 100, "fy": 100, "cx": 32, "cy": 24},
            "seconds_taken": 1.0, "splat_count": 3, "origin": {"format": "splat",
            "count": 3}})";
    const std::vector<uint8_t> psplat(1, 0);
    const auto put = [&fake](const char* path, const std::string& text) {
        fake->files[path] = std::vector<uint8_t>(text.begin(), text.end());
    };
    put("scenes/hidden/meta.json", meta_hidden);
    put("scenes/old/meta.json", meta_old);
    put("scenes/imported/meta.json", meta_imported);
    fake->files["scenes/hidden/scene.psplat"] = psplat;
    fake->files["scenes/old/scene.psplat"] = psplat;
    fake->files["scenes/imported/scene.psplat"] = psplat;
    vfs::file_systems().add(fake);

    const Path root("fake:/scenes");
    VisibilityContext context;
    context.scenes_root = Path("/data/ps5-splat-vr/scenes");
    {
        // The location is one tick: every scene on it is listed, whatever its
        // meta.json says (source folder, origin or no source at all).
        Visibility state = Visibility::all_visible(context);
        state.enabled = true;
        state.allowed = {VisibilityKey::location("fake")};
        splat::VisibilityScope scope(state);
        CHECK(scenes_of(splat::list_scenes(root, "")) ==
              (std::vector<std::string>{"hidden", "imported", "old"}));
        CHECK(splat::current_visibility()->allows_scene_folder(Path("fake:/scenes/hidden")));
        CHECK(splat::current_visibility()->allows_scene_folder(Path("fake:/scenes/old")));
    }
    {
        // Without the location tick nothing on the share is listed, even with
        // "other" ticked.
        Visibility state = Visibility::all_visible(context);
        state.enabled = true;
        state.allowed = {VisibilityKey::other()};
        splat::VisibilityScope scope(state);
        CHECK(scenes_of(splat::list_scenes(root, "")).empty());
        CHECK_FALSE(splat::current_visibility()->allows_scene_folder(Path("fake:/scenes/old")));
    }
    vfs::file_systems().remove("fake");
}

TEST_CASE("gallery_visibility boot album never picks a hidden preferred scene") {
    Fixture fixture;
    make_scene(fixture.root / "Family A" / "a-one", "album:Family A", false);
    make_scene(fixture.root / "Family B" / "b-one", "album:Family B", false);
    make_scene(fixture.root / "Family B" / "b-two", "album:Family B", false);
    fixture.install({VisibilityKey::album("Family B")});

    // The remembered scene is hidden: the boot picks the first visible one.
    const splat::BootAlbum boot = splat::boot_album(fixture.root, "a-one");
    CHECK(boot.album == "Family B");
    REQUIRE(boot.scenes.size() == 2);
    CHECK(boot.scenes[boot.index].name == "b-one");
    // Without a preference it is the gallery's first visible scene too.
    const splat::BootAlbum plain = splat::boot_album(fixture.root);
    CHECK(plain.album == "Family B");
    CHECK(plain.scenes[plain.index].name == "b-one");
}

TEST_CASE("gallery_visibility load_boot_scene and the playlist skip a hidden scene") {
    Fixture fixture;
    make_scene(fixture.root / "Family A" / "a-one", "album:Family A", false);
    make_scene(fixture.root / "Family B" / "b-one", "album:Family B", false);
    fixture.install({VisibilityKey::album("Family B")});

    const std::vector<Path> folders = {fixture.root / "Family A" / "a-one",
                                       fixture.root / "Family B" / "b-one"};
    const splat::BootSceneLoad load = splat::load_boot_scene(folders);
    CHECK(load.loaded);
    CHECK(load.index == 1);
    CHECK(load.skipped.empty());  // a hidden scene is skipped silently, no name

    const splat::Result<splat::ScenePlaylist> all =
        splat::ScenePlaylist::from_folder(fixture.root / "Family A");
    CHECK_FALSE(all.ok());  // nothing visible under it
    const splat::Result<splat::ScenePlaylist> visible =
        splat::ScenePlaylist::from_folder(fixture.root / "Family B");
    REQUIRE(visible.ok());
    CHECK(visible.value().size() == 1);

    // from_entries filters too: a stale playlist cannot open one.
    const splat::Result<splat::ScenePlaylist> stale = splat::ScenePlaylist::from_entries(
        {splat::PlaylistEntry{fixture.root / "Family A" / "a-one",
                              fixture.root / "Family A" / "a-one" / "scene.psplat"}});
    CHECK_FALSE(stale.ok());
    const splat::Result<splat::ScenePlaylist> kept = splat::ScenePlaylist::from_entries(
        {{fixture.root / "Family B" / "b-one",
          fixture.root / "Family B" / "b-one" / "scene.psplat"},
         {fixture.root / "Family A" / "a-one",
          fixture.root / "Family A" / "a-one" / "scene.psplat"}});
    REQUIRE(kept.ok());
    CHECK(kept.value().size() == 1);
    CHECK(kept.value().current().folder.filename().string() == "b-one");
}
