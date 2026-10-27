// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/overlay_hud.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace splat {
namespace {

// The camera axes the renderer builds from a FrameView's yaw/pitch/roll
// (core/render/splat_renderer.cpp): yaw about scene y, pitch about the yawed x,
// then roll about the optical axis. Kept here so the HUD is placed with exactly
// the view the splats use.
void camera_axes(const CameraPose& pose, float right[3], float down[3], float forward[3]) {
    const float cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
    const float cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const float cr = std::cos(pose.roll), sr = std::sin(pose.roll);
    forward[0] = sy * cp;
    forward[1] = sp;
    forward[2] = cy * cp;
    const float s0[3] = {cy, 0.0f, -sy};
    const float d0[3] = {forward[1] * s0[2] - forward[2] * s0[1],
                         forward[2] * s0[0] - forward[0] * s0[2],
                         forward[0] * s0[1] - forward[1] * s0[0]};
    for (int axis = 0; axis < 3; ++axis) {
        right[axis] = cr * s0[axis] + sr * d0[axis];
        down[axis] = cr * d0[axis] - sr * s0[axis];
    }
}

// Places a quad on the HUD plane: `distance` ahead, `vertical` along `up` and
// `lateral` along `right`, both metres from the plane's centre.
OverlayQuad on_plane(const CameraPose& pose, const float right[3], const float up[3],
                     const float forward[3], float distance, float vertical, float lateral) {
    OverlayQuad quad;
    for (int axis = 0; axis < 3; ++axis) {
        quad.center[axis] = pose.position[axis] + forward[axis] * distance +
                            up[axis] * vertical + right[axis] * lateral;
        quad.right[axis] = right[axis];
        quad.up[axis] = up[axis];
    }
    return quad;
}

void tint(float target[4], const float source[4]) {
    std::memcpy(target, source, sizeof(float) * 4);
}

}  // namespace

uint32_t hud_text_pixels(size_t characters, uint32_t pixel_scale) {
    return static_cast<uint32_t>(characters * 8u * std::max(pixel_scale, 1u));
}

uint32_t hud_text_line_pixels(uint32_t pixel_scale) {
    return 8u * std::max(pixel_scale, 1u);
}

float hud_text_width(size_t characters, const HudLayout& layout) {
    return static_cast<float>(hud_text_pixels(characters, layout.pixel_scale)) *
           layout.glyph_height / static_cast<float>(hud_text_line_pixels(layout.pixel_scale));
}

float hud_text_height(const HudLayout& layout) { return layout.glyph_height; }

std::vector<OverlayQuad> hud_quads(const CameraPose& camera, const HudState& state,
                                   const HudLayout& layout, const HudTextures& textures) {
    std::vector<OverlayQuad> quads;
    if (textures.solid == UINT32_MAX) return quads;

    float right[3], down[3], forward[3];
    camera_axes(camera, right, down, forward);
    const float up[3] = {-down[0], -down[1], -down[2]};

    // ---- the recentre hold: text over a thin bar, below the view centre ----
    if (state.hold_progress >= 0.0f && textures.hold_text != UINT32_MAX) {
        const float progress = std::clamp(state.hold_progress, 0.0f, 1.0f);
        const float text_width = hud_text_width(kHoldHudChars, layout);
        const float text_height = hud_text_height(layout);
        const float plate_width =
            std::max(text_width, layout.bar_width) + 2.0f * layout.padding;
        const float plate_height =
            text_height + layout.bar_gap + layout.bar_height + 2.0f * layout.padding;
        const float bar_center_y = -0.5f * (text_height + layout.bar_gap);
        const float text_center_y = 0.5f * (layout.bar_height + layout.bar_gap);

        OverlayQuad plate = on_plane(camera, right, up, forward, layout.distance,
                                     -layout.hold_offset, 0.0f);
        plate.width = plate_width;
        plate.height = plate_height;
        plate.texture = textures.solid;
        tint(plate.tint, layout.plate_tint);
        quads.push_back(plate);

        OverlayQuad text = on_plane(camera, right, up, forward, layout.distance,
                                    -layout.hold_offset + text_center_y, 0.0f);
        text.width = text_width;
        text.height = text_height;
        text.texture = textures.hold_text;
        tint(text.tint, layout.text_tint);
        quads.push_back(text);

        OverlayQuad track = on_plane(camera, right, up, forward, layout.distance,
                                     -layout.hold_offset + bar_center_y, 0.0f);
        track.width = layout.bar_width;
        track.height = layout.bar_height;
        track.texture = textures.solid;
        tint(track.tint, layout.bar_track_tint);
        quads.push_back(track);

        // The fill grows from the bar's left end: its centre moves right by half
        // the part that is not filled.
        OverlayQuad fill = on_plane(camera, right, up, forward, layout.distance,
                                    -layout.hold_offset + bar_center_y,
                                    -0.5f * layout.bar_width * (1.0f - progress));
        fill.width = layout.bar_width * progress;
        fill.height = layout.bar_height;
        fill.texture = textures.solid;
        tint(fill.tint, layout.bar_fill_tint);
        quads.push_back(fill);
    }

    // ---- the scale readout: above the view centre ----
    if (state.show_scale && textures.scale_text != UINT32_MAX) {
        const float text_width = hud_text_width(kScaleHudChars, layout);
        const float text_height = hud_text_height(layout);

        OverlayQuad plate = on_plane(camera, right, up, forward, layout.distance,
                                     layout.scale_offset, 0.0f);
        plate.width = text_width + 2.0f * layout.padding;
        plate.height = text_height + 2.0f * layout.padding;
        plate.texture = textures.solid;
        tint(plate.tint, layout.plate_tint);
        quads.push_back(plate);

        OverlayQuad text = on_plane(camera, right, up, forward, layout.distance,
                                    layout.scale_offset, 0.0f);
        text.width = text_width;
        text.height = text_height;
        text.texture = textures.scale_text;
        tint(text.tint, layout.text_tint);
        quads.push_back(text);
    }

    return quads;
}

}  // namespace splat
