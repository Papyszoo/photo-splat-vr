// The home screen: the folders (albums) of scenes as large cover tiles, the
// implicit "Unsorted" (the scenes in no folder) first and a "New folder" tile
// last. The filesystem is the only state, so on_enter() re-lists it - also
// when a pop reveals the screen again after converting or renaming.
//
// Cross opens a folder's scenes (ScreenId::Scenes, a = the folder name, "" for
// the loose scenes), or on the last tile asks for a new folder's name.
// Triangle ("More") on a named folder opens its menu: Rename and, for an empty
// folder, Delete, through the keyboard screen (a = "new-album" /
// "rename-album" / "delete-album"). Options (handled by UiApp) leads to
// Settings.
//
// A cover is the folder's first scene thumbnail; a missing one (every console
// scene today) becomes a labelled placeholder tile, never an error.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/thumb_cache.h"
#include "ui/widgets.h"

namespace splat {

// "1 scene" / "N scenes": the folder tile and folder-pick row sublabel. Shared
// by AlbumsScreen and AlbumPickScreen so the two listings cannot drift apart.
std::string scene_count_label(std::size_t count);

class AlbumsScreen : public Screen {
public:
    AlbumsScreen();

    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Scenes"; }
    bool handle_back(UiContext& ctx) override;

    // Tile captions in grid order.
    std::vector<std::string> names() const;
    const GridView& grid() const { return grid_; }
    const PopupMenu& menu() const { return menu_; }

    // The empty state: no scene anywhere under the root yet.
    bool empty_state() const { return empty_state_; }
    static constexpr const char* kEmptyText = "No scenes yet";
    static constexpr const char* kEmptyHint =
        "Open a folder and choose + to turn photos into scenes.";

private:
    struct Item {
        std::string caption;    // the tile's name line
        std::string sublabel;   // "N scenes"
        std::string album;      // album folder name; "" = the loose scenes
        std::string cover;      // folder of the cover scene, may be empty
        std::size_t scenes = 0;
        bool implicit = false;  // the "Unsorted" tile
        bool create = false;    // the "New folder" tile
    };
    enum class Action { Rename, Delete };

    void reload(UiContext& ctx);
    void build_cells();
    void refresh_covers(UiContext& ctx);
    bool open_menu();
    const Item* focused_item() const;
    void run(UiContext& ctx, Action action);

    GridView grid_;
    PopupMenu menu_;
    std::vector<Action> menu_actions_;
    ThumbCache thumbs_;
    std::vector<Item> items_;
    // One owned cover per item, empty until it has loaded (the grid draws the
    // placeholder meanwhile). Owned copies keep the pointers in GridCell
    // stable while the cache evicts.
    std::vector<UiImage> tiles_;
    std::vector<char> cover_ready_;
    bool empty_state_ = false;
    std::string default_name_;
    std::string focus_name_;  // restore the focused folder when the screen is revealed again
};

}  // namespace splat
