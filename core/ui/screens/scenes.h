// One folder of scenes: a "+" tile first (add photos), then a tile per
// conversion still queued or running for this folder (the photo with a
// progress ring), then the scenes - thumbnail or labelled placeholder, the
// name and model under it (the focused one shows a long name on two lines).
// The title is the folder's name. Focus starts on the first scene.
//
// Cross on a scene hands it to the host (UiCommand{ViewScene, album, index},
// index in list_scenes order, so it matches the album playlist). Cross on "+"
// opens the photo picker for this folder (ScreenId::Photos, a = the folder).
// Triangle ("More") on a scene opens its menu: "Move to folder" asks for the
// folder picker (ScreenId::AlbumPick, a = "move-scene", b = the scene folder),
// "Rename" the keyboard (a = "rename-scene"), "Delete" a Cancel/Delete card
// that removes only that scene's folder (gallery delete_scene()).
//
// The folder comes in ScreenArgs::a: "" is the loose-scene album, which the
// title shows as "Unsorted". The screen re-lists on every reveal (on_enter
// runs again when a pop reveals it), and again whenever a conversion for this
// folder starts, finishes or fails; it keeps the focus on the same tile.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/path.h"
#include "scene/scene_folder.h"
#include "ui/screen.h"
#include "ui/services.h"
#include "ui/thumb_cache.h"
#include "ui/widgets.h"

namespace splat {

class ScenesScreen : public Screen {
public:
    ScenesScreen();

    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override;
    bool handle_back(UiContext& ctx) override;

    // For tests and the preview: scene names in listing order, the album the
    // screen was entered with ("" = the loose scenes), the pending conversions
    // shown, and the focused scene's index (-1 on "+" or a pending tile).
    std::vector<std::string> names() const;
    const GridView& grid() const { return grid_; }
    const PopupMenu& menu() const { return menu_; }
    const ConfirmCard& confirm() const { return confirm_; }
    const std::string& album() const { return album_; }
    std::size_t pending() const { return pending_.size(); }
    int focused_scene() const;
    // Grid index of the first scene tile ("+" and the pending tiles come first).
    int first_scene_cell() const { return 1 + static_cast<int>(pending_.size()); }

private:
    struct Item {
        std::string name;      // folder name, the tile's caption
        std::string sublabel;  // the model's display name
        Path folder;
        bool placeholder = false;  // no thumb.jpg: draw the labelled tile
    };

    void reload(UiContext& ctx);
    void build_cells();
    void refresh_thumbs(UiContext& ctx);
    // Re-reads the job snapshot: true when the pending set or the finished
    // count changed (the folder must be re-listed); progress is updated in place.
    bool sync_jobs(UiContext& ctx);
    std::string focus_key() const;
    void restore_focus(const std::string& key);

    GridView grid_;
    PopupMenu menu_;
    ConfirmCard confirm_;
    ThumbCache thumbs_;
    std::string album_;
    std::vector<Item> items_;
    std::vector<JobView> pending_;  // queued/running jobs into this folder
    std::size_t finished_ = 0;      // jobs into this folder that ended (done/failed/cancelled)
    // One owned image per tile ("+" has none): the thumbnail once it has
    // loaded, empty before that (the grid draws the placeholder).
    std::vector<UiImage> tiles_;
    std::vector<char> thumb_ready_;
    std::string focus_key_;  // "scene:<folder>", "job:<id>" or "add"
};

}  // namespace splat
