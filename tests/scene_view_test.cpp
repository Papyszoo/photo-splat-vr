// The scene's stored view after rotation left the viewer (DECISIONS.md,
// "Change 2026-11-05"): only an imported scene's own scale is kept in meta.json
// ("view": {"scale": s}), plus the <state>/views.json fallback for a folder that
// refuses the write. A "view"."rotation" written by the Track R build still
// loads (it is ignored) and the next save drops it, keeping the scale and every
// key this build does not know.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/scene_folder.h"
#include "scene/scene_view.h"
#include "test_util.h"

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// A scene folder with a real meta.json, written the way a conversion writes it.
// `imported` writes the "origin" object an import carries: only an imported
// scene stores its own scale (DECISIONS.md, 2026-11-05).
splat::Path make_scene(const std::filesystem::path& folder, const std::string& backend,
                       bool imported = false) {
    splat::SceneMeta meta;
    meta.backend = backend;
    meta.seconds_taken = 3.5;
    meta.source = "inbox";
    meta.intrinsics.fx = 900.0f;
    meta.intrinsics.fy = 900.0f;
    meta.intrinsics.cx = 640.0f;
    meta.intrinsics.cy = 436.0f;
    if (imported) {
        meta.origin_format = "splat";
        meta.origin_count = 9;
    }
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::SplatScene scene = splat_test::make_test_scene(9);
    const splat::Status status = splat::write_scene_folder(folder, scene, meta, jpeg, jpeg);
    REQUIRE_MESSAGE(status.ok(), status.message());
    return splat::Path(folder);
}

splat::SceneView scaled(float scale) {
    splat::SceneView view;
    view.scale = scale;
    return view;
}

}  // namespace

TEST_CASE("scene view: an imported scene's scale is saved, read back and removed at 1") {
    splat_test::TempDir temp("scene-view");
    const splat::Path folder = make_scene(temp.path() / "scenes" / "a", "import", true);
    const splat::Path state = temp.path() / "state";
    const splat::SceneViewStore store{state};

    // Nothing stored yet: scale 1, not an error.
    const auto before = store.load(folder);
    REQUIRE_MESSAGE(before.ok(), before.message());
    CHECK(before.value().scale == doctest::Approx(1.0f));

    bool local_only = true;
    const splat::Status saved = store.save(folder, scaled(0.5f), &local_only);
    REQUIRE_MESSAGE(saved.ok(), saved.message());
    CHECK_FALSE(local_only);
    CHECK_FALSE(std::filesystem::exists(state / "views.json"));

    const auto after = store.load(folder);
    REQUIRE_MESSAGE(after.ok(), after.message());
    CHECK(after.value().scale == doctest::Approx(0.5f));

    // The file itself says so, in the documented shape, and carries no rotation.
    const nlohmann::json root = nlohmann::json::parse(read_text(splat::scene_files(folder).meta));
    REQUIRE(root.contains("view"));
    CHECK(root.at("view").at("scale").get<float>() == doctest::Approx(0.5f));
    CHECK_FALSE(root.at("view").contains("rotation"));

    // Scale 1 removes the key again: a scene nobody scaled keeps no "view".
    REQUIRE(store.save(folder, splat::SceneView{}, nullptr).ok());
    const nlohmann::json cleared = nlohmann::json::parse(read_text(splat::scene_files(folder).meta));
    CHECK_FALSE(cleared.contains("view"));
    CHECK(store.load(folder).value().scale == doctest::Approx(1.0f));
}

TEST_CASE("scene view: a legacy rotation is ignored on load and dropped by a save") {
    splat_test::TempDir temp("scene-view-legacy");
    const splat::Path folder = make_scene(temp.path() / "scenes" / "old", "sharp");
    const splat::Path state = temp.path() / "state";
    const splat::SceneViewStore store{state};

    // What a Track R build wrote: a quarter turn plus a scale, and a key this
    // build does not know.
    nlohmann::json root = nlohmann::json::parse(read_text(splat::scene_files(folder).meta));
    root["view"] = {{"rotation", {0.0f, std::sqrt(0.5f), 0.0f, std::sqrt(0.5f)}},
                    {"scale", 0.25f}};
    root["future_field"] = {{"nested", {1, 2, 3}}, {"name", "kept"}};
    write_text(splat::scene_files(folder).meta, root.dump(2) + "\n");

    // The rotation is ignored (there is no field for it any more); the scale
    // still loads, so a scene the user scaled on the share keeps its size.
    const auto loaded = store.load(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scale == doctest::Approx(0.25f));

    // A save drops "rotation", keeps "scale" and every unknown key.
    REQUIRE_MESSAGE(store.save(folder, scaled(0.25f), nullptr).ok(), "save");
    const nlohmann::json after =
        nlohmann::json::parse(read_text(splat::scene_files(folder).meta));
    CHECK_FALSE(after.at("view").contains("rotation"));
    CHECK(after.at("view").at("scale").get<float>() == doctest::Approx(0.25f));
    CHECK(after.at("future_field") == root.at("future_field"));

    const auto reread = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(reread.ok(), reread.message());
    CHECK(reread.value().meta.backend == "sharp");
    CHECK(reread.value().meta.source == "inbox");
    CHECK(reread.value().meta.splat_count == 9);
}

TEST_CASE("scene view: a save keeps the visible-folders source key") {
    splat_test::TempDir temp("scene-view-source");
    const splat::Path folder = make_scene(temp.path() / "scenes" / "source", "sharp", true);
    const splat::Path meta = splat::scene_files(folder).meta;
    // What the job writes at conversion/import time (stage H).
    nlohmann::json root = nlohmann::json::parse(read_text(meta));
    root["source_folder"] = "source:usb0";
    write_text(meta, root.dump(2) + "\n");

    const splat::SceneViewStore store{temp.path() / "state"};
    REQUIRE_MESSAGE(store.save(folder, scaled(0.5f), nullptr).ok(), "save");
    const nlohmann::json after = nlohmann::json::parse(read_text(meta));
    CHECK(after.at("source_folder").get<std::string>() == "source:usb0");
    CHECK(after.at("view").at("scale").get<float>() == doctest::Approx(0.5f));
    const auto reread = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(reread.ok(), reread.message());
    CHECK(reread.value().meta.source_folder == "source:usb0");
    CHECK(reread.value().meta.origin_present);

    // A meta.json written before stage H has no key: a save neither adds nor
    // rewrites it, and the file stays byte-identical.
    const splat::Path old_folder =
        make_scene(temp.path() / "scenes" / "old-source", "sharp");
    const splat::Path old_meta = splat::scene_files(old_folder).meta;
    const std::string before = read_text(old_meta);
    REQUIRE_MESSAGE(store.save(old_folder, splat::SceneView{}, nullptr).ok(), "no-op save");
    CHECK(read_text(old_meta) == before);
}

TEST_CASE("scene view: a broken meta.json is an error, not a silent identity") {
    splat_test::TempDir temp("scene-view-broken");
    const splat::Path folder = make_scene(temp.path() / "scenes" / "broken", "sharp");
    const splat::SceneViewStore store{temp.path() / "state"};

    write_text(splat::scene_files(folder).meta, "{not json");
    CHECK_FALSE(store.load(folder).ok());
    CHECK_FALSE(store.save(folder, scaled(0.5f), nullptr).ok());
}

TEST_CASE("scene view: a folder that refuses the write falls back to the state cache") {
    splat_test::TempDir temp("scene-view-cache");
    const splat::Path folder = make_scene(temp.path() / "scene", "import", true);
    const splat::Path state = temp.path() / "state";
    const splat::SceneViewStore store{state};

    // A read-only scene folder: the meta.json write cannot happen, so the scale
    // must land in <state>/views.json and still read back.
    std::filesystem::permissions(folder, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec |
                                             std::filesystem::perms::group_read |
                                             std::filesystem::perms::group_exec |
                                             std::filesystem::perms::others_read |
                                             std::filesystem::perms::others_exec);
    const std::string meta_before = read_text(splat::scene_files(folder).meta);

    bool local_only = false;
    const splat::Status saved = store.save(folder, scaled(0.4f), &local_only);
    // Root can still write into a read-only folder on some systems; the
    // fallback is only claimed when the write really was refused.
    if (saved.ok() && local_only) {
        CHECK(std::filesystem::exists(state / "views.json"));
        CHECK(read_text(splat::scene_files(folder).meta) == meta_before);
        const auto view = store.load(folder);
        REQUIRE_MESSAGE(view.ok(), view.message());
        CHECK(view.value().scale == doctest::Approx(0.4f));
    } else {
        MESSAGE("this user can write a read-only scene folder; the fallback is not exercised");
        CHECK_MESSAGE(saved.ok(), saved.message());
    }

    std::filesystem::permissions(folder, std::filesystem::perms::owner_all);

    // And a folder that is not a scene at all is an error, not a cache entry.
    local_only = true;
    const splat::Status missing = store.save(temp.path() / "gone", scaled(0.5f), &local_only);
    CHECK_FALSE(missing.ok());
    CHECK_FALSE(local_only);
}

TEST_CASE("scene view: a saved scale is kept when the folder starts refusing writes") {
    splat_test::TempDir temp("scene-view-shadow");
    const splat::Path folder = make_scene(temp.path() / "scene", "import", true);
    const splat::Path state = temp.path() / "state";
    const splat::SceneViewStore store{state};

    REQUIRE_MESSAGE(store.save(folder, scaled(0.8f), nullptr).ok(), "first save");

    std::filesystem::permissions(folder, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec |
                                             std::filesystem::perms::group_read |
                                             std::filesystem::perms::group_exec |
                                             std::filesystem::perms::others_read |
                                             std::filesystem::perms::others_exec);
    bool local_only = false;
    const splat::Status second = store.save(folder, scaled(0.6f), &local_only);
    if (second.ok() && local_only) {
        // The share's own file still says 0.8; the newer 0.6 must win.
        const auto view = store.load(folder);
        REQUIRE_MESSAGE(view.ok(), view.message());
        CHECK(view.value().scale == doctest::Approx(0.6f));
    } else {
        MESSAGE("this user can write a read-only scene folder; the shadow case is not exercised");
        CHECK_MESSAGE(second.ok(), second.message());
    }
    std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
}
