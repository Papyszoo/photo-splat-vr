// The VR HUD's geometry (core/render/overlay_hud.h), checked on the CPU: it is
// placed head-locked in front of the eye, the hold bar's fill tracks the
// progress, and each element's quads use the textures they should. The console
// run is what shows it on the headset.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "render/overlay_hud.h"

namespace {

double dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

splat::HudTextures textures() {
    splat::HudTextures out;
    out.hold_text = 3;
    out.scale_text = 4;
    out.solid = 5;
    return out;
}

}  // namespace

TEST_CASE("the HUD is head-locked in front of the eye") {
    splat::CameraPose camera;
    camera.position[0] = 0.3f;
    camera.position[1] = 1.6f;
    camera.position[2] = 0.2f;
    camera.yaw = 0.35f;
    camera.pitch = 0.12f;
    camera.roll = -0.05f;
    const splat::HudLayout layout;
    splat::HudState state;
    state.hold_progress = 0.5f;
    state.show_scale = true;

    const std::vector<splat::OverlayQuad> quads =
        splat::hud_quads(camera, state, layout, textures());
    // Plate, hold text, bar track, bar fill, scale plate, scale text.
    REQUIRE(quads.size() == 6);
    for (const splat::OverlayQuad& quad : quads) {
        const float to_quad[3] = {quad.center[0] - camera.position[0],
                                  quad.center[1] - camera.position[1],
                                  quad.center[2] - camera.position[2]};
        const double distance = std::sqrt(dot3(to_quad, to_quad));
        MESSAGE("quad centre at " << distance << " m, " << quad.width << " x " << quad.height);
        // The plane is 1.5 m ahead and the elements are a few centimetres from
        // its centre, so every quad is 1.4-1.6 m from the eye whatever the head
        // pose.
        CHECK(distance > 1.4);
        CHECK(distance < 1.6);
        // Textured or solid as intended, and the sizes are sane.
        CHECK(quad.width > 0.0f);
        CHECK(quad.height > 0.0f);
    }
    // The bar's fill is half the track's width at 50 %.
    const splat::OverlayQuad& track = quads[2];
    const splat::OverlayQuad& fill = quads[3];
    CHECK(track.texture == textures().solid);
    CHECK(fill.texture == textures().solid);
    CHECK(fill.width == doctest::Approx(track.width * 0.5f));
    CHECK(fill.height == doctest::Approx(track.height));
    // The text quads carry the two text masks.
    CHECK(quads[1].texture == textures().hold_text);
    CHECK(quads[4].texture == textures().solid);   // the scale plate
    CHECK(quads[5].texture == textures().scale_text);
}

TEST_CASE("the HUD hides when there is nothing to show") {
    const splat::CameraPose camera;
    const splat::HudLayout layout;
    splat::HudState state;
    // Nothing held, no scale readout.
    CHECK(splat::hud_quads(camera, state, layout, textures()).empty());
    // A hold in progress alone is four quads (plate, text, track, fill).
    state.hold_progress = 0.0f;
    CHECK(splat::hud_quads(camera, state, layout, textures()).size() == 4);
    // Without the solid texture nothing can be drawn: the plates and the bar
    // would be invisible anyway.
    splat::HudTextures none;
    none.hold_text = 3;
    none.scale_text = 4;
    CHECK(splat::hud_quads(camera, state, layout, none).empty());
    // Without the text masks the elements are skipped rather than drawn blank.
    state.show_scale = true;
    splat::HudTextures solid_only;
    solid_only.solid = 5;
    CHECK(splat::hud_quads(camera, state, layout, solid_only).empty());
}

TEST_CASE("HUD text masks keep one size for the whole scale range") {
    const splat::HudLayout layout;
    // The drawn characters must match the geometry's assumption: every scale
    // readout is kScaleHudChars and the hold line is kHoldHudChars.
    const std::string scales[] = {"Scale 0.3x - front ", "Scale 1.0x - front ",
                                  "Scale 1.6x - front ", "Scale 8.0x - centre"};
    for (const std::string& text : scales) {
        CHECK(text.size() == splat::kScaleHudChars);
        CHECK(splat::hud_text_pixels(splat::kScaleHudChars, layout.pixel_scale) ==
              static_cast<uint32_t>(text.size()) * 8u * layout.pixel_scale);
    }
    CHECK(std::string(splat::kHoldHudText).size() == splat::kHoldHudChars);
    CHECK(splat::hud_text_width(splat::kScaleHudChars, layout) > 0.0f);
    CHECK(splat::hud_text_height(layout) == doctest::Approx(layout.glyph_height));
    // The width scales with the character count.
    CHECK(splat::hud_text_width(splat::kHoldHudChars, layout) ==
          doctest::Approx(splat::hud_text_width(splat::kScaleHudChars, layout) *
                          (static_cast<float>(splat::kHoldHudChars) /
                           static_cast<float>(splat::kScaleHudChars))));
}
