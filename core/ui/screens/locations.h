// The network locations screen (L3-C, stage U1): one row per configured share
// and an "Add location" row, always.
//
// Cross opens the share as a library: ScreenId::Scenes is pushed with
// a = "" and b = "<id>:/<path>", so the folder grid lists the scenes that are
// on the NAS, through the VFS. Square shows a delete card (the share's files are
// not touched; only the configuration is removed). Triangle opens the fields.
// The state column is the last mount result: "ready", "mounting", or
// "offline: <reason>" from the adapter (a NAS that is off says so here).
//
// Every location is listed and editable here with or without a passcode: the
// visible-folders list hides a location's scenes, never its configuration
// (user decision 2026-11-06: network locations are managed in Settings).
//
// The list is rebuilt from the service every frame it changed: the store is the
// state, as with the filesystem on every other screen.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/widgets.h"

namespace splat {

class LocationsScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Network locations"; }
    bool handle_back(UiContext& ctx) override;

    const ListView& list() const { return list_; }
    void set_focus(int row) { list_.set_focus(row); }
    const ConfirmCard& confirm() const { return confirm_; }
    // Row captions, for a test; the last one is always "Add location".
    std::vector<std::string> names() const;
    static constexpr const char* kAddLabel = "Add location";

private:
    void refresh(UiContext& ctx);
    void open_library(UiContext& ctx, const std::string& id);
    void open_edit(UiContext& ctx, const std::string& id);
    void ask_delete(UiContext& ctx);

    ListView list_;
    ConfirmCard confirm_;
    // One id per row; "" for the "Add location" row.
    std::vector<std::string> ids_;
    std::vector<std::string> names_;
    std::string deleting_;
    int last_count_ = -1;
    std::string last_states_;
};

}  // namespace splat
