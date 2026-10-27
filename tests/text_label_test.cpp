// The scene label without a GPU: the font8x8 rasteriser, the backend/folder
// naming rule and the plate placement under the photo's capture frustum.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "render/scene_label.h"
#include "render/text_raster.h"

namespace {

splat::SceneMeta sharp_meta() {
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.intrinsics.width = 1280;
    meta.intrinsics.height = 720;
    meta.intrinsics.fx = 1000.0f;
    meta.intrinsics.fy = 1000.0f;
    meta.intrinsics.cx = 640.0f;
    meta.intrinsics.cy = 360.0f;
    return meta;
}

int lit_pixels(const splat::TextImage& image) {
    int count = 0;
    for (uint8_t value : image.pixels) {
        if (value != 0) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("font8x8 rasterises one 8x8 cell and integer scales") {
    const splat::TextImage one = splat::rasterise_text({"A"}, 1);
    CHECK(one.width == 8);
    CHECK(one.height == 8);
    CHECK(lit_pixels(one) > 0);
    // 'A' (0x41) has a hole: the mask is not a solid block.
    CHECK(lit_pixels(one) < 64);

    const splat::TextImage scaled = splat::rasterise_text({"A"}, 3);
    CHECK(scaled.width == 24);
    CHECK(scaled.height == 24);
    CHECK(lit_pixels(scaled) == lit_pixels(one) * 9);

    // Scale 0 is clamped to 1 instead of producing an empty image.
    CHECK(splat::rasterise_text({"A"}, 0).width == 8);

    // The glyph rows are little-endian: 'A' row 3 is 0x33, i.e. pixels 0, 1,
    // 4 and 5 on.
    const splat::TextImage a = splat::rasterise_text({"A"}, 1);
    for (int x = 0; x < 8; ++x) {
        const bool on = a.pixels[3 * 8 + x] == 255;
        CHECK(on == (x == 0 || x == 1 || x == 4 || x == 5));
    }
}

TEST_CASE("two label lines stack with a two-pixel gap") {
    const splat::TextImage two = splat::rasterise_text({"AB", "C"}, 1);
    CHECK(two.width == 16);   // two cells in the longest line
    CHECK(two.height == 18);  // 8 + 2 gap + 8
    for (int x = 0; x < 16; ++x) {
        CHECK(two.pixels[8 * 16 + x] == 0);
        CHECK(two.pixels[9 * 16 + x] == 0);
    }
    // Row 0 of the second line starts at y = 10 and has 'C' pixels.
    bool second_line = false;
    for (int y = 10; y < 18; ++y) {
        for (int x = 0; x < 8; ++x) second_line |= two.pixels[y * 16 + x] != 0;
    }
    CHECK(second_line);

    CHECK(splat::rasterise_text({}, 1).empty());
    CHECK(splat::rasterise_text({""}, 1).empty());
}

TEST_CASE("the middle-dot separator is a centred 2x2 dot") {
    // U+00B7 is what scene_label_text puts between the folder and the model;
    // the basic-Latin font has no such glyph, so it is synthesised.
    const splat::TextImage dot = splat::rasterise_text({"\xC2\xB7"}, 1);
    CHECK(dot.width == 8);
    CHECK(dot.height == 8);
    CHECK(lit_pixels(dot) == 4);
    CHECK(dot.pixels[3 * 8 + 3] == 255);
    CHECK(dot.pixels[3 * 8 + 4] == 255);
    CHECK(dot.pixels[4 * 8 + 3] == 255);
    CHECK(dot.pixels[4 * 8 + 4] == 255);

    // Unknown non-ASCII becomes '?' (0x3F), an ASCII space stays blank.
    CHECK(splat::rasterise_text({"\xE2\x98\x83"}, 1).pixels ==
          splat::rasterise_text({"?"}, 1).pixels);
    CHECK(lit_pixels(splat::rasterise_text({" "}, 1)) == 0);
}

TEST_CASE("the model name comes from meta.json, else the folder suffix") {
    using splat::scene_model_name;
    CHECK(scene_model_name("sharp", "anything") == "SHARP");
    // Spelled the way the Models screen names them.
    CHECK(scene_model_name("depth-anything-v2-small", "anything") == "DA2-Small");
    CHECK(scene_model_name("depth-anything-v2-large", "anything") == "DA2-Large");
    CHECK(scene_model_name("DA2-LARGE", "anything") == "DA2-Large");
    CHECK(scene_model_name("da2", "anything") == "DA2-Small");  // the pinned backend id

    // No usable backend: the folder suffix decides, and no suffix is SHARP.
    CHECK(scene_model_name("", "immichvr-1") == "SHARP");
    CHECK(scene_model_name("ply", "photo03-da2-small") == "DA2-Small");
    CHECK(scene_model_name("fallback", "photo03-da2-large") == "DA2-Large");
    CHECK(scene_model_name("mystery", "scene-x") == "SHARP");

    // The models compared next to SHARP in runs/compare_models write a
    // meta.json backend this label does not map, so the folder suffix names
    // them; "-da2" must not swallow "-da3".
    CHECK(scene_model_name("moge-2-vitl-normal", "immichvr-1-moge2") == "MoGe-2 L");
    CHECK(scene_model_name("da3metric-large", "immichvr-1-da3-metric-large") ==
          "DA3 Metric-L");
    CHECK(scene_model_name("da3-small", "immichvr-1-da3-small") == "DA3 Small");
    CHECK(scene_model_name("", "immichvr-1-moge2") == "MoGe-2 L");

    CHECK(splat::scene_label_text(sharp_meta(), "immichvr-1", 1, 5).title ==
          "immichvr-1 \xC2\xB7 SHARP");
    CHECK(splat::scene_label_text(sharp_meta(), "immichvr-1", 1, 5).counter == "scene 2/5");
}

TEST_CASE("the label plate sits under the capture frustum's bottom edge") {
    const splat::SceneMeta meta = sharp_meta();
    auto result = splat::make_scene_label(meta, "immichvr-1", 0, 5);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SceneLabel& label = result.value();

    // At 1.6 m the 720 px high image with cy = 360 ends at
    // 1.6 * (720 - 360) / 1000 = 0.576 m below the capture camera.
    CHECK(label.photo_bottom_y == doctest::Approx(0.576f));
    CHECK(label.top_y == doctest::Approx(label.photo_bottom_y + 0.04f));
    CHECK(label.distance == doctest::Approx(1.6f));
    CHECK(label.glyph_height == doctest::Approx(0.035f));

    // Plate top edge = centre - height / 2, on x = 0, at the reading distance.
    CHECK(label.plate.center[0] == doctest::Approx(0.0f));
    CHECK(label.plate.center[2] == doctest::Approx(1.6f));
    CHECK(label.plate.center[1] - 0.5f * label.plate.height == doctest::Approx(label.top_y));
    CHECK(label.plate.center[1] > label.photo_bottom_y);

    // Text fits inside the plate and the plate is larger than the text block.
    CHECK(label.plate.width > label.text.width);
    CHECK(label.plate.height > label.text.height);
    CHECK(label.text.center[0] == label.plate.center[0]);
    CHECK(label.text.center[1] == label.plate.center[1]);
    CHECK(label.text.center[2] == label.plate.center[2]);

    // Default quad orientation: facing the capture camera (right +x, up -y).
    CHECK(label.plate.right[0] == 1.0f);
    CHECK(label.plate.up[1] == -1.0f);
    // Light text on a dark, translucent plate.
    CHECK(label.plate.tint[3] > 0.3f);
    CHECK(label.plate.tint[0] < 0.1f);
    CHECK(label.text.tint[0] > 0.8f);
}

TEST_CASE("a long scene name shrinks the glyphs instead of the plate") {
    splat::SceneLabelLayout layout;
    layout.max_width = 0.6f;
    auto result = splat::make_scene_label(sharp_meta(),
                                          "a-very-long-scene-folder-name-0123456789", 3, 9,
                                          layout);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SceneLabel& label = result.value();
    CHECK(label.glyph_height < 0.035f);
    CHECK(label.text.width <= doctest::Approx(layout.max_width));
    CHECK(label.plate.width <= doctest::Approx(layout.max_width + 2.0f * layout.padding));
    CHECK(label.counter == "scene 4/9");

    // No intrinsics (a .ply scene): a 0.5 * distance drop still keeps the
    // plate under the photo.
    splat::SceneMeta bare;
    bare.backend = "ply";
    auto fallback = splat::make_scene_label(bare, "photo01_living_room", 0, 1);
    REQUIRE(fallback.ok());
    CHECK(fallback.value().photo_bottom_y == doctest::Approx(0.8f));
    CHECK(fallback.value().top_y > fallback.value().photo_bottom_y);

    CHECK(!splat::make_scene_label(sharp_meta(), "", 0, 1).ok());
}

TEST_CASE("vr9 the label uses the smooth Inter font at about headset pixel scale") {
    const splat::SceneMeta meta = sharp_meta();
    const auto result = splat::make_scene_label(meta, "immichvr-1", 0, 5);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SceneLabel& label = result.value();
    // 0.035 m at 1.6 m with the PSVR2's ~770 px focal length is a 17 px em:
    // the mask is a small, smooth block instead of the old 8x8 font at 4x.
    CHECK(label.image.width > 100);
    CHECK(label.image.width < 600);
    CHECK(label.image.height > 20);
    CHECK(label.image.height < 80);
    CHECK(label.glyph_height == doctest::Approx(0.035f));

    // Anti-aliased coverage: the Inter raster has partial pixels, not the
    // on/off cells of the bitmap font.
    int partial = 0;
    int lit = 0;
    for (uint8_t value : label.image.pixels) {
        if (value != 0) ++lit;
        if (value > 0 && value < 255) ++partial;
    }
    CHECK(lit > 50);
    CHECK(partial > 20);

    // About one texel per headset pixel at the reading distance: the drawn
    // text's projected width is within ~30 % of its texel width.
    const float projected = 770.5f * label.text.width / label.distance;
    MESSAGE("label " << label.image.width << "x" << label.image.height << " texels, projected "
                     << projected << " px");
    CHECK(projected > 0.7f * static_cast<float>(label.image.width));
    CHECK(projected < 1.3f * static_cast<float>(label.image.width));
}
