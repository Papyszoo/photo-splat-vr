// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/photos.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "gallery/photo_sources.h"
#include "gallery/thumbs.h"
#include "ui/button_glyph.h"
#include "ui/capture_service.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

constexpr int kTabHeight = 40;
constexpr int kTabGap = 8;
constexpr int kBadgeHeight = 32;  // the L1/R1 badges beside the tabs
constexpr int kLocationTop = 54;
constexpr int kGridTop = 96;
constexpr int kCellWidth = 172;
constexpr int kCellHeight = 190;  // a 4:3 photo + two caption lines for the focused one
constexpr int kCellGap = 24;
constexpr int kThumbEdge = 256;
constexpr const char* kCrumb = " \xE2\x80\xBA ";  // " > " as a single angle quote

bool same_path(const Path& a, const Path& b) {
    return !a.empty() && !b.empty() && a.lexically_normal() == b.lexically_normal();
}

}  // namespace

void PhotosScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    // A reveal (back from the model choice) keeps the tab, the folder and the
    // basket; a fresh push starts at the Inbox.
    const bool returning = !tabs_.empty() && args.a == album_;
    album_ = args.a;
    if (!returning) {
        selection_.clear();
        tab_ = 0;
        tabs_.clear();
        console_dir_.clear();
    }
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

    SourceRoots roots;
    roots.inbox = ctx.svc.paths.inbox;
    roots.mnt = ctx.svc.paths.mnt;
    roots.data = ctx.svc.paths.data;
    console_roots_ = console_roots(roots);
    data_root_ = ctx.svc.paths.data;
    // The app's own folder (inbox, state, scenes) is not a place to pick from.
    hidden_dir_ = ctx.svc.paths.inbox.parent_path();

    // Always the same four sources, in this order, each with its own empty
    // state: Inbox, Console, USB (one tab per stick when there are several),
    // Captures.
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
        tab.count = list_photos(source).size();
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
    for (PhotoTab& tab : tabs) tab.label = tab.title;

    tabs_ = std::move(tabs);
    tab_ = 0;
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        if (tabs_[index].id != keep) continue;
        tab_ = static_cast<int>(index);
        break;
    }
    // The basket belonged to the tab that is gone.
    if (tabs_[static_cast<std::size_t>(tab_)].id != keep) {
        selection_.clear();
        console_dir_.clear();
    }
    load_tab(ctx);
}

void PhotosScreen::set_tab(UiContext& ctx, int index) {
    const int count = static_cast<int>(tabs_.size());
    const int clamped = count == 0 ? 0 : std::max(0, std::min(index, count - 1));
    if (clamped != tab_) {
        selection_.clear();
        console_dir_.clear();
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
    if (tabs_.empty()) return;

    const PhotoTab& tab = tabs_[static_cast<std::size_t>(tab_)];
    if (tab.console) {
        load_console();
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
            entries_.push_back(
                Entry{capture.path, capture.name, capture.bytes, false, capture.supported});
        }
        if (entries_.empty()) message_ = "No captures found";
        return;
    }
    if (tab.removable && tab.dir.empty()) {
        message_ = "No USB drive found";
        return;
    }

    for (const PhotoEntry& photo :
         list_photos(PhotoSource{tab.id, tab.title, tab.dir, tab.removable})) {
        entries_.push_back(Entry{photo.path, photo.name, photo.size, false, true});
    }
    if (!entries_.empty()) return;
    message_ = tab.removable ? "No photos on this USB drive" : "No photos in the Inbox yet";
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

void PhotosScreen::load_console() {
    // One root (the usual console): start inside it; several: list them.
    if (console_dir_.empty() && console_roots_.size() == 1) console_dir_ = console_roots_[0].dir;
    if (console_dir_.empty()) {
        for (std::size_t index = 0; index < console_roots_.size(); ++index) {
            entries_.push_back(Entry{console_roots_[index].dir, root_label(index), 0, true, true});
        }
        if (entries_.empty()) message_ = "No readable storage found";
        return;
    }
    for (const FolderEntry& item : list_folder(console_dir_)) {
        if (item.folder && same_path(item.path, hidden_dir_)) continue;
        entries_.push_back(Entry{item.path, item.name, item.size, item.folder, true});
    }
    if (entries_.empty()) message_ = "No folders or photos here";
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
    if (!on_console() || console_dir_.empty()) return false;
    const bool at_root = root_index(console_dir_) >= 0;
    // The only root is where the browser starts: Circle leaves the picker.
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
    if (tab.captures) return "Screenshots from this console";
    if (tab.removable) return tab.dir.empty() ? std::string() : "Photos on the USB drive";
    return "Upload photos by FTP to " + display_path(tab.dir);
}

Rect PhotosScreen::grid_area(const Rect& content) const {
    return Rect{content.x, content.y + kGridTop, content.w, content.h - kGridTop};
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
        if (entry.folder) {
            cell.icon = GridCell::Icon::Folder;
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
        // Same tiles: only the selection marks may have changed.
        for (std::size_t index = 0; index < count; ++index) {
            if (GridCell* cell = grid_.cell(static_cast<int>(index))) {
                cell->checked = cells[index].checked;
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

std::vector<Rect> PhotosScreen::tab_rects(const Rect& content, const UiTheme& theme) const {
    std::vector<Rect> rects;
    int x = content.x + button_glyph_width("L1", kBadgeHeight) + 14;
    const int tab_size = theme.body_size - 2;
    for (const PhotoTab& tab : tabs_) {
        const int width = static_cast<int>(UiCanvas::text_width(tab.title, tab_size)) + 36;
        rects.push_back(Rect{x, content.y, width, kTabHeight});
        x += width + kTabGap;
    }
    return rects;
}

void PhotosScreen::update(UiContext& ctx, const UiInput& input) {
    // The Sense pointer (or a mouse) picks a source by clicking its tab.
    if (input.pointer_valid && input.click) {
        const std::vector<Rect> rects = tab_rects(ctx.content, ctx.theme);
        for (std::size_t index = 0; index < rects.size(); ++index) {
            if (!rects[index].contains(input.px, input.py)) continue;
            refresh(ctx);
            set_tab(ctx, static_cast<int>(index));
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

    grid_.cell_width = kCellWidth;
    grid_.cell_height = kCellHeight;
    grid_.gap = kCellGap;
    grid_.caption_lines = 2;
    grid_.show_sublabel = false;
    const Rect area = grid_area(ctx.content);
    sync_cells(ctx, area);
    grid_.update(input, area, ctx.theme);

    if (grid_.activated()) {
        grid_.clear_activated();
        const int index = grid_.focus();
        if (index >= 0 && index < static_cast<int>(entries_.size())) {
            const Entry& entry = entries_[static_cast<std::size_t>(index)];
            if (entry.folder) {
                enter_folder(ctx, entry.path);
                sync_cells(ctx, area);
            } else if (!entry.supported) {
                ctx.toast(entry.name + " is HDR, not supported");
            } else {
                toggle(entry.path);
                sync_cells(ctx, area);
            }
        }
    }

    if ((input.pressed & Secondary) != 0) secondary(ctx);
    if (copying_) poll_copy(ctx);
    if (thumbs_.pending()) ctx.invalidate();
}

void PhotosScreen::toggle(const Path& path) {
    const auto found = std::find(selection_.begin(), selection_.end(), path);
    if (found != selection_.end()) {
        selection_.erase(found);
    } else {
        selection_.push_back(path);
    }
}

bool PhotosScreen::selected(const Path& path) const {
    return std::find(selection_.begin(), selection_.end(), path) != selection_.end();
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
        const Status started = ctx.svc.captures->copy_to_inbox(selection_);
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

    if (selection_.empty()) {
        ctx.toast("Select photos with Cross first");
        return;
    }
    ScreenArgs args;
    args.a = album_;
    args.list.reserve(selection_.size());
    for (const Path& path : selection_) args.list.push_back(path.string());
    ctx.nav.push(ScreenId::ConvertDialog, args);
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

    // Source tabs as pills between the L1 and R1 badges.
    const int cy = content.y + kTabHeight / 2;
    const Rgba badge{255, 255, 255, 46};
    draw_button_glyph(canvas, content.x, cy, "L1", kBadgeHeight, badge, theme.text);
    const int tab_size = theme.body_size - 2;
    const std::vector<Rect> rects = tab_rects(content, theme);
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        const bool active = static_cast<int>(index) == tab_;
        const Rect& box = rects[index];
        if (active) canvas.fill_round_rect(box.x, box.y, box.w, box.h, box.h / 2, theme.light);
        canvas.text(box.x + 18, UiCanvas::text_top_centered(box.y, box.h, tab_size),
                    tabs_[index].title, tab_size, active ? theme.background_bottom : theme.dim_text);
    }
    const int after = rects.empty() ? content.x + 60 : rects.back().right() + kTabGap;
    draw_button_glyph(canvas, after + 6, cy, "R1", kBadgeHeight, badge, theme.text);

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
    canvas.text(content.x + 4, content.y + kLocationTop,
                fit_text(where, theme.small_size, content.w - 8), theme.small_size, theme.dim_text);

    const Rect area = grid_area(content);
    if (!message_.empty()) {
        canvas.text(area.x + 4, area.y + 40, message_, theme.heading_size, theme.text);
        canvas.text(area.x + 4, area.y + 40 + UiCanvas::line_height(theme.heading_size),
                    "L1 and R1 switch to the other sources.", theme.body_size - 2, theme.dim_text);
        return;
    }
    grid_.draw(canvas, area, theme);
}

std::vector<Hint> PhotosScreen::hints() const {
    std::vector<Hint> hints;
    const int focus = grid_.focus();
    const bool on_folder = focus >= 0 && focus < static_cast<int>(entries_.size()) &&
                           entries_[static_cast<std::size_t>(focus)].folder;
    if (!entries_.empty()) hints.push_back(Hint{"Cross", on_folder ? "Open" : "Select"});
    if (on_captures() && !entries_.empty()) {
        hints.push_back(Hint{"Square", "Copy to Inbox"});
    } else if (!selection_.empty()) {
        hints.push_back(Hint{"Square", "Convert (" + std::to_string(selection_.size()) + ")"});
    }
    hints.push_back(Hint{"Circle", at_console_top() ? "Back" : "Up"});
    return hints;
}

}  // namespace splat
