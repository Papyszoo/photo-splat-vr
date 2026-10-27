#pragma once

#include <cmath>

#include "ui/canvas.h"

namespace splat {

// Colours and metrics for the panel: a dark, deep-blue look with light
// anti-aliased text, rounded tiles and a bright focus ring, sized to be read
// from a sofa and inside the headset. The design canvas is 1280x720; on the VR
// panel (1.4 m at 1.6 m) one canvas pixel is about 0.7 headset pixels, so body
// text is 22 px (12 px x-height) and secondary text (counts, model names,
// breadcrumbs, hints) is 20 px; nothing that must be read is smaller.
struct UiTheme {
    // The backdrop: a vertical gradient with a soft glow near the top.
    Rgba background_top{};
    Rgba background_bottom{};
    Rgba background{};  // flat fill where a single colour is needed

    // Surfaces: translucent white over the backdrop.
    Rgba panel{};     // tiles, list plates, cards
    Rgba selected{};  // the focused plate
    Rgba veil{};      // dims the screen behind a popup
    Rgba card{};      // popups, confirmations and the toast
    Rgba light{};     // light plates: a focused key, the active tab, button prompts

    // Text.
    Rgba text{};      // primary copy
    Rgba dim_text{};  // secondary lines, unfocused captions
    Rgba faint_text{};  // hints that should recede

    // States.
    Rgba accent{};   // progress, selection marks, active tab
    Rgba focus{};    // focus ring
    Rgba glow{};     // halo around the focus ring
    Rgba warning{};  // errors, destructive actions
    Rgba ok{};       // success, ready states

    // Metrics in pixels.
    int margin = 64;      // left/right inset of every screen
    int padding = 16;     // inside plates
    int radius = 14;      // tile and plate corners
    int row_height = 72;  // list row: label + sublabel
    int small_size = 20;  // captions, sublabels, hints, breadcrumbs
    int body_size = 22;   // labels
    int heading_size = 28;  // card headings
    int title_size = 40;  // the screen title

    // The pixel scale this theme is built at: 1 for the 1280x720 TV canvas,
    // 896/1280 = 0.7 for the VR panel's 896x504 canvas (platform/ps5/viewer/
    // ui_setup.cpp), so both show the same physical size. `scaled()` is for the
    // screens' own design-time constants (a key height, the chrome bands), so a
    // scaled theme scales the whole layout and not only the fields above.
    float scale = 1.0f;

    // Height of one line box at `size` px.
    static int text_height(int size) { return UiCanvas::line_height(size); }

    // A design pixel (a constant written for the 1280x720 canvas) at this
    // theme's scale.
    int scaled(int design_pixels) const {
        return static_cast<int>(std::lround(static_cast<float>(design_pixels) * scale));
    }
};

// The same design at another canvas scale: every metric and font size above
// multiplied by `factor` (whole pixels). The VR panel draws the 1280x720 design
// on 896x504, so it uses scaled_theme(default_theme(), 896.0f / 1280.0f) and its
// screens cover the same physical size in the headset as on the TV; the TV keeps
// default_theme() unscaled. Fonts rasterise at the requested size (ui/font.cpp),
// so the smaller sizes are real glyph sizes, not a downsampled bitmap. This is
// the same theme struct, scaled - not a second look.
inline UiTheme scaled_theme(const UiTheme& design, float factor) {
    UiTheme theme = design;
    if (!(factor > 0.0f) || !std::isfinite(factor)) return theme;
    const float total = design.scale * factor;
    auto scaled = [total](int value) {
        return static_cast<int>(std::lround(static_cast<float>(value) * total));
    };
    theme.scale = total;
    theme.margin = scaled(design.margin);
    theme.padding = scaled(design.padding);
    theme.radius = scaled(design.radius);
    theme.row_height = scaled(design.row_height);
    theme.small_size = scaled(design.small_size);
    theme.body_size = scaled(design.body_size);
    theme.heading_size = scaled(design.heading_size);
    theme.title_size = scaled(design.title_size);
    return theme;
}

inline UiTheme default_theme() {
    UiTheme theme;
    theme.background_top = Rgba{14, 30, 72, 255};
    theme.background_bottom = Rgba{3, 6, 18, 255};
    theme.background = Rgba{8, 14, 34, 255};
    theme.panel = Rgba{255, 255, 255, 20};
    theme.selected = Rgba{255, 255, 255, 44};
    theme.veil = Rgba{2, 4, 12, 228};
    theme.card = Rgba{30, 42, 76, 252};
    theme.light = Rgba{236, 240, 248, 255};
    theme.text = Rgba{246, 248, 252, 255};
    theme.dim_text = Rgba{168, 178, 200, 255};
    theme.faint_text = Rgba{118, 130, 156, 255};
    theme.accent = Rgba{64, 148, 255, 255};
    theme.focus = Rgba{255, 255, 255, 255};
    theme.glow = Rgba{110, 170, 255, 120};
    theme.warning = Rgba{255, 118, 96, 255};
    theme.ok = Rgba{96, 214, 150, 255};
    return theme;
}

}  // namespace splat
