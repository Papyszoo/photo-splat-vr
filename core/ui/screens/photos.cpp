// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/photos.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

#include "gallery/photo_sources.h"
#include "gallery/photo_turns.h"
#include "gallery/visibility.h"
#include "gallery/thumbs.h"
#include "ui/button_glyph.h"
#include "ui/capture_service.h"
#include "ui/screen_stack.h"
#include "ui/screens/location_edit.h"
#include "ui/text_fit.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr int kTabHeight = 40;
constexpr int kTabGap = 8;
constexpr int kBadgeHeight = 32;  // the L1/R1 badges beside the tabs
constexpr int kLocationTop = 54;
constexpr int kGridTop = 96;
constexpr int kThumbEdge = 256;
constexpr const char* kCrumb = " \xE2\x80\xBA ";  // " > " as a single angle quote

bool same_path(const Path& a, const Path& b) {
    return !a.empty() && !b.empty() && a.lexically_normal() == b.lexically_normal();
}

// The format token under a splat tile ("spz", "splat", "compressed ply"). It is
// the file name, not the content: a tile never opens a file just to draw it.
std::string splat_extension_label(const Path& path) {
    std::string name = path.filename().string();
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string compressed = ".compressed.ply";
    if (name.size() >= compressed.size() &&
        name.compare(name.size() - compressed.size(), compressed.size(), compressed) == 0) {
        return "compressed ply";
    }
    std::string extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

// The tab strip, laid out once for both the pointer and the painter. It starts
// after the L1 badge and stops before the R1 badge, which stays at the right
// edge of the content rect. With more tabs than fit, whole tabs are dropped
// from the left until the focused one and as many before it as fit are shown;
// a tab is never cut in half.
struct TabStrip {
    int first = 0;
    std::vector<Rect> rects;
};

TabStrip tab_strip(const Rect& content, const UiTheme& theme,
                   const std::vector<PhotoTab>& tabs, int focused) {
    TabStrip strip;
    if (tabs.empty() || content.w <= 0) return strip;
    const int tab_height = std::max(1, theme.scaled(kTabHeight));
    const int badge_height = std::max(1, theme.scaled(kBadgeHeight));
    const int tab_size = theme.body_size - 2;
    const int gap = theme.scaled(kTabGap);
    const int left = content.x + button_glyph_width("L1", badge_height) + theme.scaled(14);
    const int badge = button_glyph_width("R1", badge_height);
    const int right = std::max(left + 1, content.right() - badge - theme.scaled(12));

    std::vector<int> widths;
    widths.reserve(tabs.size());
    for (const PhotoTab& tab : tabs) {
        widths.push_back(static_cast<int>(UiCanvas::text_width(tab.title, tab_size)) +
                         theme.scaled(36));
    }
    const int focus = std::max(0, std::min(focused, static_cast<int>(tabs.size()) - 1));
    // From the focused tab, take in whole tabs to its left while they fit, so
    // a strip that fits completely always starts at tab 0.
    int first = focus;
    int used = widths[static_cast<std::size_t>(focus)];
    while (first > 0 &&
           used + gap + widths[static_cast<std::size_t>(first - 1)] <= right - left) {
        --first;
        used += gap + widths[static_cast<std::size_t>(first)];
    }
    int x = left;
    for (int index = first; index < static_cast<int>(tabs.size()); ++index) {
        const int width = widths[static_cast<std::size_t>(index)];
        if (index != first && x + width > right) break;
        // A content rect narrower than one tab still keeps the tab inside it.
        const int clamped = std::max(1, std::min(width, right - x));
        strip.rects.push_back(Rect{x, content.y, clamped, tab_height});
        x += width + gap;
    }
    strip.first = first;
    return strip;
}

}  // namespace

std::vector<Rect> PhotosScreen::tab_rects(const Rect& content, const UiTheme& theme) const {
    return tab_strip(content, theme, tabs_, tab_).rects;
}

int PhotosScreen::tab_first(const Rect& content, const UiTheme& theme) const {
    return tab_strip(content, theme, tabs_, tab_).first;
}

bool PhotosScreen::on_add_location() const {
    return !tabs_.empty() && tabs_[static_cast<std::size_t>(tab_)].add_location;
}

void PhotosScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    // A reveal (back from the model choice) keeps the tab, the folder and the
    // basket; a fresh push starts at the Inbox.
    const Path root = args.b.empty() ? ctx.svc.paths.scenes : Path(args.b);
    const bool returning = !tabs_.empty() && args.a == album_ && root == root_;
    // A reveal also keeps the entry the focus was on: opening Settings and
    // changing the thumbnail size must not send the picker back to the first
    // tile. The path is applied once the listing is back.
    restore_path_.clear();
    if (returning) {
        const int focus = grid_.focus();
        if (focus >= 0 && focus < static_cast<int>(entries_.size())) {
            restore_path_ = entries_[static_cast<std::size_t>(focus)].path;
        }
    }
    album_ = args.a;
    root_ = root;
    if (!returning) {
        selection_.clear();
        tab_ = 0;
        tabs_.clear();
        console_dir_.clear();
    }
    // The saved turns live in <state>/photo-turns.json and belong to the files,
    // not to one tab: a relaunch, a tab switch and a re-list all read them
    // again, so a saved turn never depends on this object's lifetime.
    rotations_ = PhotoTurnStore(ctx.svc.paths.state).load_all();
    // A copy started before the screen was left is still the frame loop's job:
    // pick the wait up again so its completion (or error) is still reported.
    copying_ = ctx.svc.captures != nullptr &&
               ctx.svc.captures->state() == CaptureService::kCopying;
    refresh(ctx);
}

bool PhotosScreen::on_captures() const {
    return !tabs_.empty() && tabs_[static_cast<std::size_t>(tab_)].captures;
}

bool PhotosScreen::on_console() const {
    return !tabs_.empty() && tabs_[static_cast<std::size_t>(tab_)].console;
}

void PhotosScreen::refresh(UiContext& ctx) {
    const std::string keep = tabs_.empty() ? std::string() : tabs_[static_cast<std::size_t>(tab_)].id;

    // Stage U1: the photo picker is not filtered by the visible-folders list.
    // Every source, folder and photo is here; the passcode only hides scene
    // albums and network scene libraries (the shelf, not this screen).
    SourceRoots roots;
    roots.inbox = ctx.svc.paths.inbox;
    roots.mnt = ctx.svc.paths.mnt;
    roots.data = ctx.svc.paths.data;
    console_roots_ = console_roots(roots);
    data_root_ = ctx.svc.paths.data;
    // The app's own folder (inbox, state, scenes) is not a place to pick from.
    hidden_dir_ = ctx.svc.paths.inbox.parent_path();
    // The saved turns are read once per refresh: a file may have gained one in
    // another screen or another session, and the badge must not go stale.
    rotations_ = PhotoTurnStore(ctx.svc.paths.state).load_all();

    // Always the same sources, in this order, each with its own empty state:
    // Inbox, Console, USB (one tab per stick when there are several), Captures,
    // then the network locations that are photo sources. The last tab is
    // "+ Add network location" when this build has the location service.
    std::vector<PhotoTab> tabs;
    std::vector<PhotoTab> sticks;
    std::string source_ids;
    for (const PhotoSource& source : discover_sources(roots)) {
        source_ids += " " + source.id;
        PhotoTab tab;
        tab.id = source.id;
        tab.title = source.label;
        tab.dir = source.dir;
        tab.removable = source.removable;
        tab.count = list_photos(source, true).size();
        (source.removable ? sticks : tabs).push_back(std::move(tab));
    }
    // The console test reads what the title itself sees (W23/W24): the inbox
    // plus every usbN alias that opened. Never the capture helper.
    if (ctx.svc.log) ctx.svc.log("ui: sources" + source_ids);
    PhotoTab console;
    console.id = "console";
    console.title = "Console";
    console.console = true;
    tabs.push_back(std::move(console));
    if (sticks.empty()) {
        PhotoTab usb;
        usb.id = "usb";
        usb.removable = true;
        sticks.push_back(std::move(usb));
    }
    for (std::size_t index = 0; index < sticks.size(); ++index) {
        sticks[index].title = sticks.size() == 1 ? "USB" : "USB " + std::to_string(index + 1);
        tabs.push_back(std::move(sticks[index]));
    }
    PhotoTab captures;
    captures.id = "captures";
    captures.title = "Captures";
    captures.captures = true;
    if (ctx.svc.captures != nullptr && ctx.svc.captures->available()) {
        captures.count = ctx.svc.captures->list().size();
    }
    tabs.push_back(std::move(captures));
    // L3: one tab per network location that asked for one, in store order. The
    // count is left at 0 on purpose: a tab label must not wait on a share (the
    // tab lists its photos when it is opened, through the VFS).
    if (ctx.svc.locations != nullptr) {
        for (const LocationView& location : ctx.svc.locations->list()) {
            if (!location.photo_source) continue;
            PhotoTab tab;
            tab.id = location.id;
            tab.title = location.name;
            tab.dir = vfs::join(location.id, location.path);
            tab.location = true;
            tabs.push_back(std::move(tab));
        }
        // The one way to add a share from where a photo is being looked for.
        PhotoTab add;
        add.id = "add_location";
        add.title = kAddLocationLabel;
        add.add_location = true;
        tabs.push_back(std::move(add));
    }
    for (PhotoTab& tab : tabs) tab.label = tab.title;

    tabs_ = std::move(tabs);
    tab_ = 0;
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        if (tabs_[index].id != keep) continue;
        tab_ = static_cast<int>(index);
        break;
    }
    // The basket belonged to the tab that is gone; a saved turn belongs to the
    // file, so rotations_ is reloaded above and stays.
    if (tabs_.empty() || tabs_[static_cast<std::size_t>(tab_)].id != keep) {
        selection_.clear();
        restore_path_.clear();
        if (tabs_.empty()) console_dir_.clear();
    }
    load_tab(ctx);
}

void PhotosScreen::set_tab(UiContext& ctx, int index) {
    const int count = static_cast<int>(tabs_.size());
    const int clamped = count == 0 ? 0 : std::max(0, std::min(index, count - 1));
    if (clamped != tab_) {
        selection_.clear();
        console_dir_.clear();
        rotations_ = PhotoTurnStore(ctx.svc.paths.state).load_all();
    }
    tab_ = clamped;
    load_tab(ctx);
}

void PhotosScreen::load_tab(UiContext& ctx) {
    entries_.clear();
    message_.clear();
    thumbs_owned_.clear();
    thumb_keys_.clear();
    cells_key_.clear();
    loaded_ = 0;
    grid_.set_cells({});
    grid_.set_focus(0);
    listing_.cancel();
    if (tabs_.empty()) return;

    const PhotoTab tab = tabs_[static_cast<std::size_t>(tab_)];
    if (tab.add_location) {
        // Nothing is listed here: Cross opens the editor and the tab is
        // replaced by the new source's tab when the editor returns.
        message_ = kAddLocationHint;
        return;
    }
    if (tab.captures) {
        if (ctx.svc.captures == nullptr) {
            message_ = "Captures are not available";
            return;
        }
        if (!ctx.svc.captures->available()) {
            // The helper ships in the package but needs the payload loader.
            message_ = "Captures need the payload loader";
            return;
        }
        for (const CaptureView& capture : ctx.svc.captures->list()) {
            entries_.push_back(Entry{capture.path, capture.name, capture.bytes, false,
                                     capture.supported, quarters_for(capture.path), false, 0});
        }
        if (entries_.empty()) message_ = "No captures found";
        return;
    }
    if (tab.removable && tab.dir.empty()) {
        message_ = "No USB drive found";
        return;
    }
    if (tab.location && tab.dir.empty()) {
        message_ = "That location is not configured";
        return;
    }
    if (tab.console && console_dir_.empty() && console_roots_.size() == 1) {
        // Exactly one storage: start inside it, so a console with one root does
        // not show a one-row root list.
        console_dir_ = console_roots_.front().dir;
    }
    if (tab.console && console_dir_.empty()) {
        for (std::size_t index = 0; index < console_roots_.size(); ++index) {
            entries_.push_back(Entry{console_roots_[index].dir, root_label(index), 0, true, true,
                                     quarters_for(console_roots_[index].dir), false, 0});
        }
        if (entries_.empty()) message_ = "No readable storage found";
        return;
    }

    request_tab();
    // A local source is listed inside request_tab(); a location path arrives
    // through update()/tick().
    if (listing_.ready()) apply_tab(ctx);
}

void PhotosScreen::request_tab() {
    const PhotoTab tab = tabs_[static_cast<std::size_t>(tab_)];
    const Path dir = tab.console ? console_dir_ : tab.dir;
    const Path hidden = hidden_dir_;
    const bool console = tab.console;
    vfs::AsyncListing::Job job = [tab, dir, hidden, console] {
        auto listing = std::make_shared<TabListing>();
        if (console) {
            for (const FolderEntry& item : list_folder(dir)) {
                if (item.folder && same_path(item.path, hidden)) continue;
                Entry entry;
                entry.path = item.path;
                entry.name = item.name;
                entry.size = item.size;
                entry.folder = item.folder;
                entry.splat = item.splat;
                entry.source_count = item.source_count;
                listing->entries.push_back(std::move(entry));
            }
            if (listing->entries.empty()) listing->message = "No folders or photos here";
        } else {
            // Splat files are listed with the photos: an FTP upload of a
            // converted scene to the Inbox is the shortest import there is.
            const PhotoSource source{tab.id, tab.title, tab.dir, tab.removable};
            const bool remote = vfs::is_remote(tab.dir);
            // A location that cannot be read is an error, not an empty folder:
            // the screen has an error line for it, and an offline share used to
            // read "No photos in this location" with nothing in the log.
            Result<std::vector<PhotoEntry>> photos = list_photos_checked(source, true);
            if (!photos.ok() && remote) {
                return Result<vfs::AsyncListing::Snapshot>::failure(photos.message());
            }
            if (photos.ok()) {
                for (const PhotoEntry& photo : photos.value()) {
                    Entry entry;
                    entry.path = photo.path;
                    entry.name = photo.name;
                    entry.size = photo.size;
                    entry.splat = photo.splat;
                    listing->entries.push_back(std::move(entry));
                }
            }
            if (!listing->entries.empty()) {
                // listed
            } else if (tab.removable) {
                listing->message = "No photos on this USB drive";
            } else if (remote) {
                listing->message = "No photos in this location";
            } else {
                listing->message = "No photos in the Inbox yet";
            }
        }
        vfs::AsyncListing::Snapshot snapshot;
        snapshot.payload = std::move(listing);
        return Result<vfs::AsyncListing::Snapshot>::success(std::move(snapshot));
    };
    listing_.request(dir, std::move(job));
}

void PhotosScreen::apply_tab(UiContext& ctx) {
    const std::shared_ptr<TabListing> listed =
        std::static_pointer_cast<TabListing>(listing_.payload());
    const std::string error = listing_.error();
    listing_.cancel();  // consumed: ready() must not rebuild the grid again
    if (!error.empty() && ctx.svc.log) ctx.svc.log("ui: photos: " + error);
    if (!listed) {
        if (!error.empty()) message_ = error;
        return;
    }
    entries_ = std::move(listed->entries);
    message_ = std::move(listed->message);
    // The turn is a saved file action keyed by location and path (or a
    // rewrite's 0); the listing itself never carries it.
    for (Entry& entry : entries_) {
        if (!entry.folder) entry.rotate_quarters = quarters_for(entry.path);
    }
    restore_focus();
}

void PhotosScreen::restore_focus() {
    if (restore_path_.empty() || grid_.cells().empty()) return;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].path != restore_path_) continue;
        grid_.set_focus(static_cast<int>(index));
        break;
    }
    // Once: a later re-list (a tab switch) starts at its own first tile.
    restore_path_.clear();
}

std::string PhotosScreen::root_label(std::size_t index) const {
    const PhotoSource& root = console_roots_[index];
    if (root.id == "data") return "Internal storage";
    std::size_t externals = 0;
    for (const PhotoSource& other : console_roots_) externals += other.id == "data" ? 0 : 1;
    if (externals <= 1) return "Extended storage";
    return "Extended storage " + root.id.substr(3);
}

int PhotosScreen::root_index(const Path& dir) const {
    for (std::size_t index = 0; index < console_roots_.size(); ++index) {
        if (same_path(console_roots_[index].dir, dir)) return static_cast<int>(index);
    }
    return -1;
}

std::string PhotosScreen::source_key(const Path& path) const {
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    // No policy context (a unit test, a preview): leave the key empty, which is
    // the pre-stage-H meta.json.
    if (!visibility->has_context()) return std::string();
    if (tabs_.empty()) return visibility->key_for_source(path).text();
    const PhotoTab& tab = tabs_[static_cast<std::size_t>(tab_)];
    // The provenance written into the scene's meta.json, information only: a
    // capture's key is the capture source (the helper copies it into the inbox,
    // so its path would say "inbox"), a console photo's the top-level folder it
    // lives under.
    if (tab.captures) return VisibilityKey::photo_source("captures").text();
    if (tab.console) return visibility->key_for_source(path).text();
    if (tab.location) return VisibilityKey::location(tab.id).text();
    return VisibilityKey::photo_source(tab.id).text();
}

void PhotosScreen::enter_folder(UiContext& ctx, const Path& dir) {
    console_dir_ = dir;
    load_tab(ctx);
}

bool PhotosScreen::at_console_top() const {
    if (!on_console() || console_dir_.empty()) return true;
    return root_index(console_dir_) >= 0;
}


bool PhotosScreen::handle_back(UiContext& ctx) {
    if (tabs_.empty()) return false;
    if (!on_console() || console_dir_.empty()) return false;
    const bool at_root = root_index(console_dir_) >= 0;
    // The only storage is where the browser starts: Circle leaves the picker
    // instead of showing a one-row root list.
    if (at_root && console_roots_.size() <= 1) return false;
    const Path from = console_dir_;
    enter_folder(ctx, at_root ? Path() : console_dir_.parent_path());
    // Keep the focus on the folder we came out of.
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (same_path(entries_[index].path, from)) grid_.set_focus(static_cast<int>(index));
    }
    return true;
}

std::string PhotosScreen::display_path(const Path& path) const {
    // Paths under the console's data root read as /data/..., the way an FTP
    // client shows them.
    if (!data_root_.empty()) {
        const Path relative = path.lexically_normal().lexically_relative(data_root_.lexically_normal());
        const std::string text = relative.generic_string();
        if (!text.empty() && text != "." && text.compare(0, 2, "..") != 0) return "/data/" + text;
    }
    return path.generic_string();
}

std::vector<std::string> PhotosScreen::breadcrumb() const {
    // The tab already says "Console": the crumbs start below it.
    std::vector<std::string> parts;
    if (console_dir_.empty()) return parts;
    for (std::size_t index = 0; index < console_roots_.size(); ++index) {
        const Path& root = console_roots_[index].dir;
        const Path relative = console_dir_.lexically_normal().lexically_relative(root.lexically_normal());
        const std::string text = relative.generic_string();
        if (text.empty() || text.compare(0, 2, "..") == 0) continue;
        if (console_roots_.size() > 1) parts.push_back(root_label(index));
        if (text != ".") {
            for (const Path& part : relative) parts.push_back(part.string());
        }
        break;
    }
    return parts;
}

std::string PhotosScreen::location() const {
    if (tabs_.empty()) return {};
    const PhotoTab& tab = tabs_[static_cast<std::size_t>(tab_)];
    if (tab.console) {
        std::string line;
        for (const std::string& part : breadcrumb()) line += (line.empty() ? "" : kCrumb) + part;
        return line;
    }
    if (tab.add_location) return std::string();
    if (tab.captures) return "Screenshots from this console";
    if (tab.removable) return tab.dir.empty() ? std::string() : "Photos on the USB drive";
    if (tab.location) return tab.dir.empty() ? std::string() : "Photos on " + tab.title;
    return "Upload photos by FTP to " + display_path(tab.dir);
}

Rect PhotosScreen::grid_area(const Rect& content, const UiTheme& theme) {
    const int top = theme.scaled(kGridTop);
    return Rect{content.x, content.y + top, content.w, content.h - top};
}

void PhotosScreen::sync_cells(UiContext& ctx, const Rect& area) {
    const std::size_t count = entries_.size();
    const std::string key = (tabs_.empty() ? std::string() : tabs_[static_cast<std::size_t>(tab_)].id) +
                            ":" + console_dir_.string() + ":" + std::to_string(count);

    bool resized = false;
    if (thumbs_owned_.size() != count || thumb_keys_.size() != count) {
        thumbs_owned_.assign(count, UiImage{});
        thumb_keys_.assign(count, std::string());
        resized = true;
    }

    std::vector<GridCell> cells;
    cells.reserve(count);
    int loaded = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const Entry& entry = entries_[index];
        GridCell cell;
        cell.caption = entry.name;
        cell.dim = !entry.supported;
        cell.checked = !entry.folder && selected(entry.path);
        cell.turn_quarters = entry.folder ? 0 : entry.rotate_quarters;
        // Every photo tile shows the whole frame (stage TF2): fitted inside the
        // tile with the backdrop in the bars, never cropped. Folder and splat
        // tiles have no picture to fit.
        cell.fit = !entry.folder && !entry.splat;
        if (entry.folder) {
            cell.icon = GridCell::Icon::Folder;
            cells.push_back(std::move(cell));
            continue;
        }
        if (entry.splat) {
            // No thumbnail: a splat file has no photo, and decoding one here
            // would import it twice.
            cell.badge = "SPLAT";
            cell.sublabel = "SPLAT \xC2\xB7 " + splat_extension_label(entry.path);
            cells.push_back(std::move(cell));
            continue;
        }
        if (!entry.supported) {
            cell.caption = entry.name + " (HDR)";
            cell.sublabel = "HDR";
        }

        const std::string thumb_key = entry.path.string();
        if (thumb_keys_[index] != thumb_key) {
            thumb_keys_[index] = thumb_key;
            thumbs_owned_[index] = UiImage{};
        }
        if (thumbs_owned_[index].empty() && entry.supported) {
            const Path source = entry.path;
            thumbs_.request(thumb_key, [source] { return load_photo_thumb(source, kThumbEdge); });
            if (const UiImage* image = thumbs_.get(thumb_key)) thumbs_owned_[index] = *image;
        }
        if (!thumbs_owned_[index].empty()) {
            cell.thumb = &thumbs_owned_[index];
            ++loaded;
        }
        cells.push_back(std::move(cell));
    }

    if (!resized && key == cells_key_ && loaded == loaded_) {
        // Same tiles: only the selection mark and the drawn turn may have
        // changed.
        for (std::size_t index = 0; index < count; ++index) {
            if (GridCell* cell = grid_.cell(static_cast<int>(index))) {
                cell->checked = cells[index].checked;
                cell->turn_quarters = cells[index].turn_quarters;
                cell->fit = cells[index].fit;
            }
        }
        return;
    }
    const int previous_loaded = loaded_;
    const int focus = grid_.focus();
    grid_.set_cells(std::move(cells));
    grid_.set_focus(focus);
    // set_cells() starts the scroll over; bring the focused row back into view
    // exactly like an idle frame would.
    grid_.update(UiInput{}, area, ctx.theme);
    cells_key_ = key;
    loaded_ = loaded;
    // A tile arriving is a change even when the worker has just gone idle, so
    // the host has to repaint for it to be seen.
    if (loaded != previous_loaded) ctx.invalidate();
}

void PhotosScreen::update(UiContext& ctx, const UiInput& input) {
    // The Sense pointer (or a mouse) picks a source by clicking its tab. The
    // strip is scrolled by whole tabs, so a click maps through tab_first().
    if (input.pointer_valid && input.click) {
        const std::vector<Rect> rects = tab_rects(ctx.content, ctx.theme);
        const int first = tab_first(ctx.content, ctx.theme);
        for (std::size_t index = 0; index < rects.size(); ++index) {
            if (!rects[index].contains(input.px, input.py)) continue;
            refresh(ctx);
            set_tab(ctx, first + static_cast<int>(index));
            return;
        }
    }
    if ((input.pressed & (PagePrev | PageNext)) != 0) {
        refresh(ctx);
        const int count = static_cast<int>(tabs_.size());
        if (count > 0) {
            const int step = (input.pressed & PageNext) != 0 ? 1 : -1;
            set_tab(ctx, ((tab_ + step) % count + count) % count);
        }
    }

    // Cross on the last tab opens the location editor with an empty id: this is
    // where a photo is being looked for, so a share is added without leaving the
    // picker, and it starts as a photo source so its tab is there when the
    // editor returns. The return re-lists the tabs (on_enter -> refresh).
    if ((input.pressed & Select) != 0 && on_add_location()) {
        ScreenArgs args;
        args.b = LocationEditScreen::kPickSourceMode;
        ctx.nav.push(ScreenId::LocationEdit, args);
        return;
    }

    listing_.tick();
    if (listing_.ready()) {
        apply_tab(ctx);
        ctx.invalidate();
    }
    // A location still being listed: keep drawing "Loading..." and no grid.
    if (listing_.busy()) {
        if (thumbs_.pending()) ctx.invalidate();
        return;
    }

    // A splat tile's second caption says which format it is; photos keep the
    // two-line name of the shared label rule.
    grid_.show_sublabel = std::any_of(entries_.begin(), entries_.end(),
                                      [](const Entry& entry) { return entry.splat; });
    const Rect area = grid_area(ctx.content, ctx.theme);
    apply_thumbnail_layout(grid_, area, ctx.theme, ctx.thumbnail_size());
    sync_cells(ctx, area);
    // The reveal's focus can be applied only once the cells exist (a local
    // listing clears them in load_tab() before publishing inside it).
    restore_focus();
    // L1/R1 are the source tabs on this screen, never a grid page: the same
    // edge that switched the tab must not also move the new tab's focus.
    UiInput grid_input = input;
    grid_input.pressed &= ~static_cast<uint32_t>(PagePrev | PageNext);
    grid_.update(grid_input, area, ctx.theme);

    // L3/R3 turn the focused entry 90 degrees: the file action, on both this
    // screen and the Scenes shelf (the same two keys everywhere).
    if ((input.pressed & RotateLeft) != 0 && focused_is_photo()) {
        rotate_focused(ctx, -1);
        sync_cells(ctx, area);
    }
    if ((input.pressed & RotateRight) != 0 && focused_is_photo()) {
        rotate_focused(ctx, 1);
        sync_cells(ctx, area);
    }

    if (grid_.activated()) {
        grid_.clear_activated();
        const int index = grid_.focus();
        if (index >= 0 && index < static_cast<int>(entries_.size())) {
            const Entry& entry = entries_[static_cast<std::size_t>(index)];
            if (entry.folder) {
                enter_folder(ctx, entry.path);
                sync_cells(ctx, area);
            } else if (entry.splat) {
                if (ctx.svc.jobs == nullptr) {
                    ctx.toast("Imports are not available in this build");
                } else {
                    const Status status = ctx.svc.jobs->enqueue_import(
                        {entry.path}, album_, nullptr, vfs::is_remote(root_) ? root_ : Path(),
                        source_key(entry.path));
                    ctx.toast(status.ok() ? "Importing " + entry.name : status.message());
                }
            } else if (!entry.supported) {
                ctx.toast(entry.name + " is HDR, not supported");
            } else {
                toggle(entry.path, entry.rotate_quarters);
                sync_cells(ctx, area);
            }
        }
    }

    if ((input.pressed & Secondary) != 0) secondary(ctx);
    if (copying_) poll_copy(ctx);
    if (thumbs_.pending()) ctx.invalidate();
}

void PhotosScreen::toggle(const Path& path, int rotate_quarters) {
    const auto found = std::find_if(selection_.begin(), selection_.end(),
                                    [&path](const PhotoPick& pick) { return pick.path == path; });
    if (found != selection_.end()) {
        selection_.erase(found);
    } else {
        // The key travels with the pick: the queue never guesses the tab.
        selection_.push_back(PhotoPick{path, rotate_quarters, source_key(path)});
    }
}

bool PhotosScreen::selected(const Path& path) const {
    return std::find_if(selection_.begin(), selection_.end(), [&path](const PhotoPick& pick) {
               return pick.path == path;
           }) != selection_.end();
}

int PhotosScreen::quarters_for(const Path& path) const {
    const auto found = rotations_.find(vfs::path_key(path).string());
    return found == rotations_.end() ? 0 : found->second;
}

bool PhotosScreen::focused_is_photo() const {
    const int index = grid_.focus();
    if (index < 0 || index >= static_cast<int>(entries_.size())) return false;
    const Entry& entry = entries_[static_cast<std::size_t>(index)];
    return !entry.folder && entry.supported && !entry.splat;
}

int PhotosScreen::focused_quarters() const {
    if (!focused_is_photo()) return 0;
    return entries_[static_cast<std::size_t>(grid_.focus())].rotate_quarters;
}

void PhotosScreen::rotate_focused(UiContext& ctx, int delta_quarters) {
    const int index = grid_.focus();
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    Entry& entry = entries_[static_cast<std::size_t>(index)];
    if (entry.folder || entry.splat || !entry.supported) return;

    // The file action: a JPEG/baseline TIFF gets its Orientation tag rewritten;
    // everything else keeps the source and stores the turn. A capture is always
    // the stored route: the helper owns those files.
    PhotoTurnStore store(ctx.svc.paths.state);
    const Result<PhotoTurnResult> turned =
        rotate_photo_file(entry.path, delta_quarters, store, !on_captures());
    if (!turned.ok()) {
        ctx.toast(turned.message().empty() ? "Cannot rotate this photo" : turned.message());
        return;
    }
    const PhotoTurnResult& result = turned.value();
    entry.rotate_quarters = result.quarters;
    const std::string key = vfs::path_key(entry.path).string();
    if (result.quarters == 0) {
        rotations_.erase(key);
    } else {
        rotations_[key] = result.quarters;
    }
    // The basket carries the same turn into the conversion. After a rewrite the
    // file's own tag is the turn, so the job must receive 0 - applying it again
    // would turn the photo twice.
    for (PhotoPick& pick : selection_) {
        if (pick.path == entry.path) pick.rotate_quarters = result.quarters;
    }
    if (result.route == PhotoTurnResult::Route::rewritten) {
        // The picture on disk changed: drop the decoded tile, and drop any load
        // already in flight for it, so the grid asks for the new one.
        thumbs_.forget(entry.path.string());
        if (thumb_keys_.size() == entries_.size()) thumb_keys_[static_cast<std::size_t>(index)].clear();
        if (thumbs_owned_.size() == entries_.size()) {
            thumbs_owned_[static_cast<std::size_t>(index)] = UiImage{};
        }
    }
    const std::string direction = delta_quarters >= 0 ? " right" : " left";
    ctx.toast("Rotated " + entry.path.stem().string() + direction +
              (result.route == PhotoTurnResult::Route::rewritten
                   ? " (file updated)"
                   : "; the file itself was not changed"));
    ctx.invalidate();
}

void PhotosScreen::secondary(UiContext& ctx) {
    if (tabs_.empty()) return;
    if (on_captures()) {
        if (ctx.svc.captures == nullptr) return;
        if (copying_) {
            ctx.toast("Still copying to the inbox");
            return;
        }
        if (selection_.empty()) {
            ctx.toast("Select captures to copy");
            return;
        }
        // The capture copy takes paths: a capture has no quarter turn.
        std::vector<Path> captures;
        captures.reserve(selection_.size());
        for (const PhotoPick& pick : selection_) captures.push_back(pick.path);
        const Status started = ctx.svc.captures->copy_to_inbox(captures);
        if (!started.ok()) {
            ctx.toast(started.message());
            return;
        }
        selection_.clear();
        if (ctx.svc.captures->state() == CaptureService::kCopying) {
            // The helper still has it: the line under the tabs says
            // "Copying..." and every later update() polls it.
            copying_ = true;
            return;
        }
        // A service that answers inside the call (the fakes, fake console
        // mode) is already finished; report it like a landed copy.
        poll_copy(ctx);
        return;
    }

    // Square on a folder row imports everything splat-shaped below it: one
    // press brings a whole export in.
    const int focus = grid_.focus();
    if (on_console() && focus >= 0 && focus < static_cast<int>(entries_.size()) &&
        entries_[static_cast<std::size_t>(focus)].folder) {
        import_splat_folder(ctx, entries_[static_cast<std::size_t>(focus)].path);
        return;
    }

    // Cross marks a photo for the basket and Square converts the basket. The
    // turn is the file action on L3/R3, so it is not hidden behind Square on a
    // photo that is not selected.
    if (selection_.empty()) {
        ctx.toast("Select photos with Cross first");
        return;
    }
    ScreenArgs args;
    args.a = album_;
    args.b = root_.string();
    args.list.reserve(selection_.size());
    args.turns.reserve(selection_.size());
    args.sources.reserve(selection_.size());
    for (const PhotoPick& pick : selection_) {
        args.list.push_back(pick.path.string());
        args.turns.push_back(((pick.rotate_quarters % 4) + 4) % 4);
        // The dialog refuses a hidden source and the queue receives the same
        // key: one rule, one spelling.
        args.sources.push_back(pick.source_folder.empty() ? source_key(pick.path)
                                                          : pick.source_folder);
    }
    ctx.nav.push(ScreenId::ConvertDialog, args);
}

void PhotosScreen::import_splat_folder(UiContext& ctx, const Path& dir) {
    if (ctx.svc.jobs == nullptr) {
        ctx.toast("Imports are not available in this build");
        return;
    }
    const std::vector<Path> files = list_splat_files(dir);
    if (files.empty()) {
        ctx.toast("No splat files in this folder");
        return;
    }
    std::size_t queued = 0;
    const Status status = ctx.svc.jobs->enqueue_import(
        files, album_, &queued, vfs::is_remote(root_) ? root_ : Path(), source_key(dir));
    if (!status.ok()) {
        ctx.toast(status.message());
        return;
    }
    ctx.toast("Queued " + std::to_string(queued) + (queued == 1 ? " import" : " imports"));
}

void PhotosScreen::poll_copy(UiContext& ctx) {
    if (ctx.svc.captures == nullptr) {
        copying_ = false;
        return;
    }
    const std::string state = ctx.svc.captures->state();
    if (state == CaptureService::kCopying) return;  // still with the helper
    copying_ = false;

    if (!CaptureService::copy_finished(state)) {
        // The helper's own error, a timeout or a dead mailbox.
        ctx.toast(state.empty() ? std::string("Copy failed") : state);
        return;
    }

    const std::string copied =
        state.substr(std::string(CaptureService::kCopiedPrefix).size());
    ctx.toast("Copied " + copied + " capture(s) to the inbox");
    // The copies are photos now: re-count every tab and show the inbox.
    refresh(ctx);
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        if (tabs_[index].id != "inbox") continue;
        set_tab(ctx, static_cast<int>(index));
        break;
    }
}

void PhotosScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;

    // Source tabs as pills between the L1 and R1 badges. The strip never leaves
    // the content rect: with many tabs it scrolls by whole tabs and always
    // draws the focused one, and R1 keeps the right edge.
    const int tab_height = std::max(1, theme.scaled(kTabHeight));
    const int badge_height = std::max(1, theme.scaled(kBadgeHeight));
    const int cy = content.y + tab_height / 2;
    const Rgba badge{255, 255, 255, 46};
    draw_button_glyph(canvas, content.x, cy, "L1", badge_height, badge, theme.text);
    const int tab_size = theme.body_size - 2;
    const TabStrip strip = tab_strip(content, theme, tabs_, tab_);
    for (std::size_t offset = 0; offset < strip.rects.size(); ++offset) {
        const std::size_t index = static_cast<std::size_t>(strip.first) + offset;
        if (index >= tabs_.size()) break;
        const bool active = static_cast<int>(index) == tab_;
        const Rect& box = strip.rects[offset];
        if (active) canvas.fill_round_rect(box.x, box.y, box.w, box.h, box.h / 2, theme.light);
        canvas.text(box.x + theme.scaled(18),
                    UiCanvas::text_top_centered(box.y, box.h, tab_size),
                    fit_text(tabs_[index].title, tab_size, box.w - theme.scaled(24)), tab_size,
                    active ? theme.background_bottom : theme.dim_text);
    }
    draw_button_glyph(canvas,
                      content.right() - button_glyph_width("R1", badge_height) - theme.scaled(4),
                      cy, "R1", badge_height, badge, theme.text);

    // Where the photos come from, in plain words; the capture copy state too.
    std::string where = location();
    if (on_console()) {
        // A long breadcrumb keeps its deepest folders behind a leading "…".
        std::vector<std::string> parts = breadcrumb();
        while (parts.size() > 1 &&
               static_cast<int>(UiCanvas::text_width(where, theme.small_size)) > content.w - 8) {
            parts.erase(parts.begin());
            where = "\xE2\x80\xA6";
            for (const std::string& part : parts) where += kCrumb + part;
        }
    }
    if (on_captures() && ctx.svc.captures != nullptr) {
        const std::string state = ctx.svc.captures->state();
        if (!state.empty() && state != CaptureService::kIdle &&
            state != CaptureService::kNeedsLoader) {
            where += "  \xC2\xB7  " + state;
        }
    }
    canvas.text(content.x + 4, content.y + theme.scaled(kLocationTop),
                fit_text(where, theme.small_size, content.w - 8), theme.small_size, theme.dim_text);

    const Rect area = grid_area(content, theme);
    if (listing_.busy()) {
        canvas.text(area.x + 4, area.y + 40, "Loading\xE2\x80\xA6", theme.heading_size, theme.text);
        return;
    }
    if (!message_.empty()) {
        canvas.text(area.x + 4, area.y + 40, message_, theme.heading_size, theme.text);
        canvas.text(area.x + 4, area.y + 40 + UiCanvas::line_height(theme.heading_size),
                    on_add_location() ? "Press Cross to add a share."
                                      : "L1 and R1 switch to the other sources.",
                    theme.body_size - 2, theme.dim_text);
        return;
    }
    apply_thumbnail_layout(grid_, area, theme, ctx.thumbnail_size());
    grid_.draw(canvas, area, theme);
}

std::vector<Hint> PhotosScreen::hints() const {
    std::vector<Hint> hints;
    if (on_add_location()) {
        return {Hint{"Cross", "Add network location"}, Hint{"Circle", "Back"}};
    }
    const int focus = grid_.focus();
    const bool in_range = focus >= 0 && focus < static_cast<int>(entries_.size());
    const bool on_folder = in_range && entries_[static_cast<std::size_t>(focus)].folder;
    const bool on_splat = in_range && entries_[static_cast<std::size_t>(focus)].splat;
    if (!entries_.empty()) {
        hints.push_back(Hint{"Cross", on_folder ? "Open" : (on_splat ? "Import" : "Select")});
    }
    if (on_captures() && !entries_.empty()) {
        hints.push_back(Hint{"Square", "Copy to Inbox"});
    } else if (on_folder && on_console()) {
        hints.push_back(Hint{"Square", "Import splats"});
    } else if (!selection_.empty()) {
        hints.push_back(Hint{"Square", "Convert (" + std::to_string(selection_.size()) + ")"});
    }
    // The turn is a file action on the focused entry, the same two keys the
    // Scenes shelf uses. A capture keeps its turn in the app (the helper owns
    // the file); a photo the app can rewrite gets its tag changed.
    if (focused_is_photo()) {
        hints.push_back(Hint{"L3", "Rotate left"});
        hints.push_back(Hint{"R3", "Rotate right"});
    }
    hints.push_back(Hint{"Circle", at_console_top() ? "Back" : "Up"});
    return hints;
}

}  // namespace splat
