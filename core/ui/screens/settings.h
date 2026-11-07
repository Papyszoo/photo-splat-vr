// Settings, reached with Options from anywhere in the panel (UiApp): the
// comfort controls (scale pivot, move speed, turn speed), the display test
// toggles (edge softness, blending), the models (download, licence, delete),
// the conversion queue, the Output folder (where new scenes are saved: the
// console, or a folder on a network location picked by browsing it) and Quit.
// Rows whose service is missing are drawn dim
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
        ThumbnailSize,
        SystemHeader,
        Models,
        Conversions,
        OutputFolder,
        Locations,
        VisibleFolders,
        Quit,
        ItemCount
    };

    // For tests: the row order and the live right-hand texts. The conversion
    // status is the top bar's chip (UiApp), not repeated in a row.
    const ListView& list() const { return list_; }
    // The thumbnail size slider's state and its row text ("Default · 7 per
    // row"): both are what the row draws and what a pointer drag changes.
    const Slider& thumbnail_slider() const { return slider_; }
    std::string thumbnail_text(const UiContext& ctx) const;
    // The width of the widest step's value text: the slider's track and hit box
    // are laid out from it, so neither moves when the step's label changes width.
    int thumbnail_text_width(const UiContext& ctx) const;
    // The right-hand part of a list row the slider is drawn in (the same box
    // the pointer hits).
    Rect slider_area(const Rect& row, const UiTheme& theme) const;
    // The values this screen edits (a mirror of UiServices::state after the
    // last change, or the screen's own copy when there is no service state).
    const UiState& values() const { return local_state_; }

private:
    UiState& state(UiContext& ctx);
    void refresh_rows(const UiContext& ctx);
    // The per-row part of the slider's text: one number, or "low-high" when the
    // folder grid and the photo picker draw different column counts at a step.
    std::string thumbnail_columns_text(const UiContext& ctx, int size) const;
    // The pointer's and the pad's one setting: the thumbnail size step.
    void set_thumbnail_size(UiContext& ctx, int size);
    // Cycles one setting by `direction` (-1/+1) and saves the state.
    void cycle(UiContext& ctx, int item, int direction);
    void save(UiContext& ctx);

    ListView list_;
    Slider slider_;
    // The screen's copy when there is no UiServices::state (unit tests and the
    // UI preview); otherwise it mirrors the service's object so values() stays
    // usable without a context.
    UiState local_state_;
};

}  // namespace splat
