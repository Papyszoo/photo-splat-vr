#include "render/scene_label.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "ui/canvas.h"

namespace splat {
namespace {

// VR9: rasterise the two label lines with the panel's Inter font, at the pixel
// size the layout derives from the headset's own focal length. The result is
// still an R8 mask (the label quad tints it), so the overlay path is unchanged;
// only the glyphs are smooth and the resolution is 1:1 with the headset.
TextImage rasterise_label(const SceneLabelText& text, int title_px, int counter_px,
                          int* title_line_px) {
    const int title_h = UiCanvas::line_height(title_px);
    const int counter_h = UiCanvas::line_height(counter_px);
    const int line_gap = std::max(2, title_h / 4);
    const int pad_x = 2;
    const int pad_y = 1;
    const int title_width = UiCanvas::text_width(text.title, title_px);
    const int counter_width = UiCanvas::text_width(text.counter, counter_px);
    TextImage image;
    image.width = static_cast<uint32_t>(std::max(title_width, counter_width) + 2 * pad_x);
    image.height =
        static_cast<uint32_t>(2 * pad_y + title_h + line_gap + counter_h);
    if (image.width == 0 || image.height == 0) return image;
    UiCanvas canvas(image.width, image.height);
    const Rgba white{255, 255, 255, 255};
    canvas.text(pad_x, pad_y, text.title, title_px, white);
    canvas.text(pad_x, pad_y + title_h + line_gap, text.counter, counter_px, white);
    image.pixels.resize(static_cast<size_t>(image.width) * image.height);
    const std::vector<uint8_t>& rgba = canvas.pixels();
    for (size_t index = 0; index < image.pixels.size(); ++index) {
        image.pixels[index] = rgba[index * 4 + 3];
    }
    if (title_line_px != nullptr) *title_line_px = title_h;
    return image;
}

std::string lowercase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string model_from_folder_suffix(const std::string& folder_name) {
    const std::string name = lowercase(folder_name);
    if (ends_with(name, "-da2-large")) return "DA2-Large";
    if (ends_with(name, "-da2-small")) return "DA2-Small";
    // Depth-only models compared next to SHARP (runs/compare_models): their
    // meta.json names are not a backend this label knows, so the folder suffix
    // carries the model, like the DA2 scenes.
    if (ends_with(name, "-moge2")) return "MoGe-2 L";
    if (ends_with(name, "-da3-metric-large")) return "DA3 Metric-L";
    if (ends_with(name, "-da3-small")) return "DA3 Small";
    // A hand-picked scene folder without a backend suffix is the product
    // backend (docs/PLAN.md: SHARP only).
    return "SHARP";
}

}  // namespace

std::string scene_model_name(const std::string& backend, const std::string& folder_name) {
    const std::string name = lowercase(backend);
    if (name.find("sharp") != std::string::npos) return "SHARP";
    if (name.find("depth-anything") != std::string::npos || name.find("da2") != std::string::npos) {
        // Spelled as the Models screen names them ("DA2-Small"); the pinned
        // "da2" backend is DA2-Small.
        if (name.find("large") != std::string::npos) return "DA2-Large";
        if (name.find("small") != std::string::npos || name == "da2") return "DA2-Small";
        return "DA2";
    }
    // No meta.json, an empty backend, "ply", "fallback" or a backend this label
    // does not know: the folder name is the remaining source of truth.
    return model_from_folder_suffix(folder_name);
}

SceneLabelText scene_label_text(const SceneMeta& meta, const std::string& folder_name,
                                size_t index, size_t count) {
    SceneLabelText text;
    text.title = folder_name + " \xC2\xB7 " + scene_model_name(meta.backend, folder_name);
    if (count == 0) {
        text.counter = "scene -/-";
    } else {
        text.counter = "scene " + std::to_string(index + 1) + "/" + std::to_string(count);
    }
    return text;
}

Result<SceneLabel> make_scene_label(const SceneMeta& meta, const std::string& folder_name,
                                    size_t index, size_t count,
                                    const SceneLabelLayout& layout) {
    if (folder_name.empty()) {
        return Result<SceneLabel>::failure("the scene folder has no name");
    }
    SceneLabel label;
    const SceneLabelText text = scene_label_text(meta, folder_name, index, count);
    label.title = text.title;
    label.counter = text.counter;
    // One texel per headset pixel at the label's distance (see the layout).
    const float distance = layout.distance > 0.0f ? layout.distance : 1.6f;
    const float texels_per_metre = layout.reference_focal_pixels / distance;
    const int line_px = std::max<int>(static_cast<int>(layout.min_font_pixels),
                                      static_cast<int>(std::lround(layout.glyph_height *
                                                                   texels_per_metre)));
    const int counter_px = std::max<int>(static_cast<int>(layout.min_font_pixels),
                                         static_cast<int>(std::lround(
                                             static_cast<float>(line_px) * layout.counter_scale)));
    int title_line_px = line_px;
    label.image = rasterise_label(text, line_px, counter_px, &title_line_px);
    if (label.image.empty()) {
        return Result<SceneLabel>::failure("cannot rasterise the scene label text");
    }

    // One texel in metres, from the title's line box (glyph_height), then
    // shrunk if the longest line would exceed max_width.
    float texel = layout.glyph_height / static_cast<float>(std::max(title_line_px, 1));
    if (layout.max_width > 0.0f && label.image.width * texel > layout.max_width) {
        texel = layout.max_width / static_cast<float>(label.image.width);
    }
    const float text_width = static_cast<float>(label.image.width) * texel;
    const float text_height = static_cast<float>(label.image.height) * texel;
    const float plate_width = text_width + 2.0f * layout.padding;
    const float plate_height = text_height + 2.0f * layout.padding;

    // The capture frustum's bottom edge at the reading distance: the photo's
    // last row is at image y = height, so its direction is
    // (height - cy) / fy. No intrinsics (a .ply scene) falls back to a 0.5 m
    // drop at 1.6 m, below any capture image's centre.
    float bottom = 0.5f * distance;
    if (meta.intrinsics.fx > 0.0f && meta.intrinsics.fy > 0.0f && meta.intrinsics.height > 0) {
        const float candidate =
            distance * (static_cast<float>(meta.intrinsics.height) - meta.intrinsics.cy) /
            meta.intrinsics.fy;
        if (std::isfinite(candidate)) bottom = candidate;
    }
    const float top = bottom + layout.gap;
    const float center_y = top + 0.5f * plate_height;

    label.photo_bottom_y = bottom;
    label.top_y = top;
    label.glyph_height = static_cast<float>(title_line_px) * texel;
    label.distance = distance;

    // The plate and the text share one centre, world-locked under the photo
    // and facing the capture camera (scene +y is down, so up is -y). The
    // caller draws the plate first, then the text over it.
    label.plate.center[0] = 0.0f;
    label.plate.center[1] = center_y;
    label.plate.center[2] = distance;
    label.plate.width = plate_width;
    label.plate.height = plate_height;
    std::copy(layout.plate_tint, layout.plate_tint + 4, label.plate.tint);

    label.text.center[0] = 0.0f;
    label.text.center[1] = center_y;
    label.text.center[2] = distance;
    label.text.width = text_width;
    label.text.height = text_height;
    std::copy(layout.text_tint, layout.text_tint + 4, label.text.tint);
    return Result<SceneLabel>::success(std::move(label));
}

}  // namespace splat
