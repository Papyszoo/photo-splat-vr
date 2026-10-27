#pragma once

#include <cstdint>

namespace splat {

// One world-space, textured quad for the overlay pass (scene label, later
// controller tooltips and menus). No Vulkan types here: the geometry can be
// built and unit tested on a CPU-only host.
//
// Scene axes are OpenCV (camera.h): +x right, +y down, +z forward. A quad that
// faces the capture camera therefore has right = +x and up = -y. The corners
// are center +- right * width / 2 +- up * height / 2; the texture's v = 0 row
// is the top edge (`up` direction).
struct OverlayQuad {
    float center[3] = {0.0f, 0.0f, 0.0f};
    float right[3] = {1.0f, 0.0f, 0.0f};  // world direction of +u, unit length
    float up[3] = {0.0f, -1.0f, 0.0f};    // world direction of +v (top), unit
    float width = 1.0f;                   // metres
    float height = 1.0f;                  // metres
    // Index returned by SplatRenderer::add_overlay_texture (R8 mask, tinted) or
    // add_overlay_rgba_texture (RGBA panel, its own colours).
    uint32_t texture = 0;
    // Straight-alpha tint: RGB in linear light, a = opacity. The fragment
    // shader outputs (rgb, a * mask) and the pipeline blends with
    // src_alpha / one_minus_src_alpha. An RGBA texture ignores the tint RGB;
    // only `a` fades the panel.
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

}  // namespace splat
