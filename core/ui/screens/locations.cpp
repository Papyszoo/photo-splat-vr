// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/locations.h"

#include <utility>

#include "ui/screen_stack.h"
#include "ui/text_fit.h"
#include "vfs/io.h"

namespace splat {
namespace {

std::string protocol_label(const std::string& protocol) {
    return protocol == "nfs" ? std::string("NFS") : std::string("SMB");
}

std::string share_line(const LocationView& location) {
    std::string line = protocol_label(location.protocol) + " \xC2\xB7 " + location.host;
    if (!location.share.empty()) line += "/" + location.share;
    if (location.photo_source) line += " \xC2\xB7 photos";
    return line;
}

}  // namespace

void LocationsScreen::on_enter(UiContext& ctx, const ScreenArgs& /*args*/) {
    confirm_.close();
    last_count_ = -1;
    last_states_.clear();
    refresh(ctx);
}

void LocationsScreen::refresh(UiContext& ctx) {
    if (ctx.svc.locations == nullptr) return;
    // Every configured location is listed and manageable (stage U1). The
    // passcode hides scenes on a location, not the location's configuration:
    // the owner adds, edits and removes shares here, with or without one.
    const std::vector<LocationView> locations = ctx.svc.locations->list();

    std::string states;
    for (const LocationView& location : locations) states += location.id + "=" + location.state + ";";
    if (static_cast<int>(locations.size()) == last_count_ && states == last_states_) return;
    last_count_ = static_cast<int>(locations.size());
    last_states_ = std::move(states);

    // One count line when the list changes: the console check proves that every
    // configured location is listed, without reading a name.
    if (ctx.svc.log) {
        ctx.svc.log("ui: locations: " + std::to_string(locations.size()) + " configured");
    }

    const int focus = list_.focus();
    ids_.clear();
    names_.clear();
    std::vector<ListRow> rows;
    rows.reserve(locations.size() + 1);
    for (const LocationView& location : locations) {
        ListRow row;
        row.label = location.name;
        row.sublabel = share_line(location);
        row.right = location.state;
        rows.push_back(std::move(row));
        ids_.push_back(location.id);
        names_.push_back(location.name);
    }
    ListRow add;
    add.label = kAddLabel;
    add.add = true;
    rows.push_back(std::move(add));
    ids_.push_back(std::string());
    names_.push_back(kAddLabel);

    list_.set_rows(std::move(rows));
    list_.set_focus(focus);
}

void LocationsScreen::open_library(UiContext& ctx, const std::string& id) {
    if (ctx.svc.locations == nullptr) return;
    for (const LocationView& location : ctx.svc.locations->list()) {
        if (location.id != id) continue;
        ScreenArgs args;
        args.a.clear();  // the location root itself is the album
        args.b = vfs::join(location.id, location.path).string();
        ctx.nav.push(ScreenId::Scenes, args);
        return;
    }
    ctx.toast("That location is gone");
}

void LocationsScreen::open_edit(UiContext& ctx, const std::string& id) {
    ScreenArgs args;
    args.a = id;
    ctx.nav.push(ScreenId::LocationEdit, args);
}

void LocationsScreen::ask_delete(UiContext& ctx) {
    const int focus = list_.focus();
    if (focus < 0 || focus >= static_cast<int>(ids_.size())) return;
    if (ids_[static_cast<std::size_t>(focus)].empty()) return;  // the Add row
    deleting_ = ids_[static_cast<std::size_t>(focus)];
    confirm_.open("\u201C" + names_[static_cast<std::size_t>(focus)] + "\u201D?",
                  "Removes the location from this console. The files on the share stay.", "Delete");
}

void LocationsScreen::update(UiContext& ctx, const UiInput& input) {
    if (ctx.svc.locations != nullptr) ctx.svc.locations->tick(input.dt);
    refresh(ctx);

    if (ctx.content.empty()) return;
    const int focus = list_.focus();
    const bool on_location =
        focus >= 0 && focus < static_cast<int>(ids_.size()) &&
        !ids_[static_cast<std::size_t>(focus)].empty();

    if (confirm_.is_open()) {
        switch (confirm_.update(input, ctx.content)) {
            case ConfirmCard::Choice::None: return;
            case ConfirmCard::Choice::Cancel: confirm_.close(); return;
            case ConfirmCard::Choice::Confirm: {
                const Status removed = ctx.svc.locations->remove(deleting_);
                ctx.toast(removed.ok() ? "Removed the location" : removed.message());
                confirm_.close();
                last_count_ = -1;
                refresh(ctx);
                return;
            }
        }
    }

    list_.update(input, ctx.content, ctx.theme);
    if ((input.pressed & Context) != 0) {
        list_.clear_activated();
        if (on_location) open_edit(ctx, ids_[static_cast<std::size_t>(focus)]);
        return;
    }
    if ((input.pressed & Secondary) != 0) {
        list_.clear_activated();
        if (on_location) ask_delete(ctx);
        else ctx.toast("Only a location can be removed");
        return;
    }
    if (!list_.activated()) return;
    list_.clear_activated();

    if (ctx.svc.locations == nullptr) {
        ctx.toast("Network locations are not available in this build");
        return;
    }
    if (focus < 0 || focus >= static_cast<int>(ids_.size())) return;
    const std::string& id = ids_[static_cast<std::size_t>(focus)];
    if (id.empty()) {
        open_edit(ctx, std::string());
    } else {
        open_library(ctx, id);
    }
}

void LocationsScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;
    list_.draw(canvas, content, theme);
    if (ctx.svc.locations == nullptr) {
        canvas.text(content.x + 4, content.y + 40,
                    "Network locations are not available in this build", theme.heading_size,
                    theme.text);
    } else if (ids_.empty() || (ids_.size() == 1 && ids_.front().empty())) {
        canvas.text(content.x + 4, content.y + 40, "No network location yet",
                    theme.heading_size, theme.text);
        canvas.text(content.x + 4, content.y + 40 + UiCanvas::line_height(theme.heading_size),
                    "Add a share to see its scenes and photos here.", theme.body_size - 2,
                    theme.dim_text);
    }
    if (confirm_.is_open()) {
        canvas.fill_rect(0, 0, static_cast<int>(canvas.width()), static_cast<int>(canvas.height()),
                         theme.veil);
        confirm_.draw(canvas, content, theme);
    }
}

std::vector<Hint> LocationsScreen::hints() const {
    if (confirm_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Cancel"}};
    const int focus = list_.focus();
    const bool on_location =
        focus >= 0 && focus < static_cast<int>(ids_.size()) &&
        !ids_[static_cast<std::size_t>(focus)].empty();
    if (!on_location) return {Hint{"Cross", "Add"}, Hint{"Circle", "Back"}};
    return {Hint{"Cross", "Open"}, Hint{"Square", "Delete"}, Hint{"Triangle", "Edit"}};
}

bool LocationsScreen::handle_back(UiContext& /*ctx*/) {
    if (!confirm_.is_open()) return false;
    confirm_.close();
    return true;
}

std::vector<std::string> LocationsScreen::names() const { return names_; }

}  // namespace splat
