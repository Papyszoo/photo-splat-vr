// Settings, reached with Options from anywhere in the panel (UiApp): the
// comfort controls (scale pivot, move speed, turn speed), the display test
// toggles (edge softness, blending), the models (download, licence, delete),
// the conversion queue and Quit. Rows whose service is missing are drawn dim
// but still open their screen, which then explains what this build lacks. Quit
// is an ordinary row (the app also closes from the PS5 system menu like any
// game).
//
// The comfort/display values live in the persistent UiState (ui.json, through
// UiServices::state) because the VR loop reads the same object every frame.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/ui_state.h"
#include "ui/widgets.h"

namespace splat {

class SettingsScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Settings"; }

    enum Item {
        ComfortHeader = 0,
        ScaleAround,
        MoveSpeed,
        TurnSpeed,
        DisplayHeader,
        EdgeSoftness,
        Blending,
        SystemHeader,
        Models,
        Conversions,
        Quit,
        ItemCount
    };

    // For tests: the row order and the live right-hand texts. The conversion
    // status is the top bar's chip (UiApp), not repeated in a row.
    const ListView& list() const { return list_; }
    // The values this screen edits (a mirror of UiServices::state after the
    // last change, or the screen's own copy when there is no service state).
    const UiState& values() const { return local_state_; }

private:
    UiState& state(UiContext& ctx);
    void refresh_rows(const UiContext& ctx);
    // Cycles one setting by `direction` (-1/+1) and saves the state.
    void cycle(UiContext& ctx, int item, int direction);
    void save(UiContext& ctx);

    ListView list_;
    // The screen's copy when there is no UiServices::state (unit tests and the
    // UI preview); otherwise it mirrors the service's object so values() stays
    // usable without a context.
    UiState local_state_;
};

}  // namespace splat
