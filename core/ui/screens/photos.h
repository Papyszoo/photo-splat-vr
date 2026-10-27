// The photo picker behind a folder's "+" tile: choose photos to turn into
// scenes in that folder (ScreenArgs::a, "" = the loose scenes).
//
// Always four sources, in this order, each with its own empty state: Inbox
// (the FTP upload target; the line under the tabs says where to upload),
// Console (a folder browser over what the title can read: /data and the
// extended storage /mnt/extN; the app's own folder is hidden), USB (one tab per
// stick when there are several) and Captures. A grid of photo tiles with
// asynchronous thumbnails; Cross toggles a photo, Square continues to the model
// choice with the selection (or, on Captures, copies the selected captures into
// the inbox and jumps to the Inbox tab). L1/R1 switch tabs.
//
// In the Console tab, Cross on a folder enters it and Circle goes back up; at
// the top Circle leaves the picker as usual. Only folders and .jpg/.png files
// are shown, and the place is a breadcrumb under the tab ("photos > 2026-10";
// the tab itself says "Console"), never a raw path.
//
// The tabs are rebuilt from the filesystem on on_enter(), on every tab switch
// and from refresh(): a USB stick that was pulled out is gone afterwards.
// Sources and listings come from core/gallery; nothing here touches the
// filesystem directly.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/path.h"
#include "gallery/photo_sources.h"
#include "ui/screen.h"
#include "ui/thumb_cache.h"
#include "ui/widgets.h"

namespace splat {

// One tab: a photo source from discover_sources(), the console browser or the
// capture service.
struct PhotoTab {
    std::string id;          // "inbox", "console", "usb0" (or "usb" with none), "captures"
    std::string title;       // "Inbox", "Console", "USB" / "USB 1", "Captures"
    std::string label;       // the tab text (the title)
    Path dir;                // source directory; empty on console/captures and without a stick
    bool removable = false;  // a USB stick: dropped when it vanishes
    bool captures = false;
    bool console = false;
    std::size_t count = 0;
};

class PhotosScreen : public Screen {
public:
    void on_enter(UiContext& ctx, const ScreenArgs& args) override;
    void update(UiContext& ctx, const UiInput& input) override;
    void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) override;
    std::vector<Hint> hints() const override;
    std::string title() const override { return "Add photos"; }
    bool handle_back(UiContext& ctx) override;

    // Re-discovers the sources and re-lists every one of them (the counts are
    // part of the tab labels). Keeps the current tab while its id is still
    // there, otherwise falls back to the first tab (Inbox).
    void refresh(UiContext& ctx);
    // Switches to `index` (clamped) and clears the basket: a basket belongs to
    // one source, so a conversion never mixes an inbox photo with a capture.
    void set_tab(UiContext& ctx, int index);

    // For the tests and the preview.
    const std::vector<PhotoTab>& tabs() const { return tabs_; }
    int tab() const { return tab_; }
    const GridView& grid() const { return grid_; }
    const std::vector<Path>& selection() const { return selection_; }
    bool selected(const Path& path) const;
    // The empty-state line of the current tab ("" when it has photos).
    const std::string& message() const { return message_; }
    // The destination folder ("" = the loose scenes).
    const std::string& album() const { return album_; }
    // The console browser's folder ("" = the list of roots).
    const Path& console_dir() const { return console_dir_; }
    // The dim line under the tabs: where this tab's photos come from.
    std::string location() const;
    // The console browser's place below the tab: the storage (when there are
    // several), then the folders; empty at the top.
    std::vector<std::string> breadcrumb() const;

private:
    // One tile of the current tab.
    struct Entry {
        Path path;
        std::string name;
        uint64_t size = 0;
        bool folder = false;     // console browser: a folder to enter
        bool supported = true;   // false: an HDR capture
    };

    bool on_captures() const;
    bool on_console() const;
    void load_tab(UiContext& ctx);
    void load_console();
    void enter_folder(UiContext& ctx, const Path& dir);
    std::string root_label(std::size_t index) const;
    int root_index(const Path& dir) const;
    bool at_console_top() const;
    std::string display_path(const Path& path) const;
    void sync_cells(UiContext& ctx, const Rect& area);
    void toggle(const Path& path);
    void secondary(UiContext& ctx);
    // One frame of the async capture copy: state() == kCopying means the
    // helper still has it; anything else is the outcome (toast, and a re-list
    // on success).
    void poll_copy(UiContext& ctx);
    Rect grid_area(const Rect& content) const;
    // Where each source tab is drawn: the pointer clicks the same boxes.
    std::vector<Rect> tab_rects(const Rect& content, const UiTheme& theme) const;

    std::string album_;
    std::vector<PhotoTab> tabs_;
    std::vector<Entry> entries_;
    std::vector<PhotoSource> console_roots_;
    Path console_dir_;
    Path data_root_;   // shown as /data in paths
    Path hidden_dir_;  // the app's own folder, not listed in the browser
    std::vector<Path> selection_;
    // A copy the service has accepted but not answered yet: the frame loop
    // keeps drawing (the line under the tabs says "Copying...") and update()
    // finishes it.
    bool copying_ = false;
    // One decoded tile per cell, copied out of the cache so the GridCell
    // pointers stay valid across frames (the cache only promises a pointer
    // until its next request/get).
    std::vector<UiImage> thumbs_owned_;
    std::vector<std::string> thumb_keys_;
    std::string cells_key_;  // tab id + folder + count the cells were built for
    int loaded_ = 0;         // tiles present when the cells were built
    GridView grid_;
    ThumbCache thumbs_{96};
    std::string message_;
    int tab_ = 0;
};

}  // namespace splat
