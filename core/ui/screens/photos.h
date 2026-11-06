// The photo picker behind a folder's "+" tile: choose photos to turn into
// scenes in that folder (ScreenArgs::a, "" = the loose scenes).
//
// Always the same sources, in this order, each with its own empty state: Inbox
// (the FTP upload target; the line under the tabs says where to upload),
// Console (a folder browser over what the title can read: /data and the
// extended storage /mnt/extN; the app's own folder is hidden), USB (one tab per
// stick when there are several), Captures, one tab per network location that is
// a photo source, and last the "+ Add network location" tab. A grid of photo
// tiles with asynchronous thumbnails; Cross toggles a photo, Square continues
// to the model choice with the selection (or, on Captures, copies the selected
// captures into the inbox and jumps to the Inbox tab). L1/R1 switch tabs.
//
// The picker is not filtered by the visible-folders passcode (user decision
// 2026-11-06): every photo source, console folder and location is listed, and
// only the scene shelf hides albums. Cross on the last tab pushes
// ScreenId::LocationEdit with an empty id, so a new share can be added from the
// place the photos are looked for; the editor's return re-lists the tabs and
// the new location is there.
//
// In the Console tab, Cross on a folder enters it and Circle goes back up; at
// the top Circle leaves the picker as usual. Folders, photos and (L2) splat
// files are shown, and the place is a breadcrumb under the tab ("photos >
// 2026-10"; the tab itself says "Console"), never a raw path.
//
// A splat file (an already-converted scene: .splat, .spz, .compressed.ply, ...)
// is a tile labelled "SPLAT · spz" with a SPLAT badge. Cross imports it into the
// album the picker was opened for; it is never added to the photo basket. Square
// on a folder row imports every splat file at most three levels below it (at
// most 50), so a whole export can be brought in from one folder.
//
// The tabs are rebuilt from the filesystem on on_enter(), on every tab switch
// and from refresh(): a USB stick that was pulled out is gone afterwards.
// Sources and listings come from core/gallery; nothing here touches the
// filesystem directly.
//
// The turn is a file action, not a picker-only choice: L3/R3 on the focused
// photo turns it left/right by 90 degrees, the rotated version replaces the
// previous one (the source's Orientation tag for a JPEG/baseline TIFF, a saved
// turn in <state>/photo-turns.json otherwise). The thumbnail is drawn turned
// and fitted inside its tile (core/ui/widgets.cpp), in the photo's current
// orientation; nothing marks the angle on the tile.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "common/path.h"
#include "gallery/photo_sources.h"
#include "ui/screen.h"
#include "ui/thumb_cache.h"
#include "ui/widgets.h"
#include "vfs/listing.h"

namespace splat {

// One tab: a photo source from discover_sources(), the console browser, the
// capture service, a network location or the trailing "add" tab.
struct PhotoTab {
    std::string id;          // "inbox", "console", "usb0" (or "usb"), "captures",
                             // a location id, "add_location"
    std::string title;       // "Inbox", "Console", "USB" / "USB 1", "Captures",
                             // the location's name, "+ Add network location"
    std::string label;       // the tab text (the title)
    Path dir;                // source directory; empty on console/captures and without a stick
    bool removable = false;  // a USB stick: dropped when it vanishes
    bool captures = false;
    bool console = false;
    // A network location's photo source: dir is "<id>:/<path>", and the count
    // is not computed (a tab label must not wait on a NAS).
    bool location = false;
    // The terminal tab: Cross opens the location editor (stage U1).
    bool add_location = false;
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
    // The basket: the photos to convert, each with its quarter turns. A pick's
    // path compares equal to a Path argument through selected().
    const std::vector<PhotoPick>& selection() const { return selection_; }
    // Clockwise quarter turns the focused photo carries (0 when the focus is
    // not on a photo).
    int focused_quarters() const;
    // Square on a folder row: imports every splat file below it. Public so a
    // test can drive the same path the button takes.
    void import_splat_folder(UiContext& ctx, const Path& dir);
    bool selected(const Path& path) const;
    // The empty-state line of the current tab ("" when it has photos).
    const std::string& message() const { return message_; }
    // How many thumbnail loaders the cache has run: a layout change must not
    // make it decode the same pictures again (stage T).
    std::size_t thumbnail_loads() const { return thumbs_.loads(); }
    // The destination folder ("" = the loose scenes).
    const std::string& album() const { return album_; }
    // The console browser's folder ("" = the list of roots).
    const Path& console_dir() const { return console_dir_; }
    // The dim line under the tabs: where this tab's photos come from.
    std::string location() const;
    // The console browser's place below the tab: the storage (when there are
    // several), then the folders; empty at the top.
    std::vector<std::string> breadcrumb() const;
    // The grid area below the tab header: the one rect the picker lays its tiles
    // out in, for the tests' area table too (no second copy of kGridTop).
    static Rect grid_area(const Rect& content, const UiTheme& theme);
    // The tabs drawn this frame, left to right: whole tabs only, inside the
    // content rect and clear of the R1 badge. `tab_first` is the index of the
    // first one (0 unless the strip overflows; the focused tab is always drawn).
    std::vector<Rect> tab_rects(const Rect& content, const UiTheme& theme) const;
    int tab_first(const Rect& content, const UiTheme& theme) const;
    bool on_add_location() const;
    static constexpr const char* kAddLocationLabel = "+ Add network location";
    static constexpr const char* kAddLocationHint =
        "Add a network location to pick photos from that share.";

private:
    // One tile of the current tab.
    struct Entry {
        Path path;
        std::string name;
        uint64_t size = 0;
        bool folder = false;     // console browser: a folder to enter
        bool supported = true;   // false: an HDR capture
        int rotate_quarters = 0; // Square on this tile: the turn it is drawn with
        bool splat = false;      // an already-converted splat file: import it
        uint32_t source_count = 0;  // the splat count, 0 until read
    };

    bool on_captures() const;
    bool on_console() const;
    // What the worker publishes for a file-backed tab.
    struct TabListing {
        std::vector<Entry> entries;
        std::string message;
    };

    void request_tab();
    void apply_tab(UiContext& ctx);
    void load_tab(UiContext& ctx);
    // Puts the focus back on the entry the screen was left on (a reveal, e.g.
    // back from Settings after the thumbnail size changed), once, after the
    // listing that contains it has been published.
    void restore_focus();
    void enter_folder(UiContext& ctx, const Path& dir);
    std::string root_label(std::size_t index) const;
    int root_index(const Path& dir) const;
    bool at_console_top() const;
    // The visible-folders key of a photo from the current tab ("" when this
    // build has no policy context: the old meta.json is written then). It is
    // information for meta.json; the picker is not filtered by it.
    std::string source_key(const Path& path) const;
    std::string display_path(const Path& path) const;
    void sync_cells(UiContext& ctx, const Rect& area);
    void toggle(const Path& path, int rotate_quarters);
    // L3/R3 on the focused photo: the file action, left/right by 90 degrees.
    void rotate_focused(UiContext& ctx, int delta_quarters);
    // Whether the focused tile is a photo this screen can select or turn.
    bool focused_is_photo() const;
    // The turn this path carries, so a re-list cannot forget it.
    int quarters_for(const Path& path) const;
    void secondary(UiContext& ctx);
    // One frame of the async capture copy: state() == kCopying means the
    // helper still has it; anything else is the outcome (toast, and a re-list
    // on success).
    void poll_copy(UiContext& ctx);

    std::string album_;
    // The library root the picker was opened for: the local scene folder, or a
    // network library (ScreenArgs::b). A conversion from a remote root is
    // staged locally and uploaded there.
    Path root_;
    std::vector<PhotoTab> tabs_;
    std::vector<Entry> entries_;
    // The tab's listing: inline for a local source, on a worker for a network
    // location, published by update() through apply_tab().
    vfs::AsyncListing listing_;
    std::vector<PhotoSource> console_roots_;
    Path console_dir_;
    Path data_root_;   // shown as /data in paths
    Path hidden_dir_;  // the app's own folder, not listed in the browser
    std::vector<PhotoPick> selection_;
    // Saved clockwise quarter turns, keyed by vfs::path_key: the turn belongs
    // to the file and survives a relaunch and a re-list. A rewritten file has
    // no entry (its Orientation tag carries the turn).
    std::map<std::string, int> rotations_;
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
    // The entry a reveal should focus (a path; empty = start at the first
    // tile). Set by on_enter() when the same tab and folder are shown again.
    Path restore_path_;
    int loaded_ = 0;         // tiles present when the cells were built
    GridView grid_;
    ThumbCache thumbs_{96};
    std::string message_;
    int tab_ = 0;
};

}  // namespace splat
