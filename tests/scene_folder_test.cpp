#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/scene_folder.h"
#include "scene/scene_view.h"
#include "test_util.h"

using splat::SceneMeta;

namespace {

splat::SceneMeta make_meta() {
    SceneMeta meta;
    meta.format_version = splat::kPsplatVersion;
    meta.backend = "sharp";
    meta.intrinsics.width = 1280;
    meta.intrinsics.height = 873;
    meta.intrinsics.fx = 1074.2907f;
    meta.intrinsics.fy = 1074.2907f;
    meta.intrinsics.cx = 640.0f;
    meta.intrinsics.cy = 436.5f;
    meta.seconds_taken = 12.5;
    meta.splat_count = 123;
    return meta;
}

}  // namespace

TEST_CASE("meta.json round trip preserves every field") {
    const SceneMeta original = make_meta();
    const std::string json = splat::scene_meta_to_json(original);

    const nlohmann::json parsed = nlohmann::json::parse(json);
    CHECK(parsed.at("format_version").get<uint32_t>() == 1);
    CHECK(parsed.at("backend").get<std::string>() == "sharp");
    CHECK(parsed.at("intrinsics").at("fx").get<float>() == doctest::Approx(1074.2907f));
    CHECK(parsed.at("seconds_taken").get<double>() == doctest::Approx(12.5));

    const auto loaded = splat::scene_meta_from_json(json);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    const SceneMeta& meta = loaded.value();
    CHECK(meta.format_version == original.format_version);
    CHECK(meta.backend == original.backend);
    CHECK(meta.intrinsics.width == original.intrinsics.width);
    CHECK(meta.intrinsics.height == original.intrinsics.height);
    CHECK(meta.intrinsics.fx == original.intrinsics.fx);
    CHECK(meta.intrinsics.fy == original.intrinsics.fy);
    CHECK(meta.intrinsics.cx == original.intrinsics.cx);
    CHECK(meta.intrinsics.cy == original.intrinsics.cy);
    CHECK(meta.seconds_taken == original.seconds_taken);
    CHECK(meta.splat_count == original.splat_count);
}

TEST_CASE("meta.json carries the scene's source folder and an old file still round-trips") {
    // A scene written by stage H records the visible-folders key of the folder
    // it came from. The JSON key is optional: it is written only when set.
    SceneMeta with_source = make_meta();
    with_source.source_folder = "folder:/data/Photos";
    const std::string json = splat::scene_meta_to_json(with_source);
    const nlohmann::json parsed = nlohmann::json::parse(json);
    CHECK(parsed.at("source_folder").get<std::string>() == "folder:/data/Photos");
    const auto loaded = splat::scene_meta_from_json(json);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().source_folder == "folder:/data/Photos");

    // A meta.json written before the feature has no key: it reads as empty, and
    // writing it back is byte-identical (nothing is invented).
    const std::string old = splat::scene_meta_to_json(make_meta());
    CHECK(old.find("source_folder") == std::string::npos);
    const auto old_loaded = splat::scene_meta_from_json(old);
    REQUIRE(old_loaded.ok());
    CHECK(old_loaded.value().source_folder.empty());
    CHECK(splat::scene_meta_to_json(old_loaded.value()) == old);
}

TEST_CASE("meta.json rejects malformed input") {
    CHECK_FALSE(splat::scene_meta_from_json("not json").ok());
    CHECK_FALSE(splat::scene_meta_from_json("[]").ok());
    CHECK_FALSE(splat::scene_meta_from_json(R"({"format_version":1})").ok());
}

TEST_CASE("scene folder writes the four files and reads the scene back") {
    splat_test::TempDir temp("scene-folder");
    const auto folder = temp.path() / "photo01";
    const std::vector<uint8_t> original{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00};
    const std::vector<uint8_t> thumbnail{0xFF, 0xD8, 0xFF, 0xD9};

    const splat::SplatScene scene = splat_test::make_test_scene(23);
    SceneMeta meta = make_meta();
    meta.splat_count = 0;  // write_scene_folder fills this from the scene

    const auto status = splat::write_scene_folder(folder, scene, meta, original, thumbnail);
    REQUIRE_MESSAGE(status.ok(), status.message());

    const splat::SceneFiles files = splat::scene_files(folder);
    CHECK(std::filesystem::exists(files.original));
    CHECK(std::filesystem::exists(files.thumbnail));
    CHECK(std::filesystem::exists(files.splat));
    CHECK(std::filesystem::exists(files.meta));
    CHECK(std::filesystem::file_size(files.original) == original.size());
    CHECK(std::filesystem::file_size(files.splat) == splat::psplat_file_size(scene.count()));

    std::ifstream thumb(files.thumbnail, std::ios::binary);
    const std::vector<uint8_t> thumb_bytes((std::istreambuf_iterator<char>(thumb)),
                                           std::istreambuf_iterator<char>());
    CHECK(thumb_bytes == thumbnail);

    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == scene.count());
    CHECK(loaded.value().meta.backend == "sharp");
    CHECK(loaded.value().meta.splat_count == scene.count());
    CHECK(loaded.value().meta.intrinsics.fx == doctest::Approx(1074.2907f));
}

// A "view" object written by the Track R build still reads: this build has no
// scene rotation (DECISIONS.md, "Change 2026-11-05"), so the rotation is
// ignored, not an error, and a fresh meta.json never writes one.
TEST_CASE("meta.json ignores a legacy view rotation and writes none") {
    SceneMeta meta = make_meta();
    const nlohmann::json plain = nlohmann::json::parse(splat::scene_meta_to_json(meta));
    CHECK_FALSE(plain.contains("view"));

    // A legacy file: the reader accepts it and exposes no rotation at all.
    nlohmann::json legacy = plain;
    legacy["view"] = {{"rotation", {0.0, 0.7071068, 0.0, 0.7071068}}, {"scale", 0.5}};
    const auto loaded = splat::scene_meta_from_json(legacy.dump());
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().backend == meta.backend);
    CHECK(loaded.value().splat_count == meta.splat_count);
}

TEST_CASE("a saved scene view keeps the conversion's meta.json fields") {
    splat_test::TempDir temp("scene-view-meta");
    const auto folder = temp.path() / "scenes" / "kept";
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::SplatScene scene = splat_test::make_test_scene(17);
    SceneMeta meta = make_meta();
    meta.source = "inbox";
    meta.source_folder = "folder:/data/Photos";
    meta.splat_count = 0;
    // An import: only an imported scene stores its own scale (DECISIONS.md,
    // 2026-11-05). The point of the test is that the save edits only the view
    // and leaves the conversion/import fields alone.
    meta.origin_format = "splat";
    meta.origin_count = 17;
    REQUIRE_MESSAGE(splat::write_scene_folder(folder, scene, meta, jpeg, jpeg).ok(), "write");

    splat::SceneView view;
    view.scale = 0.5f;
    const splat::SceneViewStore store{temp.path() / "state"};
    bool local_only = false;
    REQUIRE_MESSAGE(store.save(splat::Path(folder), view, &local_only).ok(), "save view");
    CHECK_FALSE(local_only);

    const auto loaded = splat::read_scene_folder(splat::Path(folder));
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.backend == "sharp");
    CHECK(loaded.value().meta.source == "inbox");
    // SceneViewStore::save() edits the JSON in place, so the visible-folders key
    // a saved view must not drop is still there.
    CHECK(loaded.value().meta.source_folder == "folder:/data/Photos");
    CHECK(loaded.value().meta.intrinsics.fx == doctest::Approx(1074.2907f));
    CHECK(loaded.value().meta.intrinsics.width == 1280);
    CHECK(loaded.value().meta.splat_count == 17);
    CHECK(loaded.value().scene.count() == 17);
}

TEST_CASE("scene folder refuses the wrong splat_count") {    splat_test::TempDir temp("scene-folder-mismatch");
    const auto folder = temp.path() / "photo";
    const splat::SplatScene scene = splat_test::make_test_scene(5);
    SceneMeta meta = make_meta();
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};

    REQUIRE(splat::write_scene_folder(folder, scene, meta, jpeg, jpeg).ok());
    const auto files = splat::scene_files(folder);

    meta.splat_count = 99;
    REQUIRE(splat::write_scene_meta(files.meta, meta).ok());
    CHECK_FALSE(splat::read_scene_folder(folder).ok());
}
