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
// that removes only that scene's folder (gallery delete_scene()). L3/R3 on a
// scene row turn it left/right by 90 degrees as a file action: a background job
// rewrites the scene's own files, the tile says "Rotating" while it runs, and a
// scene that is open or already being turned is refused with a toast. This is
// the same two keys the photo picker uses (DECISIONS.md, 2026-11-05).
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
#include "gallery/gallery.h"
#include "scene/scene_folder.h"
#include "ui/screen.h"
#include "ui/services.h"
#include "ui/thumb_cache.h"
#include "ui/widgets.h"
#include "vfs/listing.h"

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
    // The library root the scenes were listed from: the app's scene folder, or
    // the location path a network library was opened with (ScreenArgs::b).
    const Path& root() const { return root_; }
    // The listing's failure line ("smb0: ..."), empty on a good listing. It is
    // drawn instead of the grid, so a test can assert the user sees it.
    const std::string& error() const { return error_; }
    // True when the folder this screen was opened for is not allowed by the
    // visible-folders policy: nothing is listed and the title is generic.
    bool blocked() const { return blocked_; }
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

    // What the worker publishes: the scene folders of this library root.
    struct SceneListing {
        std::vector<SceneEntry> scenes;
    };

    void request_scenes();
    void apply_scenes(UiContext& ctx);
    void reload(UiContext& ctx);
    void build_cells();
    void refresh_thumbs(UiContext& ctx);
    // Re-reads the job snapshot: true when the pending set or the finished
    // count changed (the folder must be re-listed); progress is updated in place.
    bool sync_jobs(UiContext& ctx);
    // Drops a finished turn's cached thumbnail and toasts its result once per
    // session, whether the shelf watched the job run or came back afterwards.
    void announce_finished_turn(UiContext& ctx, const JobView& job);
    // L3/R3: queue a file turn of one scene. Refused while it is open or
    // already being turned.
    void rotate_scene(UiContext& ctx, int scene, int quarters);
    bool is_rotating(const Path& folder) const;
    std::string focus_key() const;
    void restore_focus(const std::string& key);

    GridView grid_;
    PopupMenu menu_;
    ConfirmCard confirm_;
    ThumbCache thumbs_;
    std::string album_;
    // The scene folder the tiles come from: paths.scenes for the app's own
    // library, or ScreenArgs::b for one opened from a network location.
    Path root_;
    vfs::AsyncListing listing_;
    std::vector<Item> items_;
    std::vector<JobView> pending_;  // queued/running conversions into this folder
    std::vector<JobView> rotating_; // queued/running file turns (any scene here)
    std::size_t finished_ = 0;      // jobs into this folder that ended (done/failed/cancelled)
    // One owned image per tile ("+" has none): the thumbnail once it has
    // loaded, empty before that (the grid draws the placeholder).
    std::vector<UiImage> tiles_;
    std::vector<char> thumb_ready_;
    // Why the last listing failed ("" when it worked): drawn instead of the
    // empty-folder hint, so an offline share is not shown as an empty one.
    std::string error_;
    std::string focus_key_;  // "scene:<folder>", "job:<id>" or "add"
    // The album or network library is hidden: the screen lists nothing, says
    // "That folder is not visible" and keeps the folder's name out of its title.
    bool blocked_ = false;
    // The next published listing should restore focus_key_ (a revealed screen)
    // instead of starting at the first scene.
    bool restore_focus_on_apply_ = false;
};

}  // namespace splat
