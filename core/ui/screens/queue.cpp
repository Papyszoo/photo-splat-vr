// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/queue.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "gallery/gallery.h"
#include "jobs/job_runner.h"
#include "render/scene_label.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

std::string percent_text(const JobView& job) {
    char buffer[16] = {};
    const int percent = static_cast<int>(job.fraction * 100.0f + 0.5f);
    std::snprintf(buffer, sizeof(buffer), "%d%%", std::clamp(percent, 0, 100));
    return buffer;
}

// The right-hand column in plain words: "Converting 50%", "Queued", "Done",
// "Failed", "Cancelled". Pipeline stages stay out of the panel.
std::string right_text(const JobView& job) {
    if (job.state == "running") {
        // A pending cancel matters more than the progress: the job is still
        // spending GPU time on the photo it already started.
        if (!job.message.empty()) return "Stopping after this photo";
        return "Converting " + percent_text(job);
    }
    if (job.state == "done") return "Done";
    if (job.state == "failed") return "Failed";
    if (job.state == "cancelled") return "Cancelled";
    return "Queued";
}

// "SHARP  .  Krakow trip"; a failed job shows its reason instead.
std::string sublabel_text(const JobView& job) {
    if (job.state == "failed" && !job.message.empty()) return job.message;
    std::string text = scene_model_name(job.backend, std::string());
    text += "  \xC2\xB7  ";
    text += job.album.empty() ? std::string(kAllScenesAlbumName) : job.album;
    return text;
}

bool suffix_digits(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

// The scene a done job wrote, by the folder name the runner gives it
// (jobs/job_runner.h scene_folder_base) and its -2, -3, ... collision names.
// The highest suffix wins: it is the conversion that ran last.
int scene_index_for(const std::string& photo_name, const std::vector<SceneEntry>& scenes) {
    const std::string base = scene_folder_base(Path(photo_name));
    int best = -1;
    int best_suffix = 0;
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        const std::string& name = scenes[index].name;
        int suffix = 0;
        if (name == base) {
            suffix = 1;
        } else if (name.size() > base.size() + 1 && name.compare(0, base.size(), base) == 0 &&
                   name[base.size()] == '-') {
            const std::string rest = name.substr(base.size() + 1);
            if (!suffix_digits(rest)) continue;
            suffix = std::atoi(rest.c_str());
            if (suffix < 2) continue;
        } else {
            continue;
        }
        if (suffix > best_suffix) {
            best_suffix = suffix;
            best = static_cast<int>(index);
        }
    }
    return best;
}

}  // namespace

void QueueScreen::on_enter(UiContext& ctx, const ScreenArgs& /*args*/) {
    redraw_ = 0.0;
    list_.set_focus(0);
    refresh(ctx);
}

void QueueScreen::refresh(UiContext& ctx) {
    jobs_ = ctx.svc.jobs != nullptr ? ctx.svc.jobs->snapshot() : std::vector<JobView>{};

    const int right_limit = std::max(120, ctx.content.w / 2);
    const int right_size = ctx.theme.small_size;
    std::vector<ListRow> rows;
    rows.reserve(jobs_.size());
    for (const JobView& job : jobs_) {
        ListRow row;
        row.label = job.name;
        row.sublabel = sublabel_text(job);
        row.right = fit_text(right_text(job), right_size, right_limit);
        rows.push_back(std::move(row));
    }
    list_.set_rows(std::move(rows));
}

int QueueScreen::running_index() const {
    for (std::size_t index = 0; index < jobs_.size(); ++index) {
        if (jobs_[index].state == "running") return static_cast<int>(index);
    }
    return -1;
}

void QueueScreen::update(UiContext& ctx, const UiInput& input) {
    refresh(ctx);

    // About four repaints a second while a job runs: the bar and the stage move,
    // but an overlay upload per frame would be waste.
    if (running_index() >= 0) {
        redraw_ += input.dt;
        if (redraw_ >= kRedrawSeconds) {
            redraw_ = 0.0;
            ctx.invalidate();
        }
    } else {
        redraw_ = 0.0;
    }

    list_.update(input, ctx.content, ctx.theme);

    if ((input.pressed & Secondary) != 0 && ctx.svc.jobs != nullptr) {
        list_.clear_activated();
        cancel_focused(ctx);
        return;
    }
    if (!list_.activated()) return;
    list_.clear_activated();
    view_focused(ctx);
}

void QueueScreen::cancel_focused(UiContext& ctx) {
    const int focused = list_.focus();
    if (focused < 0 || focused >= static_cast<int>(jobs_.size())) return;
    const JobView& job = jobs_[static_cast<std::size_t>(focused)];
    if (job.state != "queued" && job.state != "running") {
        ctx.toast("already " + job.state);
        return;
    }

    const Status cancelled = ctx.svc.jobs->cancel(job.id);
    if (!cancelled.ok()) {
        ctx.toast(cancelled.message());
        return;
    }
    ctx.toast(job.state == "running" ? "Cancels after this photo" : "Cancelled");
    refresh(ctx);
}

void QueueScreen::view_focused(UiContext& ctx) {
    const int focused = list_.focus();
    if (focused < 0 || focused >= static_cast<int>(jobs_.size())) return;
    const JobView& job = jobs_[static_cast<std::size_t>(focused)];
    if (job.state != "done") return;

    const std::vector<SceneEntry> scenes = list_scenes(ctx.svc.paths.scenes, job.album);
    const int index = scene_index_for(job.name, scenes);
    if (index < 0) {
        ctx.toast("scene is not there yet");
        return;
    }
    ctx.command(UiCommand{UiCommand::ViewScene, job.album, index});
}

void QueueScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    if (jobs_.empty()) {
        const std::string text = ctx.svc.jobs == nullptr ? "Conversion is not available on this build"
                                                         : "No conversions yet";
        canvas.text(content.x, content.y + 8, text, ctx.theme.heading_size, ctx.theme.text);
        if (ctx.svc.jobs != nullptr) {
            canvas.text(content.x, content.y + 8 + UiCanvas::line_height(ctx.theme.heading_size),
                        "Open a folder and choose + to convert photos.", ctx.theme.body_size - 2,
                        ctx.theme.dim_text);
        }
        return;
    }

    list_.draw(canvas, content, ctx.theme);

    const int running = running_index();
    if (running < 0) return;
    const int row = running - list_.scroll();
    if (row < 0 || row >= list_.visible_rows(content, ctx.theme)) return;
    draw_row_progress(canvas, content, row, list_.rows()[static_cast<std::size_t>(running)].right,
                      jobs_[static_cast<std::size_t>(running)].fraction,
                      static_cast<int>(jobs_.size()) > list_.visible_rows(content, ctx.theme),
                      ctx.theme);
}

std::vector<Hint> QueueScreen::hints() const {
    const int focus = list_.focus();
    if (focus < 0 || focus >= static_cast<int>(jobs_.size())) return {Hint{"Circle", "Back"}};
    const JobView& job = jobs_[static_cast<std::size_t>(focus)];
    std::vector<Hint> hints;
    if (job.state == "done") hints.push_back(Hint{"Cross", "View"});
    if (job.state == "queued" || job.state == "running") hints.push_back(Hint{"Square", "Cancel"});
    hints.push_back(Hint{"Circle", "Back"});
    return hints;
}

}  // namespace splat
