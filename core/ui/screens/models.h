// The Models screen (Settings > Models): the pinned checkpoints with a plain
// description, state and size, a live progress bar for the one running
// download, and a last "Default model" row (the model the model choice offers
// first). No model is preselected.
//
// Cross acts on the row: a gated model that needs acceptance opens the Licence
// screen, a failed or accepted row (re)starts the download, the Default model
// row switches to the next Ready model. Square asks to delete a model (a
// Cancel / Delete card, Cancel focused), or cancels the running download.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/services.h"
#include "ui/widgets.h"

namespace splat {

class ModelsScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Models"; }
    bool handle_back(UiContext& ctx) override;

    // Read-only accessors for the tests.
    const ListView& list() const { return list_; }
    const std::vector<ModelView>& models() const { return models_; }
    // The default model's display name ("" = none), shown in the last row.
    const std::string& default_name() const { return default_name_; }
    bool on_default_row() const {
        return list_.focus() == static_cast<int>(models_.size());
    }
    // The Cancel / Delete card Square opens on a model.
    const ConfirmCard& confirm() const { return confirm_; }

private:
    void refresh(UiContext& ctx);
    void sync_rows(UiContext& ctx);
    void select_action(UiContext& ctx);
    void cycle_default(UiContext& ctx);
    void secondary_action(UiContext& ctx);

    const ModelView* focused() const;
    int active_index() const;
    static bool is_active(const ModelView& view);

    std::vector<ModelView> models_;
    ListView list_;
    std::string default_name_;
    std::string status_key_;
    ConfirmCard confirm_;
    std::string confirm_id_;  // the model the card would delete
};

}  // namespace splat
