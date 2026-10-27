// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/albums.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "gallery/thumbs.h"
#include "ui/screen_stack.h"

namespace splat {
namespace {

// Four cover tiles per row across the 1152 px content, two rows visible.
constexpr int kCellWidth = 264;
constexpr int kCellHeight = 250;  // image + room for a 3-line focused name
constexpr int kCellGap = 32;

}  // namespace

std::string scene_count_label(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " scene" : " scenes");
}

AlbumsScreen::AlbumsScreen() {
    grid_.cell_width = kCellWidth;
    grid_.cell_height = kCellHeight;
    grid_.gap = kCellGap;
    grid_.caption_lines = 3;
}

void AlbumsScreen::on_enter(UiContext& ctx, const ScreenArgs& /*args*/) {
    menu_.close();
    reload(ctx);

    // Coming back from a folder or the keyboard: keep the tile the user left
    // on. A first entry has no name yet and starts at the top.
    for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
        if (!focus_name_.empty() && items_[static_cast<std::size_t>(index)].caption == focus_name_) {
            grid_.set_focus(index);
            break;
        }
    }
}

void AlbumsScreen::reload(UiContext& ctx) {
    const Path root = ctx.svc.paths.scenes;
    items_.clear();

    std::size_t total = 0;
    for (const AlbumEntry& album : list_albums(root)) {
        Item item;
        item.implicit = album.dir == root || album.name == kAllScenesAlbumName;
        item.album = item.implicit ? std::string() : album.name;
        item.caption = album.name;
        item.sublabel = scene_count_label(album.scene_count);
        item.cover = album.cover.string();
        item.scenes = album.scene_count;
        total += album.scene_count;
        items_.push_back(std::move(item));
    }
    empty_state_ = total == 0;
    default_name_ = default_album_name(std::time(nullptr), root);
    Item create;
    create.create = true;
    create.caption = "New folder";
    items_.push_back(std::move(create));

    tiles_.assign(items_.size(), UiImage{});
    cover_ready_.assign(items_.size(), 0);
    build_cells();
    refresh_covers(ctx);
}

void AlbumsScreen::build_cells() {
    std::vector<GridCell> cells;
    cells.reserve(items_.size());
    for (std::size_t index = 0; index < items_.size(); ++index) {
        GridCell cell;
        cell.caption = items_[index].caption;
        cell.sublabel = items_[index].sublabel;
        cell.thumb = &tiles_[index];
        if (items_[index].create) {
            cell.icon = GridCell::Icon::Add;
            cells.push_back(std::move(cell));
            continue;
        }
        // A folder without scenes has nothing to show: a plain folder icon,
        // no stack of pictures behind it.
        cell.stack = items_[index].scenes > 0;
        if (items_[index].scenes == 0) cell.icon = GridCell::Icon::Folder;
        cells.push_back(std::move(cell));
    }
    grid_.set_cells(std::move(cells));
}

void AlbumsScreen::refresh_covers(UiContext& ctx) {
    bool arrived = false;
    for (std::size_t index = 0; index < items_.size(); ++index) {
        if (cover_ready_[index] != 0) continue;
        const std::string& cover = items_[index].cover;
        if (cover.empty()) continue;

        const std::string key = "scene:" + cover;
        const Path folder(cover);
        thumbs_.request(key, [folder] { return load_scene_thumb(folder); });
        const UiImage* image = thumbs_.get(key);
        if (image == nullptr || image->empty()) continue;

        tiles_[index] = *image;  // same vector slot: the cell's pointer stays valid
        cover_ready_[index] = 1;
        arrived = true;
    }
    if (arrived) ctx.invalidate();
}

const AlbumsScreen::Item* AlbumsScreen::focused_item() const {
    const int focus = grid_.focus();
    if (focus < 0 || focus >= static_cast<int>(items_.size())) return nullptr;
    return &items_[static_cast<std::size_t>(focus)];
}

bool AlbumsScreen::open_menu() {
    const Item* item = focused_item();
    // Only a named folder has actions; "Unsorted" and "New folder" have none.
    if (item == nullptr || item->implicit || item->create) return false;
    menu_actions_ = {Action::Rename};
    std::vector<std::string> labels = {"Rename"};
    // Only an empty folder can be removed (the gallery refuses others).
    if (item->scenes == 0) {
        menu_actions_.push_back(Action::Delete);
        labels.push_back("Delete");
    }
    menu_.open(item->caption, std::move(labels));
    return true;
}

void AlbumsScreen::run(UiContext& ctx, Action action) {
    const Item* item = focused_item();
    if (item == nullptr) return;
    ScreenArgs args;
    args.a = action == Action::Rename ? "rename-album" : "delete-album";
    args.b = item->album;
    ctx.nav.push(ScreenId::Keyboard, args);
}

bool AlbumsScreen::handle_back(UiContext& /*ctx*/) {
    if (!menu_.is_open()) return false;
    menu_.close();
    return true;
}

void AlbumsScreen::update(UiContext& ctx, const UiInput& input) {
    if (ctx.content.empty()) return;
    refresh_covers(ctx);

    if (menu_.is_open()) {
        const int chosen = menu_.update(input, ctx.content, ctx.theme);
        if (chosen >= 0 && chosen < static_cast<int>(menu_actions_.size())) {
            run(ctx, menu_actions_[static_cast<std::size_t>(chosen)]);
        }
        return;
    }

    grid_.update(input, ctx.content, ctx.theme);
    const int focus = grid_.focus();
    if (focus < 0 || focus >= static_cast<int>(items_.size())) return;
    focus_name_ = items_[static_cast<std::size_t>(focus)].caption;

    if ((input.pressed & Context) != 0) {
        grid_.clear_activated();
        open_menu();
        return;
    }
    if (!grid_.activated()) return;
    grid_.clear_activated();

    const Item& item = items_[static_cast<std::size_t>(focus)];
    ScreenArgs args;
    if (item.create) {
        args.a = "new-album";
        args.b = default_name_;
        ctx.nav.push(ScreenId::Keyboard, args);
        return;
    }
    args.a = item.album;
    ctx.nav.push(ScreenId::Scenes, args);
}

void AlbumsScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    grid_.draw(canvas, content, ctx.theme);
    if (empty_state_) {
        // Below the tiles: what to do first.
        const int rows = (static_cast<int>(items_.size()) + grid_.columns(content) - 1) /
                         std::max(1, grid_.columns(content));
        const int y = content.y + rows * (kCellHeight + kCellGap) + 8;
        canvas.text(content.x, y, kEmptyText, ctx.theme.heading_size, ctx.theme.text);
        canvas.text(content.x, y + UiCanvas::line_height(ctx.theme.heading_size) + 2, kEmptyHint,
                    ctx.theme.body_size - 2, ctx.theme.dim_text);
    }
    menu_.draw(canvas, content, ctx.theme);
}

std::vector<Hint> AlbumsScreen::hints() const {
    if (menu_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Close"}};
    const Item* item = focused_item();
    if (item != nullptr && item->create) {
        return {Hint{"Cross", "New folder"}, Hint{"Options", "Settings"}};
    }
    if (item == nullptr || item->implicit) return {Hint{"Cross", "Open"}, Hint{"Options", "Settings"}};
    return {Hint{"Cross", "Open"}, Hint{"Triangle", "More"}, Hint{"Options", "Settings"}};
}

std::vector<std::string> AlbumsScreen::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const Item& item : items_) out.push_back(item.caption);
    return out;
}

}  // namespace splat
