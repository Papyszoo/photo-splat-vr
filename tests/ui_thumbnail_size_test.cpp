// Stage T: one thumbnail size setting for every entry grid.
//
// The numbers here are the stage's contract, measured on the panel canvases the
// app really draws (the TV's 1280x720 design canvas, upscaled to the 1920x1080
// output, and the VR panel's 896x504 at the 0.7 theme):
//
//   - the default shows at least 12 entries on every entry screen in both
//     modes (the VR folder grid used to show two: the cell constants were never
//     scaled, so 264 px tiles gave 2 columns by 1 row on 896x504);
//   - the largest shows at least 6, and no step and no supported resolution can
//     fall back to the 2-entry case;
//   - the size saves in ui.json, an old file loads the default and an
//     out-of-range value is clamped;
//   - the focused entry stays focused and in view after a change, and the
//     scroll follows the new rows;
//   - Left/Right and a pointer press/drag move the Settings slider;
//   - Albums, Scenes and Photos all lay out from the same function;
//   - the folder grid and the photo picker draw the same columns and the same
//     18 px gap at every step (stage TF2), and a photo tile fits the whole
//     picture instead of cropping it;
//   - a size change does not decode a thumbnail again.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "jobs/image_io.h"
#include "scene/scene_folder.h"
#include "test_util.h"
#include "ui/app.h"
#include "ui/fakes.h"
#include "ui/screens/albums.h"
#include "ui/screens/photos.h"
#include "ui/screens/scenes.h"
#include "ui/screens/settings.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"
#include "ui/theme.h"
#include "ui/ui_state.h"
#include "ui/widgets.h"

using splat::Path;

namespace {

// A tiny real JPEG, so the photo picker's worker decodes a real file.
void write_jpeg(const Path& path, uint8_t red) {
    splat::Image image;
    image.resize(32, 24);
    for (uint32_t y = 0; y < 24; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(red + x);
            pixel[1] = static_cast<uint8_t>(y * 8);
            pixel[2] = 96;
        }
    }
    const auto bytes = splat::encode_jpeg(image, 85);
    REQUIRE_MESSAGE(bytes.ok(), bytes.message());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.value().data()),
              static_cast<std::streamsize>(bytes.value().size()));
    REQUIRE(static_cast<bool>(out));
}

// A gallery scene folder the two listing functions accept.
void write_scene(const Path& folder) {
    std::filesystem::create_directories(folder);
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.seconds_taken = 1.0;
    const splat::SplatScene scene = splat_test::make_test_scene(8);
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::Status status = splat::write_scene_folder(folder, scene, meta, jpeg, jpeg);
    REQUIRE_MESSAGE(status.ok(), status.message());
}

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

splat::UiInput pointer(float px, float py, bool down, bool click) {
    splat::UiInput input;
    input.pointer_valid = true;
    input.px = px;
    input.py = py;
    input.pointer_down = down;
    input.click = click;
    return input;
}

// The panel a mode really runs: the canvas, the theme built for it and the
// content rectangle UiApp hands the screen (app.cpp's panel_content_rect). The
// photo picker starts its grid below the tab header (PhotosScreen::grid_area).
// Both come from the app, so a chrome or grid-top change moves the tests with
// the real area instead of leaving them on a stale copy.
struct PanelMode {
    std::string name;
    splat::UiTheme theme;
    splat::Rect content;
    splat::Rect photos;
    uint32_t width = 1280;
    uint32_t height = 720;
};

PanelMode tv_mode() {
    PanelMode mode;
    mode.name = "TV 1920x1080 output (1280x720 canvas)";
    mode.theme = splat::default_theme();
    mode.content = splat::panel_content_rect(mode.theme, mode.width, mode.height);
    mode.photos = splat::PhotosScreen::grid_area(mode.content, mode.theme);
    return mode;
}

PanelMode vr_mode() {
    PanelMode mode;
    mode.name = "VR panel 896x504";
    mode.width = 896;
    mode.height = 504;
    mode.theme = splat::scaled_theme(splat::default_theme(), 896.0f / 1280.0f);
    mode.content = splat::panel_content_rect(mode.theme, mode.width, mode.height);
    mode.photos = splat::PhotosScreen::grid_area(mode.content, mode.theme);
    return mode;
}

// The picker area of a hypothetical 1920x1080 canvas at the design theme: not a
// shipped canvas, but the resolution the brief names for the TV, and a second
// proof that no larger panel gets fewer entries.
splat::Rect huge_photos_area() {
    const splat::UiTheme theme = splat::default_theme();
    return splat::PhotosScreen::grid_area(splat::panel_content_rect(theme, 1920, 1080), theme);
}

splat::Rect huge_content_area() {
    return splat::panel_content_rect(splat::default_theme(), 1920, 1080);
}

bool cell_in_view(const splat::GridView& grid, int index, const splat::Rect& area) {
    const splat::Rect cell = grid.cell_rect(index, area);
    return cell.y >= area.y && cell.bottom() <= area.bottom() && cell.x >= area.x &&
           cell.right() <= area.right();
}

// The glyphs a fitted caption really shows: a UTF-8 continuation byte is not a
// glyph, so a byte count would count the three-byte ellipsis as three glyphs.
int glyph_count(const std::string& text) {
    int count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("ui_thumbs every entry screen shows the stage T counts in both modes") {
    // Stage TF2: a step is a column count, so the folder grids and the photo
    // picker draw the same columns and the same gap at every size, and their
    // entry counts are now identical too (they used to differ at the largest
    // step). A folder tile reserves a sublabel line (its scene count), a photo
    // tile not, so the photo grid's cells are shorter; the count guard reserves
    // the taller block either way, so a step's column count never depends on the
    // label. The default (>= 12) and the largest (>= 6) limits hold on both
    // screens and in both modes.
    const splat::ThumbnailLabel folder_label{2, true};
    const splat::ThumbnailLabel photo_label{2, false};
    for (const PanelMode& mode : {tv_mode(), vr_mode()}) {
        CAPTURE(mode.name);
        const splat::UiTheme& theme = mode.theme;
        const splat::Rect& albums = mode.content;
        const splat::Rect& photos = mode.photos;

        // The default: at least 12 everywhere. The folder grid on the VR panel
        // used to show 2, the photo picker 4.
        CHECK(splat::thumbnail_entries_visible(albums, theme, splat::kThumbnailSizeDefault,
                                               folder_label) >= 12);
        CHECK(splat::thumbnail_entries_visible(photos, theme, splat::kThumbnailSizeDefault,
                                               photo_label) >= 12);
        CHECK(splat::thumbnail_entries_visible(albums, theme, splat::kThumbnailSizeDefault,
                                               folder_label) == 14);
        CHECK(splat::thumbnail_entries_visible(photos, theme, splat::kThumbnailSizeDefault,
                                               photo_label) == 14);

        // The largest: at least 6. Both grids show five columns there now.
        CHECK(splat::thumbnail_entries_visible(albums, theme, 4, folder_label) >= 6);
        CHECK(splat::thumbnail_entries_visible(photos, theme, 4, photo_label) >= 6);
        CHECK(splat::thumbnail_entries_visible(albums, theme, 4, folder_label) == 10);
        CHECK(splat::thumbnail_entries_visible(photos, theme, 4, photo_label) == 10);

        // Every step, both screens: the stage's count table. The two screens
        // share the counts because they now share the layout; the VR panel shows
        // the same numbers at the design's physical size.
        const int counts[splat::kThumbnailSizeCount] = {27, 24, 14, 12, 10};
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            CHECK(splat::thumbnail_entries_visible(albums, theme, size, folder_label) ==
                  counts[size]);
            CHECK(splat::thumbnail_entries_visible(photos, theme, size, photo_label) ==
                  counts[size]);
        }

        // The columns the Settings slider reports, and the smallest step: 9, 8,
        // 7, 6 and 5 on both screens.
        CHECK(splat::thumbnail_columns(albums, theme, 2, folder_label) == 7);
        CHECK(splat::thumbnail_columns(photos, theme, 2, photo_label) == 7);
        CHECK(splat::thumbnail_columns(albums, theme, 4, folder_label) == 5);
        CHECK(splat::thumbnail_columns(photos, theme, 4, photo_label) == 5);
        CHECK(splat::thumbnail_columns(albums, theme, 0, folder_label) == 9);
        CHECK(splat::thumbnail_entries_visible(albums, theme, 0, folder_label) ==
              counts[0]);
        CHECK(splat::thumbnail_entries_visible(photos, theme, 0, photo_label) ==
              counts[0]);

        // The two-entry case cannot come back at any step.
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            CHECK(splat::thumbnail_entries_visible(albums, theme, size, folder_label) >= 6);
            CHECK(splat::thumbnail_entries_visible(photos, theme, size, photo_label) >= 6);
        }
    }
}

TEST_CASE("ui_thumbs a 1080p canvas keeps the default and largest limits") {
    const splat::UiTheme theme = splat::default_theme();
    const splat::ThumbnailLabel folder_label{2, true};
    const splat::ThumbnailLabel photo_label{2, false};
    CHECK(splat::thumbnail_entries_visible(huge_content_area(), theme, 2, folder_label) >= 12);
    CHECK(splat::thumbnail_entries_visible(huge_photos_area(), theme, 2, photo_label) >= 12);
    CHECK(splat::thumbnail_entries_visible(huge_content_area(), theme, 4, folder_label) >= 6);
    CHECK(splat::thumbnail_entries_visible(huge_photos_area(), theme, 4, photo_label) >= 6);
    // Bigger than the design canvas is more entries, never fewer. Stage TF2's
    // column counts put 21 photo entries on a 1080p default and 10 at the
    // largest step, both well above the 12 / 6 guarantees.
    CHECK(splat::thumbnail_entries_visible(huge_photos_area(), theme, 2, photo_label) == 21);
    CHECK(splat::thumbnail_entries_visible(huge_photos_area(), theme, 4, photo_label) == 10);
}

TEST_CASE("ui_thumbs the smallest tile stays readable") {
    // A caption line must survive at every mode and step: the tile is at least
    // as wide as a handful of glyphs and the picture keeps a positive height.
    // This is the floor the layout guard may not cross.
    for (const PanelMode& mode : {tv_mode(), vr_mode()}) {
        CAPTURE(mode.name);
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            const splat::ThumbnailLabel label{2, false};
            const splat::ThumbnailMetrics metrics =
                splat::thumbnail_metrics(mode.photos, mode.theme, size, label);
            CHECK(metrics.cell_width >= mode.theme.scaled(110));
            const int label_height = splat::thumbnail_label_height(mode.theme, 2, false);
            CHECK(metrics.cell_height - label_height >
                  2 * splat::UiCanvas::line_height(mode.theme.body_size - 2));
            CHECK(metrics.cell_height > label_height);
            // A name still keeps several glyphs before the ellipsis (counted as
            // glyphs: the ellipsis is one, not three bytes).
            const std::string fitted = splat::fit_text(
                "photograph-of-the-garden", mode.theme.body_size - 2, metrics.cell_width - 4);
            CHECK(glyph_count(fitted) >= 6);
        }
    }
}

TEST_CASE("ui_thumbs a panel smaller than the VR one shrinks tiles, never the count") {
    // thumbnail_metrics' guard: when an area is too small for a step's
    // guaranteed entry count, the tiles shrink (down to the legible floor)
    // rather than the grid showing fewer entries. This 400x150 panel at a half
    // theme is smaller than any shipped canvas, so it must exercise the loop.
    const splat::UiTheme theme = splat::scaled_theme(splat::default_theme(), 0.5f);
    const splat::Rect small{0, 0, 400, 150};
    const splat::ThumbnailLabel label{2, true};
    const splat::ThumbnailMetrics base = splat::thumbnail_metrics(small, theme, 4, label);
    CHECK(base.cell_width < theme.scaled(176));
    CHECK(base.cell_width >= theme.scaled(110));
    // The picture still has its one shape after a shrink.
    const int picture = base.cell_height - splat::thumbnail_label_height(theme, 2, true);
    CHECK(picture == static_cast<int>(std::lround(static_cast<double>(base.cell_width) * 102.0 / 176.0)));
    // The guard actually reaches the counts it promised on this panel.
    CHECK(splat::thumbnail_entries_visible(small, theme, splat::kThumbnailSizeDefault, label) >=
          12);
    CHECK(splat::thumbnail_entries_visible(small, theme, 4, label) >= 6);
    // It never goes below the floor, even when the count cannot be met.
    const splat::Rect tiny{0, 0, 120, 60};
    const splat::ThumbnailMetrics floor = splat::thumbnail_metrics(tiny, theme, 4, label);
    CHECK(floor.cell_width >= theme.scaled(110));
    CHECK(floor.cell_height > splat::thumbnail_label_height(theme, 2, true));
}

TEST_CASE("ui_thumbs the size saves, reloads, clamps and defaults on an old file") {
    splat_test::TempDir temp("ui-thumbs-state");
    const Path state_dir = temp.path();

    // No file and an older file without the key: the default.
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == splat::kThumbnailSizeDefault);
    {
        std::ofstream out(splat::ui_state_path(state_dir), std::ios::binary | std::ios::trunc);
        out << "{\"selected_model\":\"sharp\",\"tooltips\":true}\n";
    }
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == splat::kThumbnailSizeDefault);

    // Save and reload the largest.
    splat::UiState state;
    state.thumbnail_size = 4;
    REQUIRE_MESSAGE(splat::save_ui_state(state_dir, state).ok(), "save");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == 4);
    {
        std::ifstream in(splat::ui_state_path(state_dir));
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("\"thumbnail_size\": 4") != std::string::npos);
    }

    // A hand-edited value outside the range is clamped, not rejected.
    auto write_state = [&state_dir](const std::string& text) {
        std::ofstream out(splat::ui_state_path(state_dir), std::ios::binary | std::ios::trunc);
        out << text;
    };
    write_state("{\"thumbnail_size\": 9}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == 4);
    write_state("{\"thumbnail_size\": -3}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == 0);
    write_state("{\"thumbnail_size\": 2.6}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == 3);
    // A huge number must not go through an int cast (lround overflows): the
    // clamp happens on the number itself, so it is the largest step.
    write_state("{\"thumbnail_size\": 1e308}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == 4);
    write_state("{\"thumbnail_size\": -1e308}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == 0);
    write_state("{\"thumbnail_size\": \"large\"}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == splat::kThumbnailSizeDefault);
    write_state("{\"thumbnail_size\": true}");
    CHECK(splat::load_ui_state(state_dir).thumbnail_size == splat::kThumbnailSizeDefault);
}

TEST_CASE("ui_thumbs the Settings slider moves with the pad and a pointer drag") {
    splat_test::TempDir temp("ui-thumbs-settings");
    splat::UiState state;
    splat::UiServices services;
    services.state = &state;
    services.paths.state = temp.path();
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{64, 112, 1152, 532};
    splat::SettingsScreen settings;
    settings.on_enter(context, splat::ScreenArgs{});

    // The row exists, in the Display section.
    CHECK(settings.list().rows()[splat::SettingsScreen::ThumbnailSize].label ==
          "Thumbnail size");
    CHECK(settings.list().rows()[splat::SettingsScreen::ThumbnailSize].header == false);

    // From ScaleAround, five Downs reach it.
    for (int i = 0; i < 5; ++i) settings.update(context, press(splat::Down));
    REQUIRE(settings.list().focus() == splat::SettingsScreen::ThumbnailSize);
    CHECK(state.thumbnail_size == splat::kThumbnailSizeDefault);
    CHECK(settings.thumbnail_text(context) == "Default \xC2\xB7 7 per row");

    // Right one step, then to the end and no further; Left goes back.
    settings.update(context, press(splat::Right));
    CHECK(state.thumbnail_size == 3);
    CHECK(settings.thumbnail_text(context) == "Large \xC2\xB7 6 per row");
    for (int i = 0; i < 4; ++i) settings.update(context, press(splat::Right));
    CHECK(state.thumbnail_size == 4);
    CHECK(settings.thumbnail_text(context) == "Largest \xC2\xB7 5 per row");
    settings.update(context, press(splat::Left));
    CHECK(state.thumbnail_size == 3);
    // Every change saved the file at once.
    CHECK(splat::load_ui_state(temp.path()).thumbnail_size == 3);

    // A pointer press on the track's left end selects the smallest step; a drag
    // to the right end (button still down, no new click edge) selects the
    // largest. The same row rect draws it and receives the hit.
    const splat::Rect row = settings.list().row_rect(
        splat::SettingsScreen::ThumbnailSize, context.content, theme);
    REQUIRE_FALSE(row.empty());
    const splat::Rect area = settings.slider_area(row, theme);
    const int text_width = settings.thumbnail_text_width(context);
    // The track is laid out from the widest label, so it does not move when the
    // step changes mid-drag.
    const splat::Rect track = splat::Slider::track_rect(area, text_width, theme);
    const float cy = static_cast<float>(row.y) + static_cast<float>(row.h) / 2.0f;
    settings.update(context, pointer(static_cast<float>(track.x), cy, true, true));
    CHECK(state.thumbnail_size == 0);
    CHECK(settings.thumbnail_text(context) == "Smallest \xC2\xB7 9 per row");
    CHECK(settings.list().focus() == splat::SettingsScreen::ThumbnailSize);
    settings.update(context,
                    pointer(static_cast<float>(track.right() - 1), cy, true, false));
    CHECK(state.thumbnail_size == 4);
    CHECK(splat::load_ui_state(temp.path()).thumbnail_size == 4);

    // The value text right of the track is a label, not a button: a press on it
    // keeps the step instead of jumping to the largest one, and the same is true
    // of a press in the gap between the track and the text. The widget's hit box
    // is exactly the drawn track (widened by the knob), not the whole row.
    settings.update(context, press(splat::Left));
    REQUIRE(state.thumbnail_size == 3);
    const splat::Rect hit = splat::Slider::hit_rect(area, text_width, theme);
    REQUIRE_FALSE(hit.empty());
    CHECK(hit.right() < area.right());
    CHECK(hit.w <= track.w + 2 * std::max(4, theme.scaled(9)));
    for (const float px : {static_cast<float>(hit.right() + 1),
                           static_cast<float>(area.right() - 1)}) {
        CAPTURE(px);
        settings.update(context, pointer(px, cy, true, true));
        CHECK(state.thumbnail_size == 3);
        CHECK(settings.list().focus() == splat::SettingsScreen::ThumbnailSize);
        CHECK(settings.thumbnail_slider().value() == 3);
    }
    // A drag that overshoots the track's right end still clamps to the largest
    // step: the hit box carries the knob's radius past it.
    settings.update(context,
                    pointer(static_cast<float>(hit.right() - 1), cy, true, false));
    CHECK(state.thumbnail_size == 4);

    // The widget itself clamps and maps a click to a step.
    splat::Slider slider;
    CHECK(slider.value() == splat::kThumbnailSizeDefault);
    CHECK_FALSE(slider.set_value(2));
    CHECK(slider.set_value(99));
    CHECK(slider.value() == splat::kThumbnailSizeCount - 1);
    CHECK(slider.set_value(-1));
    CHECK(slider.value() == 0);
}

TEST_CASE("ui_thumbs the slider's per-row count matches the grids it describes") {
    // The Settings text says "about how many fit per row"; it must never name a
    // count that no thumbnail screen draws. The folder grids use the panel's
    // full content area and the photo picker starts below its tab strip; since
    // stage TF2 both draw the same column count at every step, so the text
    // names one number everywhere and this test builds it from both grids.
    splat_test::TempDir temp("ui-thumbs-count-text");
    splat::UiState state;
    splat::UiServices services;
    services.state = &state;
    services.paths.state = temp.path();
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{64, 112, 1152, 532};
    splat::SettingsScreen settings;
    settings.on_enter(context, splat::ScreenArgs{});

    const splat::ThumbnailLabel folder_label{splat::kThumbnailCaptionLines, true};
    const splat::ThumbnailLabel photo_label{splat::kThumbnailCaptionLines, false};
    const splat::Rect photo_area = splat::PhotosScreen::grid_area(context.content, theme);
    for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
        CAPTURE(size);
        state.thumbnail_size = size;
        const int folder = splat::thumbnail_columns(context.content, theme, size, folder_label);
        const int photos = splat::thumbnail_columns(photo_area, theme, size, photo_label);
        const int low = std::min(folder, photos);
        const int high = std::max(folder, photos);
        const std::string expected = std::string(splat::thumbnail_size_name(size)) +
                                     " \xC2\xB7 " +
                                     (low == high ? std::to_string(low)
                                                  : std::to_string(low) + "-" +
                                                        std::to_string(high)) +
                                     " per row";
        CHECK(settings.thumbnail_text(context) == expected);
        // Every number the text shows is a count a real grid draws.
        CHECK(low >= 1);
        CHECK(high >= low);
    }
}

TEST_CASE("ui_thumbs the Settings slider draws in both modes") {
    struct Mode {
        splat::UiTheme theme;
        splat::Rect content;
        uint32_t width;
        uint32_t height;
    };
    std::vector<Mode> modes;
    modes.push_back(Mode{splat::default_theme(), splat::Rect{64, 112, 1152, 532}, 1280, 720});
    {
        splat::UiTheme theme = splat::scaled_theme(splat::default_theme(), 896.0f / 1280.0f);
        const int top = theme.scaled(112);
        const int hint = theme.scaled(76);
        modes.push_back(Mode{theme,
                             splat::Rect{theme.margin, top, 896 - 2 * theme.margin,
                                         504 - top - hint},
                             896, 504});
    }

    for (const Mode& mode : modes) {
        CAPTURE(mode.width);
        splat::UiState state;
        splat::UiServices services;
        services.state = &state;
        splat::ScreenRegistry registry;
        splat::ScreenStack stack{registry};
        splat::UiContext context{services, mode.theme, stack};
        context.content = mode.content;
        splat::SettingsScreen settings;
        settings.on_enter(context, splat::ScreenArgs{});
        // Walk down to the slider row: on the VR panel it starts below the fold,
        // so this also proves the list scrolled it into view.
        for (int i = 0; i < 5; ++i) settings.update(context, press(splat::Down));
        REQUIRE(settings.list().focus() == splat::SettingsScreen::ThumbnailSize);

        splat::UiCanvas canvas(mode.width, mode.height);
        canvas.clear(mode.theme.background);
        settings.draw(context, canvas, context.content);
        REQUIRE(canvas.dirty());

        const splat::Rect row = settings.list().row_rect(
            splat::SettingsScreen::ThumbnailSize, context.content, mode.theme);
        REQUIRE_FALSE(row.empty());
        const splat::Rect area = settings.slider_area(row, mode.theme);
        const splat::Rect track = splat::Slider::track_rect(
            area, settings.thumbnail_text_width(context), mode.theme);
        const int knob_x =
            track.x + (track.w - 1) * splat::kThumbnailSizeDefault /
                          (splat::kThumbnailSizeCount - 1);
        const int knob_y = track.y + track.h / 2;
        const std::size_t offset =
            (static_cast<std::size_t>(knob_y) * canvas.width() +
             static_cast<std::size_t>(knob_x)) * 4u;
        // The focused slider's knob is the theme's light ink, not the backdrop.
        CHECK(canvas.pixels()[offset] > 180);
        CHECK(canvas.pixels()[offset + 1] > 180);
        CHECK(canvas.pixels()[offset + 2] > 180);
    }
}

TEST_CASE("ui_thumbs a size change keeps the focused entry focused and in view") {
    // The grid-level contract first: the focus index never changes, and a scroll
    // computed for the old layout is corrected for the new one.
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect area{0, 0, 1152, 532};
    splat::GridView grid;
    grid.set_cells(std::vector<splat::GridCell>(60));
    splat::apply_thumbnail_layout(grid, area, theme, 2);
    grid.set_focus(40);
    grid.update(splat::UiInput{}, area, theme);
    REQUIRE(cell_in_view(grid, 40, area));
    for (int size : {0, 4, 1, 3, 2}) {
        CAPTURE(size);
        splat::apply_thumbnail_layout(grid, area, theme, size);
        grid.update(splat::UiInput{}, area, theme);
        CHECK(grid.focus() == 40);
        CHECK(cell_in_view(grid, 40, area));
    }
    // The largest page is smaller than the smallest page: the page keys follow
    // the layout instead of a fixed step.
    splat::apply_thumbnail_layout(grid, area, theme, 0);
    grid.update(splat::UiInput{}, area, theme);
    const int small_page = grid.columns(area) * grid.visible_rows(area);
    splat::apply_thumbnail_layout(grid, area, theme, 4);
    grid.update(splat::UiInput{}, area, theme);
    const int large_page = grid.columns(area) * grid.visible_rows(area);
    CHECK(small_page > large_page);
    grid.set_focus(0);
    grid.update(press(splat::PageNext), area, theme);
    CHECK(grid.focus() == large_page);
    CHECK(grid.scroll_row() == grid.visible_rows(area));
}

TEST_CASE("ui_thumbs one picture shape at every size and on every screen") {
    // Stage TF: the picture box a tile draws (GridView::image_rect, which is what
    // the screen paints a photo into) has the photo screen's default shape
    // (176x102 design px, about 1.73:1) at every step, on every area, and with or
    // without a sublabel line. Measuring the drawn box, not the formula that
    // built the cell, is what proves a screen cannot paint a different crop.
    constexpr double kReference = 176.0 / 102.0;
    for (const PanelMode& mode : {tv_mode(), vr_mode()}) {
        CAPTURE(mode.name);
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            for (const splat::Rect& area : {mode.content, mode.photos}) {
                CAPTURE(area.w);
                for (bool sublabel : {false, true}) {
                    CAPTURE(sublabel);
                    splat::GridView grid;
                    grid.set_cells(std::vector<splat::GridCell>(1));
                    grid.show_sublabel = sublabel;
                    splat::apply_thumbnail_layout(grid, area, mode.theme, size);
                    const splat::Rect image =
                        grid.image_rect(grid.cell_rect(0, area), mode.theme);
                    REQUIRE(image.h > 0);
                    const double aspect = static_cast<double>(image.w) / image.h;
                    CHECK(std::abs(aspect - kReference) / kReference < 0.01);
                }
            }
        }
        // The default picture is the reference itself on the design canvas: the
        // shape every step shares.
        const splat::UiTheme design_theme = splat::default_theme();
        splat::GridView design_grid;
        design_grid.set_cells(std::vector<splat::GridCell>(1));
        design_grid.show_sublabel = false;
        splat::apply_thumbnail_layout(design_grid, tv_mode().photos, design_theme,
                                      splat::kThumbnailSizeDefault);
        const splat::Rect design_image =
            design_grid.image_rect(design_grid.cell_rect(0, tv_mode().photos), design_theme);
        // Stage TF2: the default step is 7 columns of 149 px on the design
        // canvas, so its picture is 149x86 - still the one 176:102 shape.
        CHECK(design_image.w == 149);
        CHECK(design_image.h == 86);
    }
}

TEST_CASE("ui_thumbs the grid uses the panel width at every size") {
    // Stage TF: the spare width goes into the column gaps, so the right margin
    // is never wider than one tile gap (the largest step used to end about 200 px
    // before the panel's right edge).
    for (const PanelMode& mode : {tv_mode(), vr_mode()}) {
        CAPTURE(mode.name);
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            for (const splat::Rect& area : {mode.content, mode.photos}) {
                CAPTURE(area.w);
                const splat::ThumbnailLabel label{2, false};
                const splat::ThumbnailMetrics metrics =
                    splat::thumbnail_metrics(area, mode.theme, size, label);
                splat::GridView grid;
                grid.set_cells(std::vector<splat::GridCell>(24));
                grid.show_sublabel = false;
                splat::apply_thumbnail_layout(grid, area, mode.theme, size);
                const int columns = grid.columns(area);
                const int last = grid.cell_rect(columns - 1, area).right();
                const int right_margin = area.right() - last;
                CHECK(right_margin >= 0);
                // The brief's rule: the right margin is no wider than the left
                // (0, the grid starts on the panel's edge) plus one gap. The
                // bound is the tile gap, not the widened `column_gap`, so the
                // test fails for a row that stops short instead of passing it.
                CHECK(right_margin < std::max(1, metrics.gap));
                // And the row really reaches the panel: no more than the last
                // gap's worth of empty space is left over.
                CHECK(last > area.x + area.w / 2);
                CHECK(splat::thumbnail_columns(area, mode.theme, size, label) == columns);
            }
        }
    }
    // The largest step is where the old layout left the panel half empty.
    const splat::UiTheme theme = splat::default_theme();
    const splat::ThumbnailLabel plain_label{2, false};
    const splat::Rect area = tv_mode().content;
    const int tile_gap = splat::thumbnail_metrics(area, theme, 4, plain_label).gap;
    splat::GridView grid;
    grid.set_cells(std::vector<splat::GridCell>(8));
    splat::apply_thumbnail_layout(grid, area, theme, 4);
    CHECK(grid.columns(area) == 5);
    CHECK(area.right() - grid.cell_rect(4, area).right() < tile_gap);
}

TEST_CASE("ui_thumbs the scroll chevrons sit in the gutter, clear of tiles and hints") {
    // Stage TF2 fix: the "more below" chevron used to be drawn inside the grid
    // area's bottom. The photo picker's eight-column step has only 4 px of
    // bottom spare, so it covered the last row's second caption line. Both
    // chevrons now live in the panel's right gutter, level with the grid's
    // edges: outside the area, so no tile or caption can be under them, and
    // still the 28 px the hint pairs keep between them above the hint bar.
    for (const PanelMode& mode : {tv_mode(), vr_mode()}) {
        CAPTURE(mode.name);
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            const splat::Rect& area = mode.photos;
            const splat::UiTheme& theme = mode.theme;
            splat::GridView grid;
            grid.show_sublabel = false;
            splat::apply_thumbnail_layout(grid, area, theme, size);
            const int per_row = grid.columns(area);
            const int visible = grid.visible_rows(area);
            REQUIRE(per_row > 0);
            REQUIRE(visible > 0);
            // Two pages, scrolled by one row: both chevrons show.
            grid.set_cells(
                std::vector<splat::GridCell>(static_cast<std::size_t>(per_row * visible * 2)));
            grid.set_focus(per_row * visible);
            grid.update(splat::UiInput{}, area, theme);
            REQUIRE(grid.scroll_row() > 0);

            splat::UiCanvas canvas(mode.width, mode.height);
            canvas.clear(theme.background);
            grid.draw(canvas, area, theme);

            const splat::Rect down = grid.chevron_rect(area, theme, true);
            const splat::Rect up = grid.chevron_rect(area, theme, false);
            // Outside the grid: no tile, no caption and no focus ring is under
            // either chevron, at any size (the eight-column step included).
            CHECK(down.x > area.right());
            CHECK(up.x > area.right());
            // Inside the panel and the grid's own vertical span.
            CHECK(up.y >= area.y);
            CHECK(down.bottom() <= area.bottom());
            CHECK(down.right() <= static_cast<int>(mode.width));
            CHECK(up.right() <= static_cast<int>(mode.width));
            // The hint bar's ink sits on the band's centre line; the chevron
            // keeps at least the 28 px the hint pairs use between them.
            const int hint_band = theme.scaled(76);
            const int hint_line = static_cast<int>(mode.height) - hint_band / 2;
            CHECK(hint_line - down.bottom() >= theme.scaled(28));
            // No cell box reaches into the gutter (the grid ends at area.right()).
            for (int index = 0; index < per_row * visible; ++index) {
                const splat::Rect cell = grid.cell_rect(index, area);
                CHECK(cell.right() <= area.right());
                CHECK(cell.right() <= down.x);
                CHECK(cell.right() <= up.x);
            }
            // And both chevrons really are drawn in those boxes: the gutter is
            // empty background, so any other pixel there is the chevron's ink.
            auto ink = [&canvas, &theme](const splat::Rect& box) {
                int count = 0;
                for (int y = box.y; y < box.bottom(); ++y) {
                    for (int x = box.x; x < box.right(); ++x) {
                        const std::size_t offset =
                            (static_cast<std::size_t>(y) * canvas.width() +
                             static_cast<std::size_t>(x)) *
                            4u;
                        const splat::Rgba pixel{canvas.pixels()[offset],
                                                canvas.pixels()[offset + 1],
                                                canvas.pixels()[offset + 2],
                                                canvas.pixels()[offset + 3]};
                        if (pixel.r != theme.background.r || pixel.g != theme.background.g ||
                            pixel.b != theme.background.b) {
                            ++count;
                        }
                    }
                }
                return count;
            };
            CHECK(ink(down) > 0);
            CHECK(ink(up) > 0);
        }
    }
}

TEST_CASE("ui_thumbs both screens share the columns and the gap at every size") {
    // Stage TF2, finding 2: at the largest step the folder grid drew four
    // 220 px tiles with ~90 px gaps while the photo picker drew five with
    // ~19 px gaps. Both screens now draw the same column count and the same
    // 17-20 px gap (18 design px, 13 px at VR's 0.7 theme) at every step, and
    // the row still reaches the panel's right edge.
    for (const PanelMode& mode : {tv_mode(), vr_mode()}) {
        CAPTURE(mode.name);
        const splat::ThumbnailLabel folder_label{2, true};
        const splat::ThumbnailLabel photo_label{2, false};
        for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
            CAPTURE(size);
            const splat::ThumbnailMetrics folder =
                splat::thumbnail_metrics(mode.content, mode.theme, size, folder_label);
            const splat::ThumbnailMetrics photo =
                splat::thumbnail_metrics(mode.photos, mode.theme, size, photo_label);
            // The labels differ (a folder tile reserves a sublabel line), the
            // tile size, the column count and the gaps must not.
            CHECK(folder.cell_width == photo.cell_width);
            CHECK(folder.gap == photo.gap);
            CHECK(folder.column_gap == photo.column_gap);
            const int folder_columns =
                splat::thumbnail_columns(mode.content, mode.theme, size, folder_label);
            CHECK(folder_columns ==
                  splat::thumbnail_columns(mode.photos, mode.theme, size, photo_label));
            CHECK(folder_columns >= 5);
            // The gap is the design's 18 px: inside 17-20 px on the TV, the same
            // physical size on the VR panel's scaled theme.
            CHECK(folder.column_gap >= mode.theme.scaled(17));
            CHECK(folder.column_gap <= mode.theme.scaled(20));
            // The row fills the panel (the 64 px margins): the same right-margin
            // rule the panel-width test applies to one screen.
            splat::GridView grid;
            grid.set_cells(std::vector<splat::GridCell>(1));
            grid.show_sublabel = false;
            splat::apply_thumbnail_layout(grid, mode.photos, mode.theme, size);
            const int columns = grid.columns(mode.photos);
            const int last = grid.cell_rect(columns - 1, mode.photos).right();
            CHECK(mode.photos.right() - last < folder.column_gap);
            CHECK(last > mode.photos.x + mode.photos.w / 2);
        }
    }

    // The design canvas, pinned exactly: the five steps are 9, 8, 7, 6 and 5
    // columns of 112, 128, 149, 177 and 216 px, with one 18 px gap.
    const splat::UiTheme theme = splat::default_theme();
    const splat::ThumbnailLabel folder_label{2, true};
    const splat::ThumbnailLabel photo_label{2, false};
    const int expected_columns[splat::kThumbnailSizeCount] = {9, 8, 7, 6, 5};
    const int expected_width[splat::kThumbnailSizeCount] = {112, 128, 149, 177, 216};
    for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
        CAPTURE(size);
        const splat::ThumbnailMetrics folder =
            splat::thumbnail_metrics(tv_mode().content, theme, size, folder_label);
        const splat::ThumbnailMetrics photo =
            splat::thumbnail_metrics(tv_mode().photos, theme, size, photo_label);
        CHECK(folder.cell_width == expected_width[size]);
        CHECK(photo.cell_width == expected_width[size]);
        CHECK(folder.column_gap == 18);
        CHECK(photo.column_gap == 18);
        CHECK(splat::thumbnail_columns(tv_mode().content, theme, size, folder_label) ==
              expected_columns[size]);
        CHECK(splat::thumbnail_columns(tv_mode().photos, theme, size, photo_label) ==
              expected_columns[size]);
    }
}

TEST_CASE("ui_thumbs a photo tile fits the whole picture, portrait and turned") {
    // Stage TF2, finding 1: a plain portrait photo was cropped to fill its tile
    // while a turned photo showed its whole frame with dark side bars. Every
    // photo tile (GridCell::fit) now fits the whole picture inside the tile,
    // like a file browser: a portrait is narrower than the tile with the
    // backdrop on both sides, and the picture's own edge is visible. A picture
    // that already has the tile's shape still fills it.
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rgba white{250, 250, 250, 255};
    const splat::Rgba red{220, 30, 30, 255};

    // A source whose outermost 12 px are red: a crop that removes an edge is
    // then visible as white where the frame's own border should be (the border
    // is at least two drawn pixels wide at every tile size).
    auto bordered = [&](uint32_t w, uint32_t h) {
        splat::UiImage image;
        image.resize(w, h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const bool edge = x < 12 || y < 12 || x + 12 >= w || y + 12 >= h;
                uint8_t* pixel = image.pixel(x, y);
                const splat::Rgba color = edge ? red : white;
                pixel[0] = color.r;
                pixel[1] = color.g;
                pixel[2] = color.b;
                pixel[3] = color.a;
            }
        }
        return image;
    };
    const splat::UiImage portrait = bordered(240, 320);   // 3:4 portrait
    const splat::UiImage landscape = bordered(320, 240);  // the source of the turn

    const splat::Rect area = tv_mode().content;
    for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
        CAPTURE(size);
        splat::UiCanvas canvas(1280, 720);
        canvas.clear(theme.background);
        auto pixel = [&canvas](int x, int y) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * canvas.width() + static_cast<std::size_t>(x)) * 4u;
            return splat::Rgba{canvas.pixels()[offset], canvas.pixels()[offset + 1],
                               canvas.pixels()[offset + 2], canvas.pixels()[offset + 3]};
        };
        const auto is_photo = [](splat::Rgba c) { return c.r > 100 && c.g > 100 && c.b > 100; };
        const auto is_red = [](splat::Rgba c) { return c.r > 140 && c.g < 80 && c.b < 80; };
        // The tile's backdrop (theme.background under the unfocused veil). Any
        // pixel that is not the backdrop belongs to the picture, red border
        // included.
        const auto is_plate = [](splat::Rgba c) { return c.r < 60 && c.g < 60 && c.b < 70; };
        const auto is_picture = [&is_plate](splat::Rgba c) { return !is_plate(c); };

        struct Case {
            const splat::UiImage* thumb;
            int quarters;
            int source_w;
            int source_h;
            const char* name;
        };
        const Case cases[] = {
            {&portrait, 0, 240, 320, "portrait"},
            {&landscape, 1, 320, 240, "turned landscape"},  // turned: 240x320
        };
        for (const Case& item : cases) {
            CAPTURE(item.name);
            splat::GridView grid;
            grid.show_sublabel = false;
            splat::apply_thumbnail_layout(grid, area, theme, size);
            std::vector<splat::GridCell> cells(2);
            cells[0].caption = "focused";
            cells[1].caption = "photo";
            cells[1].thumb = item.thumb;
            cells[1].turn_quarters = item.quarters;
            cells[1].fit = true;
            grid.set_cells(std::move(cells));
            grid.set_focus(0);  // the photo tile is unfocused: no 5 % grow
            grid.draw(canvas, area, theme);

            const splat::Rect box = grid.cell_rect(1, area);
            const splat::Rect image = grid.image_rect(box, theme);
            // The turned picture's own dimensions: a quarter turn swaps them.
            const int turned_w = (item.quarters % 2 != 0) ? item.source_h : item.source_w;
            const int turned_h = (item.quarters % 2 != 0) ? item.source_w : item.source_h;
            const int expected_h = image.h;  // both are portrait: height-limited
            const int expected_w = static_cast<int>(
                std::lround(static_cast<double>(image.h) * turned_w / turned_h));
            REQUIRE(expected_w < image.w);

            // The drawn rectangle: the run of picture pixels on the centre row
            // and on the centre column.
            const int row = image.y + image.h / 2;
            int left = -1;
            int right = -1;
            for (int x = image.x; x < image.right(); ++x) {
                if (!is_picture(pixel(x, row))) continue;
                if (left < 0) left = x;
                right = x;
            }
            REQUIRE(left >= 0);
            const int column = image.x + image.w / 2;
            int top = -1;
            int bottom = -1;
            for (int y = image.y; y < image.bottom(); ++y) {
                if (!is_picture(pixel(column, y))) continue;
                if (top < 0) top = y;
                bottom = y;
            }
            REQUIRE(top >= 0);
            const int drawn_w = right - left + 1;
            const int drawn_h = bottom - top + 1;
            CHECK(std::abs(drawn_w - expected_w) <= 1);
            CHECK(std::abs(drawn_h - expected_h) <= 1);
            // The picture keeps its own aspect: the whole frame, not a crop.
            const double aspect = static_cast<double>(drawn_w) / drawn_h;
            const double source_aspect =
                static_cast<double>(turned_w) / static_cast<double>(turned_h);
            CHECK(std::abs(aspect - source_aspect) / source_aspect < 0.03);
            // A 4:3 photo turned one quarter is drawn in a 3:4 fit rectangle:
            // the thumbnail follows the turn with no badge naming the angle.
            if (item.quarters % 2 != 0) {
                CHECK(item.source_w * 3 == item.source_h * 4);
                CHECK(std::abs(drawn_w * 4 - drawn_h * 3) <= 2);
            }
            // Fit, not fill: the tile's backdrop shows on both sides, equally.
            const int left_bar = left - image.x;
            const int right_bar = image.right() - right - 1;
            CHECK(left_bar > 4);
            CHECK(std::abs(left_bar - right_bar) <= 1);
            CHECK(is_plate(pixel(image.x + 1, row)));
            CHECK(is_plate(pixel(image.right() - 2, row)));
            // The whole frame is inside: the source's own red border is drawn at
            // the top of the picture (a cover crop of this portrait cut it).
            CHECK(is_red(pixel(column, top + 1)));
            CHECK(is_photo(pixel(column, row)));
        }

        // A picture that already has the tile's shape fills it, as before.
        splat::GridView square;
        square.show_sublabel = false;
        splat::apply_thumbnail_layout(square, area, theme, size);
        const splat::Rect plain_box = square.cell_rect(1, area);
        const splat::Rect plain_image = square.image_rect(plain_box, theme);
        const splat::UiImage same_shape =
            bordered(static_cast<uint32_t>(plain_image.w), static_cast<uint32_t>(plain_image.h));
        std::vector<splat::GridCell> cells(2);
        cells[0].caption = "focused";
        cells[1].caption = "photo";
        cells[1].thumb = &same_shape;
        cells[1].fit = true;
        square.set_cells(std::move(cells));
        square.set_focus(0);
        square.draw(canvas, area, theme);
        const int row = plain_image.y + plain_image.h / 2;
        CHECK(is_red(pixel(plain_image.x + 1, row)));
        CHECK(is_red(pixel(plain_image.right() - 2, row)));
        CHECK(is_red(pixel(plain_image.x + plain_image.w / 2, plain_image.y + 1)));
        // Stage TF2 fix: the fitted picture is clipped to the plate's rounded
        // corners, like blit_cover. A picture of exactly the box's shape spans
        // the whole box, so before the clip it painted square corners over the
        // tile's rounded ones.
        CHECK(is_plate(pixel(plain_image.x + 1, plain_image.y + 1)));
        CHECK(is_plate(pixel(plain_image.right() - 2, plain_image.y + 1)));
        CHECK(is_plate(pixel(plain_image.x + 1, plain_image.bottom() - 2)));
        CHECK(is_plate(pixel(plain_image.right() - 2, plain_image.bottom() - 2)));

        // A photo wider than the tile's 176:102 shape is width-limited: its
        // fitted rectangle spans the box's full width and its first rows reach
        // the corner cut, which must stay plate.
        splat::UiImage wide = bordered(320, 180);
        std::vector<splat::GridCell> wide_cells(2);
        wide_cells[0].caption = "focused";
        wide_cells[1].caption = "photo";
        wide_cells[1].thumb = &wide;
        wide_cells[1].fit = true;
        square.set_cells(std::move(wide_cells));
        square.set_focus(0);
        canvas.clear(theme.background);
        square.draw(canvas, area, theme);
        CHECK(is_red(pixel(plain_image.x + plain_image.w / 2, plain_image.y + 2)));
        CHECK(is_plate(pixel(plain_image.x + 1, plain_image.y + 1)));
        CHECK(is_plate(pixel(plain_image.right() - 2, plain_image.y + 1)));
    }
}

TEST_CASE("ui_thumbs one label rule: the same lines focused or not") {
    // Stage TF: every tile wraps its name onto the same two caption lines, so
    // the label block has one height whether or not the tile is focused, and a
    // cut name ends in an ellipsis. The old rule let the focused tile wrap while
    // its neighbours kept one line.
    const splat::UiTheme theme = splat::default_theme();
    const splat::Rect area{0, 0, 1152, 532};
    const std::string long_name = "grandparents garden and the old orchard in May";

    splat::GridView grid;
    std::vector<splat::GridCell> cells(2);
    cells[0].caption = long_name;
    cells[1].caption = long_name;
    grid.set_cells(std::move(cells));
    grid.show_sublabel = false;
    splat::apply_thumbnail_layout(grid, area, theme, 2);
    REQUIRE(grid.caption_lines == splat::kThumbnailCaptionLines);
    grid.set_focus(0);

    const splat::Rect focused_box = grid.cell_rect(0, area);
    const splat::Rect other_box = grid.cell_rect(1, area);
    const splat::Rect focused_image = grid.image_rect(focused_box, theme);
    const splat::Rect other_image = grid.image_rect(other_box, theme);
    CHECK(focused_image.h == other_image.h);
    const int label_height = focused_box.h - focused_image.h;
    CHECK(label_height == splat::thumbnail_label_height(theme, 2, false));

    splat::UiCanvas canvas(1280, 720);
    canvas.clear(theme.background);
    grid.draw(canvas, area, theme);
    // The ink rows of the caption band, one band per tile. Both tiles draw the
    // same number of lines at the same offsets.
    auto ink_rows = [&canvas, &theme](const splat::Rect& box, int top, int height) {
        std::vector<int> rows;
        for (int y = box.y + top; y < box.y + top + height; ++y) {
            bool ink = false;
            for (int x = box.x + 1; x < box.right() - 1 && !ink; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * canvas.width() + static_cast<std::size_t>(x)) *
                    4u;
                const splat::Rgba pixel{canvas.pixels()[offset], canvas.pixels()[offset + 1],
                                        canvas.pixels()[offset + 2], canvas.pixels()[offset + 3]};
                ink = pixel.r != theme.background.r || pixel.g != theme.background.g ||
                      pixel.b != theme.background.b;
            }
            if (ink) rows.push_back(y - box.y);
        }
        return rows;
    };
    const std::vector<int> focused_rows =
        ink_rows(focused_box, focused_image.h + 24, label_height - 24);
    const std::vector<int> other_rows = ink_rows(other_box, other_image.h + 24, label_height - 24);
    REQUIRE_FALSE(focused_rows.empty());
    REQUIRE_FALSE(other_rows.empty());
    CHECK(focused_rows == other_rows);
    // Two lines are actually used for this name, and the wrap ends in an
    // ellipsis (the last of the two lines is cut).
    const std::vector<std::string> lines =
        splat::wrap_lines(long_name, theme.body_size - 2, focused_box.w - 4, 2);
    REQUIRE(lines.size() == 2);
    CHECK(lines.back().size() >= 3);
    CHECK(lines.back().compare(lines.back().size() - 3, 3, "\xE2\x80\xA6") == 0);
    // Both tiles draw that second line (the unfocused one used to keep one
    // line): the ink is there for the focused and the unfocused tile alike.
    CHECK(focused_rows.size() == other_rows.size());
}

TEST_CASE("ui_thumbs every thumbnail screen applies the one shared layout") {
    splat_test::TempDir temp("ui-thumbs-layout");
    const Path scenes_root = temp.path() / "scenes";
    const Path inbox = temp.path() / "inbox";
    write_scene(scenes_root / "trip" / "scene-a");
    write_jpeg(inbox / "a.jpg", 10);
    std::filesystem::create_directories(inbox);
    write_jpeg(inbox / "b.jpg", 60);
    write_jpeg(inbox / "c.jpg", 110);

    splat::UiState state;
    splat::UiServices services;
    services.paths.scenes = scenes_root;
    services.paths.inbox = inbox;
    services.paths.state = temp.path() / "state";
    services.state = &state;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{64, 112, 1152, 532};

    splat::AlbumsScreen albums;
    albums.on_enter(context, splat::ScreenArgs{});

    splat::ScenesScreen scenes;
    splat::ScreenArgs scene_args;
    scene_args.a = "trip";
    scenes.on_enter(context, scene_args);

    splat::PhotosScreen photos;
    splat::ScreenArgs photo_args;
    photo_args.a = "trip";
    photos.on_enter(context, photo_args);

    for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
        CAPTURE(size);
        state.thumbnail_size = size;
        albums.update(context, splat::UiInput{});
        scenes.update(context, splat::UiInput{});
        photos.update(context, splat::UiInput{});

        // The label block a screen draws is part of its metrics: the scene and
        // album tiles reserve a sublabel line, the photo tiles only when a splat
        // tile is listed.
        const splat::ThumbnailLabel album_label{albums.grid().caption_lines,
                                                albums.grid().show_sublabel};
        const splat::ThumbnailLabel scene_label{scenes.grid().caption_lines,
                                                scenes.grid().show_sublabel};
        const splat::Rect photo_area = splat::PhotosScreen::grid_area(context.content, theme);
        const splat::ThumbnailLabel photo_label{photos.grid().caption_lines,
                                                photos.grid().show_sublabel};
        const splat::ThumbnailMetrics album_metrics =
            splat::thumbnail_metrics(context.content, theme, size, album_label);
        const splat::ThumbnailMetrics scene_metrics =
            splat::thumbnail_metrics(context.content, theme, size, scene_label);
        const splat::ThumbnailMetrics photo_metrics =
            splat::thumbnail_metrics(photo_area, theme, size, photo_label);

        CHECK(albums.grid().caption_lines == splat::kThumbnailCaptionLines);
        CHECK(scenes.grid().caption_lines == splat::kThumbnailCaptionLines);
        CHECK(photos.grid().caption_lines == splat::kThumbnailCaptionLines);
        CHECK(albums.grid().cell_width == album_metrics.cell_width);
        CHECK(albums.grid().cell_height == album_metrics.cell_height);
        CHECK(albums.grid().gap == album_metrics.gap);
        CHECK(albums.grid().column_gap == album_metrics.column_gap);
        CHECK(scenes.grid().cell_width == scene_metrics.cell_width);
        CHECK(scenes.grid().cell_height == scene_metrics.cell_height);
        CHECK(scenes.grid().gap == scene_metrics.gap);
        CHECK(scenes.grid().column_gap == scene_metrics.column_gap);
        CHECK(photos.grid().cell_width == photo_metrics.cell_width);
        CHECK(photos.grid().cell_height == photo_metrics.cell_height);
        CHECK(photos.grid().gap == photo_metrics.gap);
        CHECK(photos.grid().column_gap == photo_metrics.column_gap);

        // And the screens' visible counts are the shared function's counts.
        CHECK(albums.grid().columns(context.content) *
                  albums.grid().visible_rows(context.content) ==
              splat::thumbnail_entries_visible(context.content, theme, size, album_label));
        CHECK(scenes.grid().columns(context.content) *
                  scenes.grid().visible_rows(context.content) ==
              splat::thumbnail_entries_visible(context.content, theme, size, scene_label));
        CHECK(photos.grid().columns(photo_area) * photos.grid().visible_rows(photo_area) ==
              splat::thumbnail_entries_visible(photo_area, theme, size, photo_label));

        // The picture box each screen really draws is the one shape at this step,
        // so a screen that kept its own image rectangle cannot pass this.
        constexpr double kReference = 176.0 / 102.0;
        const std::pair<const splat::GridView*, splat::Rect> screens[] = {
            {&albums.grid(), context.content},
            {&scenes.grid(), context.content},
            {&photos.grid(), photo_area},
        };
        for (const auto& screen : screens) {
            const splat::Rect image = screen.first->image_rect(
                screen.first->cell_rect(0, screen.second), theme);
            REQUIRE(image.h > 0);
            const double aspect = static_cast<double>(image.w) / image.h;
            CHECK(std::abs(aspect - kReference) / kReference < 0.01);
        }
    }

    // The fit is a photo-tile rule (stage TF2): the photo picker's photo cells
    // show the whole frame, a scene tile keeps its cover crop.
    state.thumbnail_size = splat::kThumbnailSizeDefault;
    photos.update(context, splat::UiInput{});
    scenes.update(context, splat::UiInput{});
    REQUIRE_FALSE(photos.grid().cells().empty());
    REQUIRE_FALSE(scenes.grid().cells().empty());
    CHECK(photos.grid().cells()[0].fit);
    CHECK_FALSE(scenes.grid().cells()[0].fit);
}

TEST_CASE("ui_thumbs a turned thumbnail keeps its aspect at every size") {
    // The photo picker's turn is drawn with the contained fit (canvas
    // blit_turned_fit), never stretched into the tile: at every size the 2:1
    // turned image is a 1:2 portrait centred on the plate, and the plate is
    // visible on both sides.
    const splat::UiTheme theme = splat::default_theme();
    splat::UiImage source;
    source.resize(40, 20);
    const splat::Rgba red{220, 30, 30, 255};
    const splat::Rgba green{30, 220, 30, 255};
    const splat::Rgba blue{30, 30, 220, 255};
    const splat::Rgba white{250, 250, 250, 255};
    for (uint32_t y = 0; y < 20; ++y) {
        for (uint32_t x = 0; x < 40; ++x) {
            const bool right = x >= 20;
            const bool bottom = y >= 10;
            const splat::Rgba color = bottom ? (right ? white : blue) : (right ? green : red);
            uint8_t* pixel = source.pixel(x, y);
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            pixel[3] = color.a;
        }
    }

    const splat::Rect area{0, 0, 1152, 532};
    for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
        CAPTURE(size);
        splat::GridView grid;
        grid.show_sublabel = false;
        splat::apply_thumbnail_layout(grid, area, theme, size);
        std::vector<splat::GridCell> cells(2);
        cells[0].caption = "first";
        cells[1].caption = "turned";
        cells[1].thumb = &source;
        cells[1].turn_quarters = 1;
        grid.set_cells(std::move(cells));
        grid.set_focus(0);  // the turned tile is unfocused: no 5 % grow

        splat::UiCanvas canvas(1280, 720);
        canvas.clear(theme.background);
        grid.draw(canvas, area, theme);

        const splat::Rect box = grid.cell_rect(1, area);
        const splat::Rect image = grid.image_rect(box, theme);
        const float scale = std::min(static_cast<float>(image.w) / 20.0f,
                                     static_cast<float>(image.h) / 40.0f);
        const int draw_w = static_cast<int>(std::lround(20.0f * scale));
        const int draw_h = static_cast<int>(std::lround(40.0f * scale));
        REQUIRE(draw_w > 6);
        REQUIRE(draw_h > 6);
        // Fit, not fill: the turned frame is narrower than the tile.
        CHECK(draw_w <= image.w);
        CHECK(draw_h <= image.h);
        const bool letterboxed = draw_w < image.w || draw_h < image.h;
        CHECK(letterboxed);
        const int left = image.x + (image.w - draw_w) / 2;
        const int top = image.y + (image.h - draw_h) / 2;

        auto pixel = [&canvas](int x, int y) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * canvas.width() + static_cast<std::size_t>(x)) * 4u;
            return splat::Rgba{canvas.pixels()[offset], canvas.pixels()[offset + 1],
                               canvas.pixels()[offset + 2], canvas.pixels()[offset + 3]};
        };
        // The plate shows on the letterbox (dimmed like every unfocused tile);
        // the four turned quadrants are in place (source bottom-left blue ->
        // top-left, top-left red -> top-right, both dimmed by the unfocused
        // tile's black veil).
        const splat::Rgba plate = pixel(left - 2, top + draw_h / 2);
        CHECK(plate.r < 40);
        CHECK(plate.g < 40);
        CHECK(plate.b < 60);
        const splat::Rgba top_left = pixel(left + 2, top + 2);
        CHECK(top_left.b > 140);
        CHECK(top_left.r < 100);
        CHECK(top_left.g < 100);
        const splat::Rgba top_right = pixel(left + draw_w - 3, top + 2);
        CHECK(top_right.r > 140);
        CHECK(top_right.g < 100);
        CHECK(top_right.b < 100);
    }
}

TEST_CASE("ui_thumbs the photo picker keeps its focused tile on a reveal") {
    splat_test::TempDir temp("ui-thumbs-reveal");
    const Path inbox = temp.path() / "inbox";
    for (int index = 0; index < 12; ++index) {
        write_jpeg(inbox / ("p" + std::to_string(index) + ".jpg"),
                   static_cast<uint8_t>(10 + index * 10));
    }

    splat::UiState state;
    splat::UiServices services;
    services.paths.scenes = temp.path() / "scenes";
    services.paths.inbox = inbox;
    services.paths.state = temp.path() / "state";
    services.state = &state;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{64, 112, 1152, 532};

    splat::PhotosScreen photos;
    splat::ScreenArgs args;
    args.a = "trip";
    photos.on_enter(context, args);
    photos.update(context, splat::UiInput{});
    REQUIRE(photos.grid().cells().size() == 12);

    // Down to the second row, then three to the right: one entry past the first
    // column of that row.
    photos.update(context, press(splat::Down));
    for (int index = 0; index < 3; ++index) photos.update(context, press(splat::Right));
    const int expected_focus = photos.grid().columns(
                                   splat::PhotosScreen::grid_area(context.content, theme)) + 3;
    REQUIRE(photos.grid().focus() == expected_focus);

    // The user opens Settings and enlarges the tiles; the reveal must keep the
    // same entry focused, and the new layout must scroll it into view.
    state.thumbnail_size = 4;
    photos.on_enter(context, args);
    photos.update(context, splat::UiInput{});
    CHECK(photos.grid().focus() == expected_focus);
    const splat::Rect area = splat::PhotosScreen::grid_area(context.content, theme);
    CHECK(cell_in_view(photos.grid(), expected_focus, area));
}

TEST_CASE("ui_thumbs a size change does not decode a thumbnail again") {
    splat_test::TempDir temp("ui-thumbs-decode");
    const Path inbox = temp.path() / "inbox";
    write_jpeg(inbox / "a.jpg", 10);
    write_jpeg(inbox / "b.jpg", 60);
    write_jpeg(inbox / "c.jpg", 110);

    splat::UiState state;
    splat::UiServices services;
    services.paths.scenes = temp.path() / "scenes";
    services.paths.inbox = inbox;
    services.paths.state = temp.path() / "state";
    services.state = &state;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{64, 112, 1152, 532};

    splat::PhotosScreen photos;
    splat::ScreenArgs args;
    args.a = "trip";
    photos.on_enter(context, args);

    // Let the worker decode the three tiles (the screen asks once per key).
    bool decoded = false;
    for (int frame = 0; frame < 300 && !decoded; ++frame) {
        photos.update(context, splat::UiInput{});
        decoded = photos.thumbnail_loads() == 3;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(photos.thumbnail_loads() == 3);
    REQUIRE(photos.grid().cells().size() == 3);
    REQUIRE(photos.grid().cells()[0].thumb != nullptr);
    const std::size_t loads = photos.thumbnail_loads();

    for (int size = 0; size < splat::kThumbnailSizeCount; ++size) {
        CAPTURE(size);
        state.thumbnail_size = size;
        photos.update(context, splat::UiInput{});
        photos.update(context, splat::UiInput{});
        CHECK(photos.thumbnail_loads() == loads);
        // The decoded tile stays on the cell: a resize redraws it, it does not
        // drop it and ask again.
        REQUIRE(photos.grid().cells().size() == 3);
        CHECK(photos.grid().cells()[0].thumb != nullptr);
    }
}
