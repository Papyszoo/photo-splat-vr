// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/album_pick.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "ui/screens/albums.h"
#include "ui/screen_stack.h"

namespace splat {

void AlbumPickScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    // `a` is "move-scene" for now; the argument stays in the contract so a
    // later picker mode (e.g. copy) can reuse the screen.
    scene_folder_ = args.b;
    reload(ctx);
}

void AlbumPickScreen::reload(UiContext& ctx) {
    const Path root = ctx.svc.paths.scenes;
    items_.clear();

    // The folder the scene is in now is not a place to move it to.
    std::string current;
    const bool located = scene_location(root, Path(scene_folder_), current);

    // The implicit loose-scene album travels as the empty folder name, exactly
    // like the albums screen does: move_scene() takes "" for the root.
    for (const AlbumEntry& album : list_albums(root)) {
        Item item;
        const bool implicit = album.dir == root || album.name == kAllScenesAlbumName;
        item.album = implicit ? std::string() : album.name;
        if (located && item.album == current) continue;
        item.label = album.name;
        item.sublabel = scene_count_label(album.scene_count);
        items_.push_back(std::move(item));
    }

    // An action, not a place: a "+" row, its date name only in the keyboard.
    default_name_ = default_album_name(std::time(nullptr), root);
    Item create;
    create.create = true;
    create.label = "New folder";
    items_.push_back(std::move(create));

    std::vector<ListRow> rows;
    rows.reserve(items_.size());
    for (const Item& item : items_) {
        ListRow row{item.label, item.sublabel, "", false};
        row.add = item.create;
        rows.push_back(std::move(row));
    }
    list_.set_rows(std::move(rows));
}

void AlbumPickScreen::update(UiContext& ctx, const UiInput& input) {
    if (ctx.content.empty()) return;

    list_.update(input, list_area(ctx.content), ctx.theme);
    if (!list_.activated()) return;
    list_.clear_activated();

    const int focus = list_.focus();
    if (focus < 0 || focus >= static_cast<int>(items_.size())) return;
    const Item& item = items_[static_cast<std::size_t>(focus)];

    if (item.create) {
        ScreenArgs args;
        args.a = "new-album";
        args.b = default_name_;
        ctx.nav.push(ScreenId::Keyboard, args);
        return;
    }

    const Status status = move_scene(ctx.svc.paths.scenes, Path(scene_folder_), item.album);
    if (status.ok()) {
        ctx.toast("Moved to " + item.label);
    } else {
        ctx.toast(status.message());
    }
    ctx.nav.pop();
}

void AlbumPickScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    list_.draw(canvas, list_area(content), ctx.theme);
}

Rect AlbumPickScreen::list_area(const Rect& content) const { return content; }

std::vector<Hint> AlbumPickScreen::hints() const {
    const int focus = list_.focus();
    const bool create = focus >= 0 && focus < static_cast<int>(items_.size()) &&
                        items_[static_cast<std::size_t>(focus)].create;
    return {Hint{"Cross", create ? "Create" : "Move here"}, Hint{"Circle", "Back"}};
}

std::vector<std::string> AlbumPickScreen::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const Item& item : items_) out.push_back(item.label);
    return out;
}

}  // namespace splat
