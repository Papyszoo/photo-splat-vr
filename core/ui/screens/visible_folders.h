// Settings > Visible folders: one passcode and the allow-list of scene folders
// it protects.
//
// The screen has four stages:
//   intro    no passcode yet: the warning and "Set a passcode"
//   locked   a passcode is set and this visit has not entered it
//   damaged  the state file cannot be understood: nothing is visible until a PC
//            deletes it (fail closed, core/gallery/visibility.cpp)
//   list     unlocked: the tick list and the two passcode actions
//
// Stage U1 (user 2026-11-06): the list covers the scene library only - albums,
// Unsorted, "Other scenes" and the network locations that are splat libraries.
// Photo folders, USB sticks, captures and console folders have no row, and the
// photo picker is not filtered by this list.
//
// The list is open only while this screen is in the stack. Circle locks it and
// pops; Options (UiApp) pops all the way to Settings and destroys the screen.
// The passcode is asked again on every new open: there is no session unlock, no
// "unlocked" flag on disk and no timeout, because a visitor who picks up the
// controller after the owner walks away must not find the list open.
//
// A tick is saved the moment it is made (the state file is the setting) and the
// process-wide policy is replaced, so every listing, playlist, job row and toast
// follows at once; the host is told to close the loaded scene if it is now
// hidden (UiCommand::HideScene).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "common/path.h"
#include "gallery/visibility.h"
#include "ui/screen.h"
#include "ui/widgets.h"

namespace splat {

class VisibleFoldersScreen : public Screen {
public:
    enum class Stage { intro, locked, damaged, list };

    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Visible folders"; }
    bool handle_back(UiContext& ctx) override;

    // For the tests: the stage, the draft the screen edits, the rows (headers
    // included, in draw order) and the key a row toggles.
    Stage stage() const { return stage_; }
    const Visibility& draft() const { return draft_; }
    std::vector<std::string> names() const;
    const ListView& list() const { return list_; }
    const ConfirmCard& confirm() const { return confirm_; }
    const VisibilityKey* key_at(int row) const;
    bool unlocked() const { return unlocked_; }
    // Test/preview hook: put the focus on a row by index (a heading is moved to
    // the nearest real row, exactly as Down would).
    void set_focus(int row) { list_.set_focus(row); }

private:
    enum class Action { None, Set, Enter, ChangePasscode, TurnOff };
    // Which keyboard answer this screen is waiting for. on_enter() consumes it;
    // the callback only stores the text (pushing or popping from there would
    // re-enter the stack while the keyboard is popping itself).
    enum class Pending {
        none,
        set_first,
        set_second,
        unlock,
        change_current,
        change_new,
        change_repeat,
    };

    struct Item {
        VisibilityKey key;
        bool toggle = false;  // a folder row: right text Visible/Hidden
        bool header = false;  // a group heading, never focused
        Action action = Action::None;
        std::string label;
        std::string sublabel;
    };

    void load(UiContext& ctx);
    void rebuild(UiContext& ctx);
    void collect_items(UiContext& ctx);
    void refresh_rows();
    void open_keyboard(UiContext& ctx, const std::string& title, Pending pending);
    void consume_pending(UiContext& ctx);
    void toggle_row(UiContext& ctx, int row);
    void turn_off(UiContext& ctx);
    // Writes the draft's allow-list and installs it. False (and no policy
    // change) when the state file cannot be written.
    bool save_and_publish(UiContext& ctx);
    void log_counts(UiContext& ctx) const;
    std::vector<std::string> headers() const;
    Rect rows_area(const Rect& content, const UiTheme& theme) const;
    const Item* item_at(int row) const;

    Stage stage_ = Stage::intro;
    Visibility draft_;
    bool unlocked_ = false;
    Pending pending_ = Pending::none;
    std::string pending_text_;      // the keyboard's last answer
    std::string first_passcode_;    // Set: the first of the two entries
    std::string current_passcode_;  // Change: the verified current code (memory only)
    std::string new_passcode_;      // Change: the new code until its repeat
    ListView list_;
    ConfirmCard confirm_;
    std::vector<Item> items_;
};

}  // namespace splat
