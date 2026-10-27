// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_pattern.h"

#include <algorithm>
#include <utility>

#include "ui/theme.h"

namespace splat {
namespace {

const char* const kTileCaptions[6] = {"ALBUMS", "PHOTOS", "CAPTURES",
                                      "MODELS", "JOBS",   "SETTINGS"};

// The action list the cursor and the log react to, in the order the input map
// names them (core/ui/ui_input.h). The strings are what a shot shows, so a
// script's line is recognisable without the log.
struct ActionName {
    uint32_t bit;
    const char* name;
};

const ActionName kActionNames[] = {
    {Up, "UP"},           {Down, "DOWN"},           {Left, "LEFT"},
    {Right, "RIGHT"},     {Select, "SELECT"},       {Back, "BACK"},
    {PagePrev, "PAGE PREV"}, {PageNext, "PAGE NEXT"}, {Secondary, "SECONDARY"},
    {Tooltips, "TOOLTIPS"}, {Menu, "MENU"},
};

// Title bar across the top, accent rule under it, size and body scale at right.
void draw_title_bar(UiCanvas& canvas, const UiTheme& theme) {
    canvas.fill_rect(0, 0, 1280, 62, theme.panel);
    canvas.fill_rect(0, 62, 1280, 2, theme.accent);
    canvas.text(theme.padding, 12, "SPLAT VR - UI PREVIEW", theme.heading_size, theme.text);

    const std::string right = "1280X720  BODY " + std::to_string(theme.body_size) + " PX";
    const uint32_t width = UiCanvas::text_width(right, theme.body_size);
    canvas.text(1280 - theme.padding - static_cast<int>(width), 23, right, theme.body_size,
                theme.dim_text);
}

// 3x2 grid of tiles with a colour bar, an index and a caption.
void draw_tile_grid(UiCanvas& canvas, const UiTheme& theme) {
    const Rgba bars[6] = {theme.accent, theme.focus, theme.ok,
                          theme.warning, theme.dim_text, theme.text};
    constexpr int kTileWidth = 240;
    constexpr int kTileHeight = 136;
    constexpr int kGap = 16;
    const int origin_x = theme.padding;
    const int origin_y = 88;
    for (int index = 0; index < 6; ++index) {
        const int column = index % 3;
        const int row = index / 3;
        const int x = origin_x + column * (kTileWidth + kGap);
        const int y = origin_y + row * (kTileHeight + kGap);
        canvas.fill_rect(x, y, kTileWidth, kTileHeight, theme.panel);
        canvas.fill_rect(x, y, 6, kTileHeight, bars[index]);
        canvas.text(x + theme.padding, y + theme.padding, "0" + std::to_string(index + 1),
                    theme.body_size, theme.dim_text);
        canvas.text(x + theme.padding, y + kTileHeight - theme.padding - 16, kTileCaptions[index],
                    theme.body_size, theme.text);
    }
}

// Four scene rows, the third focused, counts right aligned.
void draw_scene_list(UiCanvas& canvas, const UiTheme& theme) {
    const char* const names[4] = {"KITCHEN 12 AUG", "PIZZA CLOSE UP", "HALLWAY",
                                  "GARDEN NIGHT"};
    const char* const counts[4] = {"24", "8", "41", "112"};
    constexpr int kListWidth = 752;
    constexpr int kRowHeight = 40;
    constexpr int kFocusedRow = 2;
    const int x = theme.padding;
    int y = 420;

    canvas.text(x, 400, "SCENES - 4 OF 12", theme.body_size, theme.dim_text);
    for (int row = 0; row < 4; ++row) {
        const bool focused = row == kFocusedRow;
        if (focused) {
            canvas.fill_rect(x, y, kListWidth, kRowHeight, theme.selected);
            canvas.fill_rect(x, y + 4, 4, kRowHeight - 8, theme.focus);
        }
        const int text_y = y + (kRowHeight - UiTheme::text_height(theme.body_size)) / 2;
        canvas.text(x + theme.padding, text_y, names[row], theme.body_size,
                    focused ? theme.focus : theme.text);
        const uint32_t count_width = UiCanvas::text_width(counts[row], theme.body_size);
        canvas.text(x + kListWidth - theme.padding - static_cast<int>(count_width), text_y,
                    counts[row], theme.body_size, theme.dim_text);
        y += kRowHeight;
    }
}

// Right column: a progress bar at 58 %, state lines and a focus outline.
void draw_status_column(UiCanvas& canvas, const UiTheme& theme) {
    constexpr int kX = 800;
    constexpr int kWidth = 464;
    canvas.text(kX, 400, "CONVERT", theme.body_size, theme.dim_text);
    canvas.bar(kX, 420, kWidth, 24, 0.58f, theme.panel, theme.accent);
    canvas.text(kX, 452, "58 PERCENT", theme.body_size, theme.text);
    canvas.text(kX, 484, "1 FAILED - RETRY", theme.body_size, theme.warning);
    canvas.text(kX, 512, "2 DONE", theme.body_size, theme.ok);
    canvas.text(kX, 552, "SQUARE CANCELS - CIRCLE BACK", theme.body_size, theme.dim_text);

    canvas.stroke_rect(kX, 592, kWidth, 48, 2, theme.focus);
    canvas.text(kX + theme.padding, 606, "FOCUS OUTLINE", theme.body_size, theme.focus);
}

// Bottom left: the same words at every scale, bottoms aligned, to read the
// glyph size at TV and VR distance.
void draw_text_ruler(UiCanvas& canvas, const UiTheme& theme) {
    canvas.text(theme.padding, 604, "TEXT SIZES (PX)", theme.body_size, theme.dim_text);
    int x = theme.padding;
    for (const int size : {theme.small_size, theme.body_size, theme.heading_size, theme.title_size}) {
        const std::string sample = "Size " + std::to_string(size);
        const int height = UiTheme::text_height(size);
        canvas.text(x, 720 - 16 - height, sample, size, theme.text);
        x += static_cast<int>(UiCanvas::text_width(sample, size)) + theme.padding;
    }
}

}  // namespace

void draw_test_pattern(UiCanvas& canvas) {
    const UiTheme theme = default_theme();
    canvas.clear(theme.background);
    draw_title_bar(canvas, theme);
    draw_tile_grid(canvas, theme);
    draw_scene_list(canvas, theme);
    draw_status_column(canvas, theme);
    draw_text_ruler(canvas, theme);
}

TestPatternContent::TestPatternContent() = default;

void TestPatternContent::note(const char* action) {
    actions_.emplace_back(action);
    while (actions_.size() > 5) actions_.pop_front();
}

void TestPatternContent::update(const UiInput& input) {
    bool changed = false;
    const auto move = [&](uint32_t bit, int dx, int dy, const char* name) {
        if ((input.pressed & bit) == 0) return;
        cursor_x_ = std::clamp(cursor_x_ + dx, 0, kCanvasWidth - kCursorSize);
        cursor_y_ = std::clamp(cursor_y_ + dy, 0, kCanvasHeight - kCursorSize);
        note(name);
        changed = true;
    };
    move(Up, 0, -kCursorStep, "UP");
    move(Down, 0, kCursorStep, "DOWN");
    move(Left, -kCursorStep, 0, "LEFT");
    move(Right, kCursorStep, 0, "RIGHT");
    if ((input.pressed & Select) != 0) {
        cursor_lit_ = !cursor_lit_;
        note("SELECT");
        changed = true;
    }
    // The remaining actions do not move the cursor, but a script that presses
    // them must still leave a mark in the frame (the log below the title bar).
    for (const ActionName& action : kActionNames) {
        if (action.bit == Up || action.bit == Down || action.bit == Left ||
            action.bit == Right || action.bit == Select) {
            continue;
        }
        if ((input.pressed & action.bit) == 0) continue;
        note(action.name);
        changed = true;
    }
    if (changed) dirty_ = true;
}

void TestPatternContent::draw_cursor(UiCanvas& canvas) const {
    const UiTheme theme = default_theme();
    canvas.fill_rect(cursor_x_, cursor_y_, kCursorSize, kCursorSize,
                     cursor_lit_ ? theme.focus : theme.ok);
    canvas.stroke_rect(cursor_x_, cursor_y_, kCursorSize, kCursorSize, 3, theme.text);
}

void TestPatternContent::draw_actions(UiCanvas& canvas) const {
    const UiTheme theme = default_theme();
    constexpr int kX = 800;
    canvas.text(kX, 72, "CURSOR " + std::to_string(cursor_x_) + "," +
                            std::to_string(cursor_y_) + (cursor_lit_ ? " AMBER" : " GREEN"),
                theme.body_size, theme.dim_text);
    canvas.text(kX, 100, "LAST ACTIONS", theme.body_size, theme.dim_text);
    int y = 124;
    for (const std::string& action : actions_) {
        canvas.text(kX, y, action, theme.body_size, theme.text);
        y += 22;
    }
}

bool TestPatternContent::draw(UiCanvas& canvas) {
    if (!dirty_) return false;
    dirty_ = false;
    draw_test_pattern(canvas);
    draw_cursor(canvas);
    draw_actions(canvas);
    return true;
}

}  // namespace splat
