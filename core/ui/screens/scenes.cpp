// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/scenes.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "gallery/thumbs.h"
#include "gallery/visibility.h"
#include "jobs/job_runner.h"
#include "render/scene_label.h"
#include "scene/scene_view.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr uint32_t kPhotoThumbEdge = 320;

bool pending_state(const std::string& state) { return state == "queued" || state == "running"; }

std::string job_badge(const JobView& job) {
    if (job.state == "queued") return "Queued";
    if (!job.message.empty()) return "Stopping";
    return "Converting";
}

bool same_folder(const Path& a, const Path& b) {
    return !a.empty() && !b.empty() && a.lexically_normal() == b.lexically_normal();
}

// The placement the screen was opened for: an album (or Unsorted) under the
// app's library, or a network location. The one rule list_scenes() applies.
bool folder_allowed(const Path& root, const std::string& album) {
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    if (!visibility->enabled) return true;
    if (vfs::is_remote(root)) return visibility->allows_location(vfs::location_id(root));
    return visibility->allows_album(album);
}

}  // namespace

ScenesScreen::ScenesScreen() {
    // The tile size is the shared thumbnail layout (core/ui/widgets.h, stage T),
    // applied from update()/draw() for the current setting and area. The caption
    // rule (two lines, focused or not) is the shared one; this screen adds the
    // scene's model as a sublabel under the focused tile.
    grid_.caption_lines = kThumbnailCaptionLines;
    grid_.sublabel_focus_only = true;  // the model, under the focused scene only
}

void ScenesScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    // on_enter runs again when a pop reveals this screen, with the arguments it
    // was pushed with; the same album and root mean "returning", so restore the
    // focus.
    const Path root = args.b.empty() ? ctx.svc.paths.scenes : Path(args.b);
    const bool returning = args.a == album_ && root == root_ && !focus_key_.empty();
    album_ = args.a;
    root_ = root;
    menu_.close();
    confirm_.close();
    // A hidden album or location is a blocked screen: no listing, no name in
    // the title, and the tiles the visitor was on are gone.
    blocked_ = !folder_allowed(root_, album_);
    if (blocked_) {
        listing_.cancel();
        items_.clear();
        pending_.clear();
        rotating_.clear();
        finished_ = 0;
        grid_.set_cells({});
        return;
    }
    // The focus rule belongs to the frame that publishes the listing: local
    // roots publish inside reload(), a location on the next update()/tick().
    restore_focus_on_apply_ = returning;
    reload(ctx);
}

void ScenesScreen::request_scenes() {
    const Path root = root_;
    const std::string album = album_;
    vfs::AsyncListing::Job job = [root, album] {
        auto listing = std::make_shared<SceneListing>();
        if (vfs::is_remote(root)) {
            // A share that is offline or has the wrong path says so; an empty
            // album is reserved for a folder that really is empty.
            Result<std::vector<SceneEntry>> scenes = list_scenes_checked(root, album);
            if (!scenes.ok()) {
                return Result<vfs::AsyncListing::Snapshot>::failure(scenes.message());
            }
            listing->scenes = std::move(scenes.value());
        } else {
            listing->scenes = list_scenes(root, album);
        }
        vfs::AsyncListing::Snapshot snapshot;
        snapshot.payload = std::move(listing);
        return Result<vfs::AsyncListing::Snapshot>::success(std::move(snapshot));
    };
    listing_.request(root, std::move(job));
}

void ScenesScreen::reload(UiContext& ctx) {
    if (blocked_) return;
    request_scenes();
    // A local folder is listed before request() returns, exactly as the direct
    // list_scenes() call was; a network location arrives through update().
    if (listing_.ready()) apply_scenes(ctx);
}

void ScenesScreen::apply_scenes(UiContext& ctx) {
    const std::shared_ptr<SceneListing> listed =
        std::static_pointer_cast<SceneListing>(listing_.payload());
    error_ = listed ? std::string() : listing_.error();
    if (!listed && !error_.empty() && ctx.svc.log) {
        ctx.svc.log("ui: scenes: " + error_);
    }
    listing_.cancel();  // consumed: ready() must not rebuild the grid again

    items_.clear();
    pending_.clear();
    rotating_.clear();
    finished_ = 0;

    if (ctx.svc.jobs != nullptr) {
        const std::vector<JobView> snapshot = ctx.svc.jobs->snapshot();
        // A relaunch must not re-announce every old turn: the first shelf entry
        // marks what the queue file already carries as seen.
        if (!ctx.svc.turns_seeded) {
            ctx.svc.turns_seeded = true;
            for (const JobView& job : snapshot) {
                if (job.kind == "rotate" && !pending_state(job.state)) {
                    ctx.svc.announced_turns.insert(job.id);
                }
            }
        }
        for (const JobView& job : snapshot) {
            if (job.kind == "rotate") {
                // A turn is drawn on its own scene's tile, never as a second
                // "converting" tile: it is the same entry, being rewritten.
                if (pending_state(job.state)) {
                    rotating_.push_back(job);
                } else {
                    // Its thumbnail on disk changed (or a failed multi-file
                    // publish left it unknown): drop the image even when this
                    // shelf did not see the job running, and say so once.
                    announce_finished_turn(ctx, job);
                }
                continue;
            }
            if (job.album != album_) continue;
            if (pending_state(job.state)) {
                pending_.push_back(job);
            } else {
                ++finished_;
            }
        }
    }
    if (listed) {
        for (const SceneEntry& scene : listed->scenes) {
            Item item;
            item.name = scene.name;
            // The model only: the user compares models; counts and timings are
            // not for the shelf.
            item.sublabel = scene_model_name(scene.meta.backend, scene.name);
            item.folder = scene.folder;
            item.placeholder = !scene.has_thumb;
            items_.push_back(std::move(item));
        }
    }

    const std::size_t tiles = 1 + pending_.size() + items_.size();
    tiles_.assign(tiles, UiImage{});
    thumb_ready_.assign(tiles, 0);
    build_cells();
    refresh_thumbs(ctx);

    if (restore_focus_on_apply_) {
        restore_focus(focus_key_);
    } else {
        grid_.set_focus(items_.empty() ? (pending_.empty() ? 0 : 1) : first_scene_cell());
    }
    restore_focus_on_apply_ = false;
    focus_key_ = focus_key();
}

void ScenesScreen::build_cells() {
    std::vector<GridCell> cells;
    cells.reserve(tiles_.size());

    GridCell add;
    add.caption = "Add photos";
    add.icon = GridCell::Icon::Add;
    cells.push_back(std::move(add));

    for (std::size_t index = 0; index < pending_.size(); ++index) {
        const JobView& job = pending_[index];
        GridCell cell;
        cell.caption = scene_folder_base(Path(job.name));
        cell.thumb = &tiles_[1 + index];
        cell.progress = job.state == "running" ? job.fraction : 0.0f;
        cell.badge = job_badge(job);
        cells.push_back(std::move(cell));
    }
    const std::size_t offset = 1 + pending_.size();
    for (std::size_t index = 0; index < items_.size(); ++index) {
        GridCell cell;
        cell.caption = items_[index].name;
        cell.sublabel = items_[index].sublabel;
        cell.thumb = &tiles_[offset + index];
        // The busy mark of a file turn: the tile is the same entry, so the
        // state is a badge on it, not a second tile with a ring.
        if (is_rotating(items_[index].folder)) cell.badge = "Rotating";
        cells.push_back(std::move(cell));
    }
    grid_.set_cells(std::move(cells));
}

void ScenesScreen::refresh_thumbs(UiContext& ctx) {
    bool arrived = false;
    const std::size_t offset = 1 + pending_.size();
    for (std::size_t tile = 1; tile < tiles_.size(); ++tile) {
        if (thumb_ready_[tile] != 0) continue;
        std::string key;
        ThumbCache::Loader loader;
        if (tile < offset) {
            const Path photo = pending_[tile - 1].photo;
            if (photo.empty()) continue;
            key = "photo:" + photo.string();
            loader = [photo] { return load_photo_thumb(photo, kPhotoThumbEdge); };
        } else {
            const Item& item = items_[tile - offset];
            if (item.placeholder) continue;
            const Path folder = item.folder;
            key = "scene:" + folder.string();
            loader = [folder] { return load_scene_thumb(folder); };
        }
        thumbs_.request(key, std::move(loader));
        const UiImage* image = thumbs_.get(key);
        if (image == nullptr || image->empty()) continue;

        tiles_[tile] = *image;  // same vector slot: the cell's pointer stays valid
        thumb_ready_[tile] = 1;
        arrived = true;
    }
    if (arrived) ctx.invalidate();
}

bool ScenesScreen::is_rotating(const Path& folder) const {
    for (const JobView& job : rotating_) {
        if (same_folder(job.photo, folder)) return true;
    }
    return false;
}

void ScenesScreen::announce_finished_turn(UiContext& ctx, const JobView& job) {
    if (job.kind != "rotate") return;
    if (!ctx.svc.announced_turns.insert(job.id).second) return;
    // The scene files changed (a done turn) or are in an unknown state (a failed
    // multi-file publish): the cached image can no longer be trusted either way.
    thumbs_.forget("scene:" + job.photo.string());
    if (job.state == "failed") {
        ctx.toast("Could not rotate " + job.name +
                  (job.message.empty() ? std::string() : ": " + job.message));
    } else if (job.state == "done") {
        ctx.toast("Rotated " + job.name);
    }
}

void ScenesScreen::rotate_scene(UiContext& ctx, int scene, int quarters) {
    if (scene < 0 || scene >= static_cast<int>(items_.size())) return;
    const Item& item = items_[static_cast<std::size_t>(scene)];
    // The renderer holds the version it read: rewriting the files under it
    // would leave the picture and the file disagreeing. The same rule covers a
    // scene with a turn already queued or in flight.
    if (!ctx.svc.open_scene.empty() && same_folder(ctx.svc.open_scene, item.folder)) {
        ctx.toast("Close the scene before rotating it");
        return;
    }
    if (is_rotating(item.folder)) {
        ctx.toast("This scene is already being rotated");
        return;
    }
    if (ctx.svc.jobs == nullptr) {
        ctx.toast("Rotating a scene is not available in this build");
        return;
    }
    Status status = ctx.svc.jobs->enqueue_rotate(item.folder, quarters, album_);
    if (!status.ok()) {
        ctx.toast(status.message());
        return;
    }
    ctx.toast("Rotating " + item.name + "\xE2\x80\xA6");
    // Re-list now: the job snapshot carries the new row's id and state, so the
    // tile's busy mark appears this frame and the completion is noticed next
    // time. A local folder applies its listing inside reload().
    const std::string key = focus_key();
    reload(ctx);
    restore_focus(key);
    ctx.invalidate();
}

bool ScenesScreen::sync_jobs(UiContext& ctx) {
    if (ctx.svc.jobs == nullptr) return false;
    std::vector<JobView> pending;
    std::vector<JobView> rotating;
    std::size_t finished = 0;
    std::vector<JobView> snapshot = ctx.svc.jobs->snapshot();
    for (const JobView& job : snapshot) {
        if (job.kind == "rotate") {
            if (pending_state(job.state)) rotating.push_back(job);
            continue;
        }
        if (job.album != album_) continue;
        if (pending_state(job.state)) {
            pending.push_back(job);
        } else {
            ++finished;
        }
    }

    bool same = pending.size() == pending_.size() && rotating.size() == rotating_.size() &&
                finished == finished_;
    for (std::size_t index = 0; same && index < pending.size(); ++index) {
        same = pending[index].id == pending_[index].id;
    }
    for (std::size_t index = 0; same && index < rotating.size(); ++index) {
        same = rotating[index].id == rotating_[index].id;
    }
    if (!same) {
        // A conversion that left the pending set as failed says so once.
        for (const JobView& before : pending_) {
            for (const JobView& now : snapshot) {
                if (now.id != before.id || now.state != "failed") continue;
                ctx.toast("Could not convert " + now.name +
                          (now.message.empty() ? std::string() : ": " + now.message));
            }
        }
        // A finished turn says so once too, and its thumbnail is stale: the
        // scene's own file changed on disk.
        for (const JobView& before : rotating_) {
            for (const JobView& now : snapshot) {
                if (now.id != before.id) continue;
                if (pending_state(now.state)) continue;
                announce_finished_turn(ctx, now);
            }
        }
        return true;
    }

    // Same jobs: move the rings in place.
    for (std::size_t index = 0; index < pending.size(); ++index) {
        pending_[index] = pending[index];
        if (GridCell* cell = grid_.cell(static_cast<int>(1 + index))) {
            cell->progress = pending[index].state == "running" ? pending[index].fraction : 0.0f;
            cell->badge = job_badge(pending[index]);
        }
    }
    rotating_ = std::move(rotating);
    return false;
}

std::string ScenesScreen::focus_key() const {
    const int focus = grid_.focus();
    if (focus <= 0) return "add";
    if (focus < first_scene_cell()) return "job:" + pending_[static_cast<std::size_t>(focus - 1)].id;
    const int scene = focus - first_scene_cell();
    if (scene < static_cast<int>(items_.size())) {
        return "scene:" + items_[static_cast<std::size_t>(scene)].folder.string();
    }
    return "add";
}

void ScenesScreen::restore_focus(const std::string& key) {
    const int previous = grid_.focus();
    const int count = static_cast<int>(grid_.cells().size());
    for (int index = 0; index < count; ++index) {
        grid_.set_focus(index);
        if (focus_key() == key) return;
    }
    // The tile is gone (a conversion finished): stay at the same place.
    grid_.set_focus(std::min(previous, count - 1));
}

int ScenesScreen::focused_scene() const {
    const int scene = grid_.focus() - first_scene_cell();
    return scene >= 0 && scene < static_cast<int>(items_.size()) ? scene : -1;
}

void ScenesScreen::update(UiContext& ctx, const UiInput& input) {
    listing_.tick();
    if (listing_.ready()) {
        // A location publishes here, not inside reload(): the grid the focus key
        // is read from is still the *previous* one. On the first listing it is
        // empty (focus_key() says "add"), so restoring it would leave the focus
        // on "+" instead of the first scene apply_scenes() just picked - which
        // is what a network library showed before this was fixed. A re-list
        // (a conversion finished) keeps the tile the user was on.
        const bool had_cells = !grid_.cells().empty();
        const bool revealing = restore_focus_on_apply_;
        const std::string key = focus_key();
        apply_scenes(ctx);
        if (!revealing && had_cells) restore_focus(key);
        ctx.invalidate();
    }
    if (ctx.content.empty() || listing_.busy()) return;
    if (blocked_) return;
    if (sync_jobs(ctx)) {
        const std::string key = focus_key();
        reload(ctx);
        restore_focus(key);
        ctx.invalidate();
    }
    refresh_thumbs(ctx);

    const int menu_scene = focused_scene();
    if (confirm_.is_open()) {
        if (confirm_.update(input, ctx.content) == ConfirmCard::Choice::Confirm && menu_scene >= 0) {
            const Item& item = items_[static_cast<std::size_t>(menu_scene)];
            // The root the screen was opened with: a network library answers
            // its own refusal instead of "not a scene folder".
            const Status status = delete_scene(root_, item.folder);
            ctx.toast(status.ok() ? "Deleted " + item.name : status.message());
            const std::string key = focus_key();
            reload(ctx);
            restore_focus(key);
            ctx.invalidate();
        }
        return;
    }
    if (menu_.is_open()) {
        const int chosen = menu_.update(input, ctx.content, ctx.theme);
        if (chosen < 0 || menu_scene < 0) return;
        const Item& item = items_[static_cast<std::size_t>(menu_scene)];
        ScreenArgs args;
        args.b = item.folder.string();
        switch (chosen) {
            case 0:
                args.a = "move-scene";
                ctx.nav.push(ScreenId::AlbumPick, args);
                break;
            case 1:
                args.a = "rename-scene";
                ctx.nav.push(ScreenId::Keyboard, args);
                break;
            case 2:
                // Cancel has the focus first, as on the console.
                confirm_.open("Delete \u201C" + item.name + "\u201D?",
                              "The scene is removed from this console.", "Delete");
                break;
            default: break;
        }
        return;
    }

    apply_thumbnail_layout(grid_, ctx.content, ctx.theme, ctx.thumbnail_size());
    grid_.update(input, ctx.content, ctx.theme);
    focus_key_ = focus_key();
    const int focus = grid_.focus();
    const int scene = focused_scene();

    if ((input.pressed & Context) != 0) {
        grid_.clear_activated();
        // Nothing on a share can be moved, renamed or deleted (L3 keeps the
        // mutations local): say so instead of opening a menu whose every entry
        // would fail - and whose rename would test an "smb0:/..." string as an
        // OS path.
        if (vfs::is_remote(root_)) {
            ctx.toast(kRemoteMutationRefusal);
            return;
        }
        if (scene >= 0) {
            menu_.open(items_[static_cast<std::size_t>(scene)].name,
                       {"Move to folder", "Rename", "Delete"});
        }
        return;
    }
    if ((input.pressed & (RotateLeft | RotateRight)) != 0) {
        grid_.clear_activated();
        const int target = focused_scene();
        if (target >= 0) {
            rotate_scene(ctx, target, (input.pressed & RotateRight) != 0 ? 1 : 3);
        }
        return;
    }
    if ((input.pressed & Secondary) != 0) {
        // Square is not an action on a scene row any more: the turn moved to the
        // file action on L3/R3, for every entry (DECISIONS.md, 2026-11-05).
        return;
    }
    if (!grid_.activated()) return;
    grid_.clear_activated();

    if (focus == 0) {
        ScreenArgs args;
        args.a = album_;
        // A library opened from a network location keeps its root: the picker
        // and the convert dialog stage the scene locally and upload it there.
        args.b = root_.string();
        ctx.nav.push(ScreenId::Photos, args);
        return;
    }
    if (scene < 0) {
        const JobView& job = pending_[static_cast<std::size_t>(focus - 1)];
        const int percent = static_cast<int>(job.fraction * 100.0f + 0.5f);
        ctx.toast(job.state == "queued"
                      ? job.name + " is waiting for its turn"
                      : job.name + ": converting " + std::to_string(percent) + "%");
        return;
    }
    if (is_rotating(items_[static_cast<std::size_t>(scene)].folder)) {
        // The turn is queued behind another job and will rewrite the folder
        // while the renderer would be holding it. The task's rule is the same
        // one the enqueue side applies, from the other direction.
        ctx.toast("This scene is being rotated");
        return;
    }
    // The policy is checked again at the moment of the open: a stale tile (or a
    // passcode set while this screen stayed in the stack) must not open a scene
    // that is hidden now.
    if (!current_visibility()->allows_scene_folder(
            items_[static_cast<std::size_t>(scene)].folder)) {
        ctx.toast("That scene is no longer visible");
        return;
    }
    UiCommand command;
    if (vfs::is_remote(root_)) {
        // A scene on a network location is opened from where it lies: the host
        // reads it through the VFS, no copy into the local library.
        command.kind = UiCommand::OpenPath;
        command.path = scene_files(items_[static_cast<std::size_t>(scene)].folder).splat;
    } else {
        command.kind = UiCommand::ViewScene;
        command.album = album_;
        command.index = scene;
    }
    ctx.command(std::move(command));
}

void ScenesScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    if (blocked_) {
        // No name: the visitor must not learn which folder exists and is hidden.
        const int size = ctx.theme.heading_size;
        canvas.text(content.x + 4, content.y + 40, kNotVisibleText, size, ctx.theme.text);
        return;
    }
    if (listing_.busy()) {
        canvas.text(content.x + 4, content.y + 40, "Loading\xE2\x80\xA6", ctx.theme.heading_size,
                    ctx.theme.text);
        return;
    }
    if (!error_.empty()) {
        // A listing that failed (an offline share, a removed location) says
        // why, instead of the empty-folder hint the grid would otherwise show.
        const int size = ctx.theme.heading_size;
        canvas.text(content.x + 4, content.y + 40, fit_text(error_, size, content.w - 8), size,
                    ctx.theme.text);
        return;
    }
    apply_thumbnail_layout(grid_, content, ctx.theme, ctx.thumbnail_size());
    grid_.draw(canvas, content, ctx.theme);
    if (items_.empty() && pending_.empty()) {
        // Next to the lone "+" tile: what it is for.
        const Rect image = grid_.image_rect(grid_.cell_rect(0, content), ctx.theme);
        const int x = image.right() + grid_.column_gap + 8;
        const int y = image.y + image.h / 2 - UiCanvas::line_height(ctx.theme.heading_size);
        const bool restricted = current_visibility()->enabled;
        canvas.text(x, y, restricted ? "No visible scenes in this folder" : "This folder is empty",
                    ctx.theme.heading_size, ctx.theme.text);
        canvas.text(x, y + UiCanvas::line_height(ctx.theme.heading_size) + 2,
                    restricted ? "The rest of this folder is hidden."
                               : "Choose + to turn photos into scenes.",
                    ctx.theme.body_size - 2, ctx.theme.dim_text);
    }
    menu_.draw(canvas, content, ctx.theme);
    if (confirm_.is_open()) {
        canvas.fill_rect(0, 0, static_cast<int>(canvas.width()), static_cast<int>(canvas.height()),
                         ctx.theme.veil);
        confirm_.draw(canvas, content, ctx.theme);
    }
}

bool ScenesScreen::handle_back(UiContext& /*ctx*/) {
    if (confirm_.is_open()) {
        confirm_.close();
        return true;
    }
    if (!menu_.is_open()) return false;
    menu_.close();
    return true;
}

std::vector<Hint> ScenesScreen::hints() const {
    if (blocked_) return {Hint{"Circle", "Back"}};
    if (confirm_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Cancel"}};
    if (menu_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Close"}};
    if (grid_.focus() == 0) return {Hint{"Cross", "Add photos"}, Hint{"Circle", "Back"}};
    if (focused_scene() < 0) return {Hint{"Circle", "Back"}};
    // The turn is the same file action as on a photo entry: a scene of any
    // origin (converted or imported, local or on a network location) turns on
    // L3/R3. It is a background job, so while one runs the tile says "Rotating".
    return {Hint{"Cross", "View"}, Hint{"Triangle", "More"}, Hint{"L3", "Rotate left"},
            Hint{"R3", "Rotate right"}, Hint{"Circle", "Back"}};
}

std::string ScenesScreen::title() const {
    // A hidden folder keeps its name out of the title bar.
    if (blocked_) return "Folder";
    return album_.empty() ? std::string(kAllScenesAlbumName) : album_;
}

std::vector<std::string> ScenesScreen::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const Item& item : items_) out.push_back(item.name);
    return out;
}

}  // namespace splat
