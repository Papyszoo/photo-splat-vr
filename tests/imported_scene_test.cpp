// An imported scene folder: the writer, the generated thumbnail, the meta.json
// origin block, and the gallery listing a folder with no original.jpg.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/image.h"
#include "gallery/gallery.h"
#include "jobs/image_io.h"
#include "scene/imported_scene.h"
#include "scene/scene_folder.h"
#include "test_util.h"

using splat::SplatScene;

namespace {

constexpr float kSHC0 = 0.28209479177387814f;

// A scene of one colour: sh_dc set so the mean RGB is `rgb`.
SplatScene flat_scene(size_t count, float red, float green, float blue) {
    SplatScene scene;
    scene.resize(count);
    for (size_t i = 0; i < count; ++i) {
        scene.position[i * 3 + 0] = static_cast<float>(i);
        for (size_t axis = 0; axis < 3; ++axis) {
            scene.log_scale[i * 3 + axis] = -3.0f;
            scene.rotation[i * 4 + axis] = axis == 0 ? 1.0f : 0.0f;
            scene.sh_dc[i * 3 + axis] = 0.0f;
        }
        scene.rotation[i * 4 + 3] = 0.0f;
        scene.opacity_logit[i] = 0.0f;
        scene.sh_dc[i * 3 + 0] = (red - 0.5f) / kSHC0;
        scene.sh_dc[i * 3 + 1] = (green - 0.5f) / kSHC0;
        scene.sh_dc[i * 3 + 2] = (blue - 0.5f) / kSHC0;
    }
    return scene;
}

}  // namespace

TEST_CASE("write_imported_scene writes a folder read_scene_folder accepts") {
    splat_test::TempDir temp("imported-scene");
    const splat::Path folder = temp.path() / "scenes" / "robot";
    const SplatScene scene = flat_scene(5, 0.5f, 0.25f, 0.75f);

    splat::SceneMeta meta;
    meta.backend = "import";
    meta.origin_format = "splat";
    meta.origin_count = 5;
    meta.source = "usb0";

    const std::vector<uint8_t> thumb_jpeg = splat::make_import_thumb(scene, 64, 36);
    REQUIRE(!thumb_jpeg.empty());

    const splat::Status written = splat::write_imported_scene(folder, scene, meta, thumb_jpeg);
    REQUIRE_MESSAGE(written.ok(), written.message());

    CHECK(std::filesystem::exists(folder / "scene.psplat"));
    CHECK(std::filesystem::exists(folder / "thumb.jpg"));
    CHECK(std::filesystem::exists(folder / "meta.json"));
    CHECK(!std::filesystem::exists(folder / "original.jpg"));

    const splat::Result<splat::LoadedScene> loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 5);
    CHECK(loaded.value().meta.backend == "import");
    CHECK(loaded.value().meta.origin_format == "splat");
    CHECK(loaded.value().meta.origin_count == 5);
    CHECK(loaded.value().meta.source == "usb0");
    CHECK(loaded.value().meta.splat_count == 5);

    // The writer stores the count it wrote, not the one the caller passed.
    splat::SceneMeta stale = meta;
    stale.splat_count = 99;
    const splat::Path second = temp.path() / "scenes" / "stale";
    REQUIRE(splat::write_imported_scene(second, scene, stale, thumb_jpeg).ok());
    const splat::Result<splat::LoadedScene> reloaded = splat::read_scene_folder(second);
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value().meta.splat_count == 5);
}

TEST_CASE("make_import_thumb carries the scene's mean colour") {
    splat_test::TempDir temp("imported-thumb");
    const float red = 0.8f;
    const float green = 0.3f;
    const float blue = 0.15f;
    const SplatScene scene = flat_scene(16, red, green, blue);

    const std::vector<uint8_t> jpeg = splat::make_import_thumb(scene, 64, 36);
    REQUIRE(!jpeg.empty());
    const std::filesystem::path path = temp.path() / "thumb.jpg";
    splat::write_bytes_file(path, jpeg);

    const splat::Result<splat::Image> decoded = splat::decode_image(path);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    REQUIRE(decoded.value().width == 64);
    REQUIRE(decoded.value().height == 36);

    double sums[3] = {0.0, 0.0, 0.0};
    const splat::Image& image = decoded.value();
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const uint8_t* pixel = image.pixel(x, y);
            for (size_t channel = 0; channel < 3; ++channel) sums[channel] += pixel[channel];
        }
    }
    const float expected[3] = {red, green, blue};
    const double pixels = static_cast<double>(image.pixel_count());
    for (size_t channel = 0; channel < 3; ++channel) {
        const double mean = sums[channel] / pixels;
        // The mean of the decoded tile is the scene's mean sh_dc colour, within
        // JPEG quantisation and the 0..255 rounding.
        CHECK(mean == doctest::Approx(expected[channel] * 255.0).epsilon(0.03));
        CHECK(std::abs(mean - expected[channel] * 255.0) <= 6.0);
        // And the gradient really runs top to bottom.
        const uint8_t top = image.pixel(0, 0)[channel];
        const uint8_t bottom = image.pixel(0, image.height - 1)[channel];
        CHECK(bottom >= top);
    }
}

TEST_CASE("a scene folder without original.jpg is listed with its thumbnail") {
    splat_test::TempDir temp("imported-gallery");
    const splat::Path root = temp.path() / "scenes";
    const splat::Path folder = root / "robot";
    const SplatScene scene = flat_scene(3, 0.5f, 0.5f, 0.5f);

    splat::SceneMeta meta;
    meta.backend = "import";
    meta.origin_format = "spz";
    meta.origin_count = 3;
    const std::vector<uint8_t> thumb = splat::make_import_thumb(scene, 32, 18);
    REQUIRE(!thumb.empty());
    REQUIRE(splat::write_imported_scene(folder, scene, meta, thumb).ok());
    REQUIRE(!std::filesystem::exists(folder / "original.jpg"));

    const std::vector<splat::AlbumEntry> albums = splat::list_albums(root);
    REQUIRE(!albums.empty());
    CHECK(albums.front().scene_count == 1);

    const std::vector<splat::SceneEntry> scenes = splat::list_scenes(root, "");
    REQUIRE(scenes.size() == 1);
    CHECK(scenes.front().name == "robot");
    CHECK(scenes.front().has_thumb);
    CHECK(scenes.front().meta.backend == "import");
    CHECK(scenes.front().meta.origin_format == "spz");
}

TEST_CASE("meta.json origin is optional and old files still read") {
    const std::string without = R"({
  "format_version": 1,
  "backend": "sharp",
  "intrinsics": {"width": 0, "height": 0, "fx": 0, "fy": 0, "cx": 0, "cy": 0},
  "seconds_taken": 1.5,
  "splat_count": 7
})";
    const splat::Result<splat::SceneMeta> plain = splat::scene_meta_from_json(without);
    REQUIRE_MESSAGE(plain.ok(), plain.message());
    CHECK(plain.value().origin_format.empty());
    CHECK(plain.value().origin_count == 0);

    splat::SceneMeta meta;
    meta.backend = "import";
    meta.origin_format = "spz";
    meta.origin_count = 4000000;
    const std::string json = splat::scene_meta_to_json(meta);
    CHECK(json.find("\"origin\"") != std::string::npos);
    const splat::Result<splat::SceneMeta> parsed = splat::scene_meta_from_json(json);
    REQUIRE_MESSAGE(parsed.ok(), parsed.message());
    CHECK(parsed.value().origin_format == "spz");
    CHECK(parsed.value().origin_count == 4000000);

    // A conversion's meta.json has no origin block at all.
    splat::SceneMeta converted;
    converted.backend = "sharp";
    CHECK(splat::scene_meta_to_json(converted).find("origin") == std::string::npos);
}
