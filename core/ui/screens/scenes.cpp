// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/scenes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gallery/gallery.h"
#include "gallery/thumbs.h"
#include "jobs/job_runner.h"
#include "render/scene_label.h"
#include "ui/screen_stack.h"

namespace splat {
namespace {

constexpr int kCellWidth = 264;
constexpr int kCellHeight = 250;  // image + room for a 3-line focused name
constexpr int kCellGap = 32;
constexpr uint32_t kPhotoThumbEdge = 320;

bool pending_state(const std::string& state) { return state == "queued" || state == "running"; }

std::string job_badge(const JobView& job) {
    if (job.state == "queued") return "Queued";
    if (!job.message.empty()) return "Stopping";
    return "Converting";
}

}  // namespace

ScenesScreen::ScenesScreen() {
    grid_.cell_width = kCellWidth;
    grid_.cell_height = kCellHeight;
    grid_.gap = kCellGap;
    grid_.caption_lines = 3;
    grid_.sublabel_focus_only = true;  // the model, under the focused scene only
}

void ScenesScreen::on_enter(UiContext& ctx, const ScreenArgs& args) {
    // on_enter runs again when a pop reveals this screen, with the arguments it
    // was pushed with; the same album means "returning", so restore the focus.
    const bool returning = args.a == album_ && !focus_key_.empty();
    album_ = args.a;
    menu_.close();
    confirm_.close();
    reload(ctx);
    if (returning) {
        restore_focus(focus_key_);
    } else {
        grid_.set_focus(items_.empty() ? (pending_.empty() ? 0 : 1) : first_scene_cell());
    }
    focus_key_ = focus_key();
}

void ScenesScreen::reload(UiContext& ctx) {
    items_.clear();
    pending_.clear();
    finished_ = 0;

    for (const SceneEntry& scene : list_scenes(ctx.svc.paths.scenes, album_)) {
        Item item;
        item.name = scene.name;
        // The model only: the user compares models; counts and timings are
        // not for the shelf.
        item.sublabel = scene_model_name(scene.meta.backend, scene.name);
        item.folder = scene.folder;
        item.placeholder = !scene.has_thumb;
        items_.push_back(std::move(item));
    }
    if (ctx.svc.jobs != nullptr) {
        for (const JobView& job : ctx.svc.jobs->snapshot()) {
            if (job.album != album_) continue;
            if (pending_state(job.state)) {
                pending_.push_back(job);
            } else {
                ++finished_;
            }
        }
    }

    const std::size_t tiles = 1 + pending_.size() + items_.size();
    tiles_.assign(tiles, UiImage{});
    thumb_ready_.assign(tiles, 0);
    build_cells();
    refresh_thumbs(ctx);
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

bool ScenesScreen::sync_jobs(UiContext& ctx) {
    if (ctx.svc.jobs == nullptr) return false;
    std::vector<JobView> pending;
    std::size_t finished = 0;
    std::vector<JobView> snapshot = ctx.svc.jobs->snapshot();
    for (const JobView& job : snapshot) {
        if (job.album != album_) continue;
        if (pending_state(job.state)) {
            pending.push_back(job);
        } else {
            ++finished;
        }
    }

    bool same = pending.size() == pending_.size() && finished == finished_;
    for (std::size_t index = 0; same && index < pending.size(); ++index) {
        same = pending[index].id == pending_[index].id;
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
    if (ctx.content.empty()) return;
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
            const Status status = delete_scene(ctx.svc.paths.scenes, item.folder);
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

    grid_.update(input, ctx.content, ctx.theme);
    focus_key_ = focus_key();
    const int focus = grid_.focus();
    const int scene = focused_scene();

    if ((input.pressed & Context) != 0) {
        grid_.clear_activated();
        if (scene >= 0) {
            menu_.open(items_[static_cast<std::size_t>(scene)].name,
                       {"Move to folder", "Rename", "Delete"});
        }
        return;
    }
    if (!grid_.activated()) return;
    grid_.clear_activated();

    if (focus == 0) {
        ScreenArgs args;
        args.a = album_;
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
    UiCommand command;
    command.kind = UiCommand::ViewScene;
    command.album = album_;
    command.index = scene;
    ctx.command(std::move(command));
}

void ScenesScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    grid_.draw(canvas, content, ctx.theme);
    if (items_.empty() && pending_.empty()) {
        // Next to the lone "+" tile: what it is for.
        const Rect image = grid_.image_rect(grid_.cell_rect(0, content), ctx.theme);
        const int x = image.right() + kCellGap + 8;
        const int y = image.y + image.h / 2 - UiCanvas::line_height(ctx.theme.heading_size);
        canvas.text(x, y, "This folder is empty", ctx.theme.heading_size, ctx.theme.text);
        canvas.text(x, y + UiCanvas::line_height(ctx.theme.heading_size) + 2,
                    "Choose + to turn photos into scenes.", ctx.theme.body_size - 2,
                    ctx.theme.dim_text);
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
    if (confirm_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Cancel"}};
    if (menu_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Close"}};
    if (grid_.focus() == 0) return {Hint{"Cross", "Add photos"}, Hint{"Circle", "Back"}};
    if (focused_scene() < 0) return {Hint{"Circle", "Back"}};
    return {Hint{"Cross", "View"}, Hint{"Triangle", "More"}, Hint{"Circle", "Back"}};
}

std::string ScenesScreen::title() const {
    return album_.empty() ? std::string(kAllScenesAlbumName) : album_;
}

std::vector<std::string> ScenesScreen::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const Item& item : items_) out.push_back(item.name);
    return out;
}

}  // namespace splat
