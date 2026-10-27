// The folder picker behind a scene's context menu ("Move to folder"): choose
// where to move one scene folder. It lists every album plus "Unsorted" (the
// loose scenes at the root) and a "New folder" row that opens the keyboard.
//
// Arguments: a = "move-scene", b = the scene folder path. Cross on an album
// calls move_scene(), toasts the result and pops; the scenes screen re-lists on
// return. Cross on "New folder" pushes KeyboardScreen with a = "new-album" and
// the same default date name the albums screen uses.
//
// There is deliberately no "Delete this scene" row: v1 has no scene deletion.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "common/path.h"
#include "ui/screen.h"
#include "ui/widgets.h"

namespace splat {

class AlbumPickScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Move to folder"; }

    // Row labels in list order: the albums (the implicit one is "Unsorted")
    // and the trailing "New folder".
    std::vector<std::string> names() const;
    const ListView& list() const { return list_; }
    void focus_row(int index) { list_.set_focus(index); }
    const std::string& scene_folder() const { return scene_folder_; }

private:
    struct Item {
        std::string album;     // album folder name; "" for the loose scenes
        std::string label;
        std::string sublabel;
        bool create = false;
    };

    void reload(UiContext& ctx);
    Rect list_area(const Rect& content) const;

    ListView list_;
    std::vector<Item> items_;
    std::string scene_folder_;
    std::string default_name_;
};

}  // namespace splat
