#pragma once

#include <cstddef>
#include <string>

#include "common/status.h"
#include "render/overlay_quad.h"
#include "render/text_raster.h"
#include "scene/scene_folder.h"

namespace splat {

// The model name shown in the scene label. `meta.json`'s "backend" is the
// reconstructor's name ("sharp", "depth-anything-v2-small",
// "depth-anything-v2-large", ...). A backend that names a model wins; an
// empty/unknown one falls back to the folder suffix (-da2-large, -da2-small;
// no suffix = SHARP).
std::string scene_model_name(const std::string& backend, const std::string& folder_name);

// The two label lines: "<folder> \xC2\xB7 <model>" and "scene N/M".
struct SceneLabelText {
    std::string title;
    std::string counter;
};
SceneLabelText scene_label_text(const SceneMeta& meta, const std::string& folder_name,
                                size_t index, size_t count);

struct SceneLabelLayout {
    // Reading distance in front of the capture camera, metres.
    float distance = 1.6f;
    // Laid-out height of one text line, and the largest accepted line width. A
    // long title shrinks the text instead of leaving a 2 m wide plate. The line
    // height is what the 1:1 raster resolution below is derived from.
    float glyph_height = 0.035f;
    float max_width = 0.9f;
    // Plate border around the text block, and the gap between the photo's
    // bottom edge and the plate's top edge, metres.
    float padding = 0.02f;
    float gap = 0.04f;
    // VR9: the label is drawn with the smooth Inter font (core/ui/font.h) at
    // about one texel per headset pixel at `distance`. The reference is the
    // PSVR2's per-eye fy (~770.5 px at 1984x2048, docs/console-runbook.md); a
    // 0.035 m line at 1.6 m is then ~17 px, so the mask is 1:1 instead of the
    // old 8x8 font at 4x scale (2x oversampled and minified in the headset).
    float reference_focal_pixels = 770.5f;
    uint32_t min_font_pixels = 9;
    // The counter line is a little smaller than the title.
    float counter_scale = 0.8f;
    // Light text on a dark translucent plate (straight alpha, linear RGB).
    float plate_tint[4] = {0.03f, 0.03f, 0.035f, 0.65f};
    float text_tint[4] = {0.94f, 0.95f, 0.97f, 0.98f};
};

// A complete scene label: the rasterised R8 mask, the two world-space quads
// (plate first, then text at the same centre) and the placement it used. The
// caller fills in OverlayQuad::texture from its renderer.
struct SceneLabel {
    std::string title;
    std::string counter;
    TextImage image;
    OverlayQuad plate;
    OverlayQuad text;
    // World y (down positive) of the capture frustum's bottom edge at
    // `distance`, the plate's top edge, and the laid-out height of one text
    // line in metres.
    float photo_bottom_y = 0.0f;
    float top_y = 0.0f;
    float glyph_height = 0.0f;
    float distance = 0.0f;
};

// Builds the label for one scene. The plate sits below the bottom edge of the
// capture frustum at `distance` from the capture camera (the scene origin),
// centred on x = 0, facing the capture camera, so it stays world-locked under
// the photo. Fails when the text cannot be laid out (empty title).
Result<SceneLabel> make_scene_label(const SceneMeta& meta, const std::string& folder_name,
                                    size_t index, size_t count,
                                    const SceneLabelLayout& layout = SceneLabelLayout{});

}  // namespace splat
