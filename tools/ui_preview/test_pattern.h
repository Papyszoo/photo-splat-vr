// The canvas test pattern: W2's canvas demo, plus a scriptable cursor. A
// desktop diagnostic only (ui_preview "pattern"): the shipped panel is UiApp,
// so this lives with the preview tool and is not in the title.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deque>
#include <string>

#include "ui/canvas.h"
#include "ui/ui_input.h"

namespace splat {

// The static part of the pattern: every canvas primitive at the sizes the panel
// uses (1280x720), with no cursor or action log. Clears the canvas first.
void draw_test_pattern(UiCanvas& canvas);

// Pattern + a 64 px cursor square. D-pad zero bits move it in 32 px steps, Cross
// toggles its colour, and the last five actions are listed at the top right, so
// a script's button presses are visible in a frame dump. draw() repaints only
// after an update() that changed something.
class TestPatternContent : public IPanelContent {
public:
    static constexpr int kCanvasWidth = 1280;
    static constexpr int kCanvasHeight = 720;
    static constexpr int kCursorSize = 64;
    static constexpr int kCursorStep = 32;

    TestPatternContent();

    void update(const UiInput& input) override;
    bool draw(UiCanvas& canvas) override;

    int cursor_x() const { return cursor_x_; }
    int cursor_y() const { return cursor_y_; }
    bool cursor_lit() const { return cursor_lit_; }
    const std::deque<std::string>& actions() const { return actions_; }

private:
    void note(const char* action);
    void draw_cursor(UiCanvas& canvas) const;
    void draw_actions(UiCanvas& canvas) const;

    int cursor_x_ = (kCanvasWidth - kCursorSize) / 2;
    int cursor_y_ = (kCanvasHeight - kCursorSize) / 2;
    bool cursor_lit_ = false;
    bool dirty_ = true;
    std::deque<std::string> actions_;
};

}  // namespace splat
