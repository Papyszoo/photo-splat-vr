// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/albums.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "gallery/thumbs.h"
#include "gallery/visibility.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"
#include "vfs/io.h"

namespace splat {

std::string scene_count_label(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " scene" : " scenes");
}

AlbumsScreen::AlbumsScreen() {
    // The tile size is the shared thumbnail layout (core/ui/widgets.h, stage T),
    // applied from update() for the current setting and area. The caption rule
    // (two lines, focused or not) is the shared one too; this screen only adds
    // the scene count as each tile's sublabel.
}

void AlbumsScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    menu_.close();
    // The app's own library, or the network library this screen was opened for
    // (ScreenArgs::b, from the Locations screen).
    root_ = args.b.empty() ? ctx.svc.paths.scenes : Path(args.b);
    // A hidden network library is a blocked screen: nothing is listed, no share
    // is contacted, and no folder name is drawn (the same rule as the shelf).
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    blocked_ = vfs::is_remote(root_) && visibility->enabled &&
               !visibility->allows_location(vfs::location_id(root_));
    if (blocked_) {
        listing_.cancel();
        items_.clear();
        error_.clear();
        empty_state_ = false;
        build_cells();
        return;
    }
    request_listing(ctx);
    // A local root is already listed at this point; a location path arrives
    // through update()/tick().
    if (listing_.ready()) apply_listing(ctx);
}

void AlbumsScreen::request_listing(UiContext& /*ctx*/) {
    const Path root = root_;
    vfs::AsyncListing::Job job = [root] {
        auto listing = std::make_shared<Listing>();
        const bool remote = vfs::is_remote(root);
        std::vector<AlbumEntry> albums;
        if (remote) {
            // A share that is offline or has the wrong path says so; "No scenes
            // yet" is reserved for a library that really is empty.
            Result<std::vector<AlbumEntry>> listed = list_albums_checked(root);
            if (!listed.ok()) {
                return Result<vfs::AsyncListing::Snapshot>::failure(listed.message());
            }
            albums = std::move(listed.value());
        } else {
            albums = list_albums(root);
        }
        std::size_t total = 0;
        for (const AlbumEntry& album : albums) {
            // On a share the folders that hold scenes are the albums: the
            // console cannot make an empty folder there, and a share is full of
            // folders that are not scene libraries. "Unsorted" (the scenes
            // directly in the library folder) stays, as on the console.
            const bool implicit = album.dir == root || album.name == kAllScenesAlbumName;
            if (remote && !implicit && album.scene_count == 0) continue;
            Item item;
            item.implicit = album.dir == root || album.name == kAllScenesAlbumName;
            item.album = item.implicit ? std::string() : album.name;
            item.caption = album.name;
            item.sublabel = scene_count_label(album.scene_count);
            item.cover = album.cover.string();
            item.scenes = album.scene_count;
            total += album.scene_count;
            listing->items.push_back(std::move(item));
        }
        listing->empty_state = total == 0;
        listing->scenes_total = total;
        if (!remote) {
            // Folders on a share cannot be made from the console, so only the
            // console's own library offers "New folder".
            listing->default_name = default_album_name(std::time(nullptr), root);
            Item create;
            create.create = true;
            create.caption = "New folder";
            listing->items.push_back(std::move(create));
        }

        vfs::AsyncListing::Snapshot snapshot;
        snapshot.payload = std::move(listing);
        return Result<vfs::AsyncListing::Snapshot>::success(std::move(snapshot));
    };
    listing_.request(root, std::move(job));
}

void AlbumsScreen::apply_listing(UiContext& ctx) {
    const std::shared_ptr<Listing> listed = std::static_pointer_cast<Listing>(listing_.payload());
    error_ = listed ? std::string() : listing_.error();
    if (!listed) {
        if (!error_.empty() && ctx.svc.log) ctx.svc.log("ui: albums: " + error_);
        items_.clear();
        empty_state_ = true;
        build_cells();
        listing_.cancel();
        return;
    }
    items_ = std::move(listed->items);
    empty_state_ = listed->empty_state;
    default_name_ = std::move(listed->default_name);
    if (ctx.svc.log && current_visibility()->enabled) {
        // Counts only, the folders excluded by the policy among them: the
        // console test reads "ui: albums: N visible," instead of a screenshot.
        // Logged only while a passcode is set, so a run without one keeps the
        // old log exactly.
        std::size_t folders = 0;
        for (const Item& item : items_) folders += item.create ? 0 : 1;
        ctx.svc.log("ui: albums: " + std::to_string(folders) + " visible, " +
                    std::to_string(listed->scenes_total) + " scenes");
    }
    listing_.cancel();  // consumed: ready() must not rebuild the grid again

    tiles_.assign(items_.size(), UiImage{});
    cover_ready_.assign(items_.size(), 0);
    build_cells();
    refresh_covers(ctx);

    // Coming back from a folder or the keyboard: keep the tile the user left
    // on. A first entry has no name yet and starts at the top.
    for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
        if (!focus_name_.empty() && items_[static_cast<std::size_t>(index)].caption == focus_name_) {
            grid_.set_focus(index);
            break;
        }
    }
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
    listing_.tick();
    if (listing_.ready()) {
        apply_listing(ctx);
        ctx.invalidate();
    }
    if (ctx.content.empty() || listing_.busy() || blocked_) return;
    refresh_covers(ctx);

    if (menu_.is_open()) {
        const int chosen = menu_.update(input, ctx.content, ctx.theme);
        if (chosen >= 0 && chosen < static_cast<int>(menu_actions_.size())) {
            run(ctx, menu_actions_[static_cast<std::size_t>(chosen)]);
        }
        return;
    }

    apply_thumbnail_layout(grid_, ctx.content, ctx.theme, ctx.thumbnail_size());
    grid_.update(input, ctx.content, ctx.theme);
    const int focus = grid_.focus();
    if (focus < 0 || focus >= static_cast<int>(items_.size())) return;
    focus_name_ = items_[static_cast<std::size_t>(focus)].caption;

    if ((input.pressed & Context) != 0) {
        grid_.clear_activated();
        // Rename and delete are folder mutations; a share answers them with the
        // one refusal the shelf gives, so say it instead of opening a menu.
        if (vfs::is_remote(root_)) {
            ctx.toast(kRemoteMutationRefusal);
            return;
        }
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
    // A network library keeps its root for the shelf; the console's own is the
    // shelf's default.
    if (vfs::is_remote(root_)) args.b = root_.string();
    ctx.nav.push(ScreenId::Scenes, args);
}

void AlbumsScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    if (blocked_) {
        canvas.text(content.x + 4, content.y + 40, kNotVisibleText, ctx.theme.heading_size,
                    ctx.theme.text);
        return;
    }
    if (listing_.busy()) {
        canvas.text(content.x + 4, content.y + 40, "Loading\xE2\x80\xA6", ctx.theme.heading_size,
                    ctx.theme.text);
        return;
    }
    if (!error_.empty()) {
        // A failed listing (the share is offline, the location was removed)
        // says why: "No scenes yet" would be a lie.
        const int size = ctx.theme.heading_size;
        canvas.text(content.x, content.y + 40, fit_text(error_, size, content.w), size,
                    ctx.theme.text);
        return;
    }
    apply_thumbnail_layout(grid_, content, ctx.theme, ctx.thumbnail_size());
    grid_.draw(canvas, content, ctx.theme);
    if (empty_state_) {
        // Below the tiles: what to do first. With a passcode set the reason is
        // usually the allow-list, so the hint says where it lives (it never
        // names a hidden folder).
        const bool restricted = current_visibility()->enabled;
        const int rows = (static_cast<int>(items_.size()) + grid_.columns(content) - 1) /
                         std::max(1, grid_.columns(content));
        const int y = content.y + rows * (grid_.cell_height + grid_.gap) + 8;
        canvas.text(content.x, y, restricted ? "No visible folders" : kEmptyText,
                    ctx.theme.heading_size, ctx.theme.text);
        canvas.text(content.x, y + UiCanvas::line_height(ctx.theme.heading_size) + 2,
                    restricted ? "Tick the folders in Settings > Visible folders." : kEmptyHint,
                    ctx.theme.body_size - 2, ctx.theme.dim_text);
    }
    menu_.draw(canvas, content, ctx.theme);
}

std::vector<Hint> AlbumsScreen::hints() const {
    if (blocked_) return {Hint{"Circle", "Back"}};
    if (menu_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Close"}};
    const Item* item = focused_item();
    if (item != nullptr && item->create) {
        return {Hint{"Cross", "New folder"}, Hint{"Options", "Settings"}};
    }
    // A share's folders cannot be changed from the console: no "More" there.
    if (item == nullptr || item->implicit || vfs::is_remote(root_)) {
        return {Hint{"Cross", "Open"}, Hint{"Options", "Settings"}};
    }
    return {Hint{"Cross", "Open"}, Hint{"Triangle", "More"}, Hint{"Options", "Settings"}};
}

std::vector<std::string> AlbumsScreen::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const Item& item : items_) out.push_back(item.caption);
    return out;
}

}  // namespace splat
