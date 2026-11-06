// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/visible_folders.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

std::string location_label(const LocationView& location) {
    if (!location.name.empty()) return location.name;
    std::string text = location.host;
    if (!location.share.empty()) text += "/" + location.share;
    return text;
}

std::string location_sublabel(const LocationView& location) {
    return (location.protocol == "nfs" ? std::string("NFS") : std::string("SMB")) + " \xC2\xB7 " +
           location.host + (location.share.empty() ? std::string() : "/" + location.share);
}

}  // namespace

std::vector<std::string> VisibleFoldersScreen::headers() const {
    switch (stage_) {
        case Stage::intro:
            return {"No passcode is set.", "Every scene folder is visible.",
                    "Hides scene folders in this app only. FTP or another app can still open "
                    "the files."};
        case Stage::locked:
            return {"The scene folder list is protected."};
        case Stage::damaged:
            return {"The visible-folders file is damaged.", "Every scene folder stays hidden.",
                    "Delete state/visible-folders.json from a PC to recover."};
        case Stage::list:
            // The two short lines the README repeats: what this does and what it
            // does not do. Photos are not part of it any more (stage U1).
            return {"Hides scene folders in this app only.",
                    "It does not encrypt: FTP or another app can still open the files."};
    }
    return {};
}

Rect VisibleFoldersScreen::rows_area(const Rect& content, const UiTheme& theme) const {
    const int lines = static_cast<int>(headers().size());
    const int top = lines * (theme.body_size - 2 + 6) + 18;
    return Rect{content.x, content.y + top, content.w, std::max(0, content.h - top)};
}

void VisibleFoldersScreen::load(UiContext& ctx) {
    draft_ = *current_visibility();
    if (!draft_.has_context()) {
        // A host that installs no policy (a unit test, a preview) still gets the
        // screens' own paths, so the list is the real folder set.
        draft_.context.scenes_root = ctx.svc.paths.scenes;
        draft_.context.inbox = ctx.svc.paths.inbox;
        draft_.context.mnt = ctx.svc.paths.mnt;
        draft_.context.data = ctx.svc.paths.data;
        draft_.context.app_data = ctx.svc.paths.inbox.parent_path();
    }
    unlocked_ = false;
    stage_ = draft_.damaged ? Stage::damaged : (draft_.enabled ? Stage::locked : Stage::intro);
}

void VisibleFoldersScreen::collect_items(UiContext& ctx) {
    items_.clear();
    if (stage_ != Stage::list) {
        // One action row (Set / Enter) keeps Cross, the pointer and the focus
        // working without a second widget; the headers above it are text.
        Item action;
        action.action = stage_ == Stage::intro ? Action::Set
                                               : (stage_ == Stage::locked ? Action::Enter
                                                                          : Action::None);
        if (action.action != Action::None) {
            action.label = stage_ == Stage::intro ? "Set a passcode" : "Enter passcode";
            items_.push_back(std::move(action));
        }
        return;
    }

    const Path root = draft_.context.scenes_root.empty() ? ctx.svc.paths.scenes
                                                        : draft_.context.scenes_root;
    // "Scene folders": Unsorted first, then every album. list_albums_all()
    // ignores the policy on purpose - the owner must be able to tick a hidden
    // album.
    Item folders;
    folders.header = true;
    folders.label = "Scene folders";
    items_.push_back(std::move(folders));
    Item unsorted;
    unsorted.toggle = true;
    unsorted.key = VisibilityKey::unsorted();
    unsorted.label = kAllScenesAlbumName;
    items_.push_back(std::move(unsorted));
    for (const AlbumEntry& album : list_albums_all(root)) {
        if (album.dir == root || album.name == kAllScenesAlbumName) continue;
        Item row;
        row.toggle = true;
        row.key = VisibilityKey::album(album.name);
        row.label = album.name;
        items_.push_back(std::move(row));
    }

    // "Network": one row per location that is a splat library. A photos-only
    // location has no row: photos are not hidden by this list any more (stage
    // U1), and management lives in Settings.
    if (ctx.svc.locations != nullptr) {
        std::vector<Item> rows;
        for (const LocationView& location : ctx.svc.locations->list()) {
            if (!location.splat_library) continue;
            Item row;
            row.toggle = true;
            row.key = VisibilityKey::location(location.id);
            row.label = location_label(location);
            row.sublabel = location_sublabel(location);
            rows.push_back(std::move(row));
        }
        if (!rows.empty()) {
            Item network;
            network.header = true;
            network.label = "Network";
            items_.push_back(std::move(network));
            for (Item& row : rows) items_.push_back(std::move(row));
        }
    }

    // "Other": the scenes with no trustworthy folder. Always present.
    Item other_header;
    other_header.header = true;
    other_header.label = "Other";
    items_.push_back(std::move(other_header));
    Item other;
    other.toggle = true;
    other.key = VisibilityKey::other();
    other.label = "Other scenes";
    items_.push_back(std::move(other));

    // The two actions. Adding, editing and removing a network location is in
    // Settings, not behind this list.
    Item change;
    change.action = Action::ChangePasscode;
    change.label = "Change passcode";
    items_.push_back(std::move(change));
    Item off;
    off.action = Action::TurnOff;
    off.label = "Turn off passcode";
    items_.push_back(std::move(off));
}

void VisibleFoldersScreen::refresh_rows() {
    std::vector<ListRow> rows;
    rows.reserve(items_.size());
    for (const Item& item : items_) {
        ListRow row;
        row.label = item.label;
        row.sublabel = item.sublabel;
        row.header = item.header;
        if (item.toggle) row.right = draft_.allows(item.key) ? "Visible" : "Hidden";
        row.add = item.action == Action::Set || item.action == Action::Enter;
        rows.push_back(std::move(row));
    }
    const int focus = list_.focus();
    list_.set_rows(std::move(rows));
    list_.set_focus(focus);
}

void VisibleFoldersScreen::rebuild(UiContext& ctx) {
    collect_items(ctx);
    refresh_rows();
}

void VisibleFoldersScreen::on_enter(UiContext& ctx, const ScreenArgs& /*args*/) {
    confirm_.close();
    if (pending_ != Pending::none) {
        // The keyboard popped and revealed this screen: act on its answer once.
        consume_pending(ctx);
        return;
    }
    if (unlocked_) {
        // Already open in this visit: re-list from the current folders and the
        // draft. A new open is always a new screen object (load() asks for the
        // passcode), so this only serves an explicit re-list - the test that
        // adds a folder while the list is open uses it.
        stage_ = Stage::list;
        rebuild(ctx);
        return;
    }
    // A new open, and the only way in: ask the passcode again.
    load(ctx);
    rebuild(ctx);
}

void VisibleFoldersScreen::open_keyboard(UiContext& ctx, const std::string& title,
                                         Pending pending) {
    ScreenArgs args;
    args.a = "free-text";
    // The passcode is never pre-filled, never shown: one '*' per character.
    args.b = std::string();
    args.masked = true;
    args.max_length = static_cast<int>(kPasscodeMaxLength);
    args.min_length = static_cast<int>(kPasscodeMinLength);
    args.title = title;
    pending_ = pending;
    pending_text_.clear();
    args.on_text = [this](const std::string& text) { pending_text_ = text; };
    ctx.nav.push(ScreenId::Keyboard, args);
}

void VisibleFoldersScreen::log_counts(UiContext& ctx) const {
    if (!ctx.svc.log) return;
    // The numerator is the ticked rows, not allowed.size(): an older state file
    // still holds the inert source:/folder: keys of stage H, and a deleted
    // album or location keeps its key too, so counting the vector reported more
    // ticks than the list can show ("2 of 4 ticked" for one tick).
    std::size_t known = 0;
    std::size_t ticked = 0;
    for (const Item& item : items_) {
        if (!item.toggle) continue;
        ++known;
        if (draft_.allows(item.key)) ++ticked;
    }
    char line[96];
    std::snprintf(line, sizeof line, "ui: visible folders: passcode on; %zu of %zu ticked", ticked,
                  known);
    ctx.svc.log(line);
}

bool VisibleFoldersScreen::save_and_publish(UiContext& ctx) {
    std::sort(draft_.allowed.begin(), draft_.allowed.end());
    const Status saved = draft_.save_allowed(ctx.svc.paths.state);
    if (!saved.ok()) {
        ctx.toast("Could not save the folder list");
        if (ctx.svc.log) ctx.svc.log("ui: visible folders: " + saved.message());
        return false;
    }
    set_visibility(draft_);
    log_counts(ctx);
    // The loaded scene may be hidden now: the host closes it (main.cpp,
    // vr_mode.cpp) instead of leaving it on screen behind the panel.
    ctx.command(UiCommand{UiCommand::HideScene, std::string(), 0});
    return true;
}

void VisibleFoldersScreen::consume_pending(UiContext& ctx) {
    const Pending kind = pending_;
    pending_ = Pending::none;
    const std::string text = pending_text_;
    pending_text_.clear();

    if (text.empty()) {
        // Circle in the keyboard: back to the stage we came from. Never open
        // the keyboard again from here.
        if (kind == Pending::set_first || kind == Pending::set_second) {
            first_passcode_.clear();
            stage_ = Stage::intro;
            unlocked_ = false;
        } else if (kind == Pending::unlock) {
            // A cancelled "Enter passcode" is exactly like a wrong one: the
            // list stays closed. Falling through to the list here would hand
            // the visitor an editable, savable allow-list without the code.
            stage_ = Stage::locked;
            unlocked_ = false;
        } else {
            // Change is only reachable from the open list, by the owner who
            // typed the current passcode a moment ago (or cancelled that
            // entry): stay there and keep the old code.
            current_passcode_.clear();
            new_passcode_.clear();
            stage_ = Stage::list;
            unlocked_ = true;
        }
        rebuild(ctx);
        return;
    }

    switch (kind) {
        case Pending::set_first:
            first_passcode_ = text;
            open_keyboard(ctx, "Repeat the passcode", Pending::set_second);
            return;
        case Pending::set_second: {
            const std::string first = first_passcode_;
            first_passcode_.clear();
            if (text.size() < kPasscodeMinLength) {
                ctx.toast("Passcode must be at least 4 characters");
                stage_ = Stage::intro;
                rebuild(ctx);
                return;
            }
            if (text != first) {
                ctx.toast("The passcodes do not match");
                stage_ = Stage::intro;
                rebuild(ctx);
                return;
            }
            // enable() writes the file with an empty allow-list: fail closed
            // until the owner ticks what may be seen.
            const Status enabled = draft_.enable(ctx.svc.paths.state, text);
            if (!enabled.ok()) {
                ctx.toast(enabled.message());
                stage_ = Stage::intro;
                rebuild(ctx);
                return;
            }
            set_visibility(draft_);
            unlocked_ = true;
            stage_ = Stage::list;
            if (ctx.svc.log) ctx.svc.log("ui: visible folders: passcode set");
            ctx.toast("Passcode set. Tick the folders that may be seen.");
            rebuild(ctx);
            log_counts(ctx);
            ctx.command(UiCommand{UiCommand::HideScene, std::string(), 0});
            return;
        }
        case Pending::unlock: {
            if (!draft_.check_passcode(text)) {
                // No retry limit and no delay beyond the KDF (user decision).
                ctx.toast("Wrong passcode");
                if (ctx.svc.log) ctx.svc.log("ui: visible folders: wrong passcode");
                stage_ = Stage::locked;
                rebuild(ctx);
                return;
            }
            unlocked_ = true;
            stage_ = Stage::list;
            rebuild(ctx);
            log_counts(ctx);
            return;
        }
        case Pending::change_current: {
            if (!draft_.check_passcode(text)) {
                ctx.toast("Wrong passcode");
                if (ctx.svc.log) ctx.svc.log("ui: visible folders: wrong passcode");
                // A visitor who finds the list open must not be able to replace
                // the code: stay on the list, change nothing.
                stage_ = Stage::list;
                unlocked_ = true;
                rebuild(ctx);
                return;
            }
            current_passcode_ = text;
            open_keyboard(ctx, "New passcode", Pending::change_new);
            return;
        }
        case Pending::change_new:
            new_passcode_ = text;
            open_keyboard(ctx, "Repeat the new passcode", Pending::change_repeat);
            return;
        case Pending::change_repeat: {
            const std::string next = text;
            const std::string current = current_passcode_;
            const bool matched = next == new_passcode_;
            current_passcode_.clear();
            new_passcode_.clear();
            stage_ = Stage::list;
            unlocked_ = true;
            if (!matched) {
                ctx.toast("The passcodes do not match");
                rebuild(ctx);
                return;
            }
            // A fresh salt and hash; the allow-list is untouched.
            const Status changed = draft_.change_passcode(ctx.svc.paths.state, current, next);
            if (!changed.ok()) {
                ctx.toast(changed.message());
                rebuild(ctx);
                return;
            }
            set_visibility(draft_);
            if (ctx.svc.log) ctx.svc.log("ui: visible folders: passcode changed");
            ctx.toast("Passcode changed");
            rebuild(ctx);
            log_counts(ctx);
            return;
        }
        case Pending::none:
            return;
    }
}

void VisibleFoldersScreen::toggle_row(UiContext& ctx, int row) {
    const Item* item = item_at(row);
    if (item == nullptr || !item->toggle) return;
    const VisibilityKey key = item->key;
    const std::vector<VisibilityKey> previous = draft_.allowed;
    const auto found = std::find(draft_.allowed.begin(), draft_.allowed.end(), key);
    if (found == draft_.allowed.end()) {
        draft_.allowed.push_back(key);
    } else {
        draft_.allowed.erase(found);
    }
    if (!save_and_publish(ctx)) {
        // The file and the policy still hold the untoggled list: the row must
        // say so too, not claim a tick that a relaunch would lose.
        draft_.allowed = previous;
    }
    // Rebuild in place, keeping the row the owner just ticked under the focus.
    collect_items(ctx);
    refresh_rows();
    list_.set_focus(row);
}

void VisibleFoldersScreen::turn_off(UiContext& ctx) {
    const Status removed = draft_.disable(ctx.svc.paths.state);
    if (!removed.ok()) {
        ctx.toast(removed.message());
        return;
    }
    // Every folder visible again, installed at once so the gallery behind the
    // panel is complete before the screen closes.
    set_visibility(draft_);
    unlocked_ = false;
    stage_ = Stage::intro;
    if (ctx.svc.log) ctx.svc.log("ui: visible folders: passcode off");
    ctx.toast("Passcode off");
    rebuild(ctx);
    ctx.command(UiCommand{UiCommand::HideScene, std::string(), 0});
}

const VisibleFoldersScreen::Item* VisibleFoldersScreen::item_at(int row) const {
    if (row < 0 || row >= static_cast<int>(items_.size())) return nullptr;
    return &items_[static_cast<std::size_t>(row)];
}

const VisibilityKey* VisibleFoldersScreen::key_at(int row) const {
    const Item* item = item_at(row);
    return item != nullptr && item->toggle ? &item->key : nullptr;
}

void VisibleFoldersScreen::update(UiContext& ctx, const UiInput& input) {
    if (ctx.content.empty()) return;
    const Rect area = rows_area(ctx.content, ctx.theme);

    if (confirm_.is_open()) {
        switch (confirm_.update(input, ctx.content)) {
            case ConfirmCard::Choice::None: return;
            case ConfirmCard::Choice::Cancel: confirm_.close(); return;
            case ConfirmCard::Choice::Confirm: confirm_.close(); turn_off(ctx); return;
        }
    }

    list_.update(input, area, ctx.theme);
    if (!list_.activated()) return;
    list_.clear_activated();

    const Item* item = item_at(list_.focus());
    if (item == nullptr) return;
    if (item->toggle) {
        toggle_row(ctx, list_.focus());
        return;
    }
    switch (item->action) {
        case Action::Set: open_keyboard(ctx, "Set a passcode", Pending::set_first); return;
        case Action::Enter: open_keyboard(ctx, "Enter passcode", Pending::unlock); return;
        case Action::ChangePasscode:
            open_keyboard(ctx, "Current passcode", Pending::change_current);
            return;
        case Action::TurnOff:
            // Cancel has the focus first, as on the console.
            confirm_.open("Turn off the passcode?", "Every scene folder becomes visible again.",
                          "Turn off");
            return;
        case Action::None: return;
    }
}

void VisibleFoldersScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;
    int y = content.y;
    const int size = theme.body_size - 2;
    for (const std::string& line : headers()) {
        canvas.text(content.x, y, fit_text(line, size, content.w - 8), size, theme.dim_text);
        y += size + 6;
    }
    const Rect area = rows_area(content, theme);
    if (items_.empty()) return;
    list_.draw(canvas, area, theme);
    if (confirm_.is_open()) {
        canvas.fill_rect(0, 0, static_cast<int>(canvas.width()), static_cast<int>(canvas.height()),
                         theme.veil);
        confirm_.draw(canvas, content, theme);
    }
}

std::vector<Hint> VisibleFoldersScreen::hints() const {
    if (confirm_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Cancel"}};
    if (stage_ == Stage::damaged) return {Hint{"Circle", "Back"}};
    if (stage_ == Stage::intro) return {Hint{"Cross", "Set a passcode"}, Hint{"Circle", "Back"}};
    if (stage_ == Stage::locked) return {Hint{"Cross", "Enter passcode"}, Hint{"Circle", "Back"}};
    const Item* item = item_at(list_.focus());
    const std::string open = item != nullptr && item->toggle ? std::string("Show or hide")
                                                            : std::string("Open");
    return {Hint{"Cross", open}, Hint{"Circle", "Back"}};
}

bool VisibleFoldersScreen::handle_back(UiContext& /*ctx*/) {
    if (confirm_.is_open()) {
        confirm_.close();
        return true;
    }
    if (!unlocked_) return false;
    // Leaving the list locks it, and UiApp pops to Settings from here. The
    // screen object may then be destroyed; nothing unlocked survives the visit.
    unlocked_ = false;
    stage_ = Stage::locked;
    return false;
}

std::vector<std::string> VisibleFoldersScreen::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const Item& item : items_) out.push_back(item.label);
    return out;
}

}  // namespace splat
