#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "ui/canvas.h"

namespace splat {
namespace ui_preview {

// A named screen painter. Tools register one per preview: the built-in test
// pattern, and later one per panel screen, so judging a screen needs no console.
using Pattern = std::function<void(UiCanvas&)>;

// How one preview is drawn: the canvas the panel runs at, and the theme scale
// the screen is designed for. The TV panel is 1280x720 at scale 1; a preview
// whose name ends in "-vr" draws the same screen as the headset sees it,
// 896x504 with the 0.7 theme (platform/ps5/viewer/ui_setup.cpp), which is how
// the VR layout's clipping is judged without a console.
struct PreviewSpec {
    uint32_t width = 1280;
    uint32_t height = 720;
    float theme_scale = 1.0f;
    Pattern draw;
};

using Registry = std::map<std::string, PreviewSpec>;

// The registry lives in patterns.cpp. A new screen is one line there, e.g.
//   {"albums", {1280, 720, 1.0f, draw_albums_screen}},
const Registry& registry();

}  // namespace ui_preview
}  // namespace splat
