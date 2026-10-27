// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/convert_dialog.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>

#include "gallery/gallery.h"
#include "gallery/thumbs.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

constexpr int kPreviewWidth = 360;
constexpr int kPreviewHeight = 270;
constexpr int kColumnGap = 56;

std::string photo_count(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " photo" : " photos");
}

std::string seconds_hint(double seconds) {
    if (seconds <= 0.0) return {};
    char text[32];
    std::snprintf(text, sizeof(text), "about %.0f s per photo", std::max(1.0, seconds));
    return text;
}

}  // namespace

void ConvertDialog::on_enter(UiContext& ctx, const ScreenArgs& args) {
    photos_.clear();
    photos_.reserve(args.list.size());
    for (const std::string& photo : args.list) photos_.push_back(Path(photo));
    album_ = args.a;
    jobs_available_ = ctx.svc.jobs != nullptr;
    preview_ = UiImage{};
    ready_.clear();
    refresh_rows(ctx);
    list_.set_focus(0);
}

Rect ConvertDialog::list_area(const Rect& content) const {
    const int x = content.x + kPreviewWidth + kColumnGap;
    return Rect{x, content.y + 52, content.right() - x, content.h - 52};
}

void ConvertDialog::refresh_rows(UiContext& ctx) {
    // The model set may change while the screen is open (a download finishes
    // in the background). Rebuild the rows only when it did, so the focus the
    // user moved is not undone by the next frame.
    std::vector<ModelView> fresh;
    if (ctx.svc.models != nullptr) {
        for (const ModelView& model : ctx.svc.models->list()) {
            if (model.state != "ready" || !model.offered) continue;
            fresh.push_back(model);
        }
        // The remembered model comes first.
        for (std::size_t index = 0; index < fresh.size(); ++index) {
            if (!fresh[index].selected) continue;
            std::rotate(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(index),
                        fresh.end());
            break;
        }
    }
    bool same = fresh.size() == ready_.size() && !list_.rows().empty();
    for (std::size_t index = 0; same && index < fresh.size(); ++index) {
        same = fresh[index].id == ready_[index].id;
    }
    if (same) return;

    const std::string keep = model_id();
    ready_ = std::move(fresh);
    std::vector<ListRow> rows;
    if (ready_.empty()) {
        rows.push_back(ListRow{"Open Models", "Download a model in Settings", "", false});
    } else {
        for (const ModelView& model : ready_) {
            rows.push_back(ListRow{model.name, model.blurb, seconds_hint(model.backend_seconds),
                                   !jobs_available_});
        }
    }
    list_.set_rows(std::move(rows));
    for (std::size_t index = 0; index < ready_.size(); ++index) {
        if (ready_[index].id == keep) list_.set_focus(static_cast<int>(index));
    }
}

std::string ConvertDialog::model_id() const {
    const int focus = list_.focus();
    if (focus < 0 || focus >= static_cast<int>(ready_.size())) return {};
    return ready_[static_cast<std::size_t>(focus)].id;
}

void ConvertDialog::update(UiContext& ctx, const UiInput& input) {
    refresh_rows(ctx);
    if (preview_.empty() && !photos_.empty()) {
        const Path first = photos_.front();
        const std::string key = first.string();
        thumbs_.request(key, [first] { return load_photo_thumb(first, kPreviewWidth); });
        if (const UiImage* image = thumbs_.get(key)) {
            preview_ = *image;
            ctx.invalidate();
        } else if (thumbs_.pending()) {
            ctx.invalidate();
        }
    }

    list_.update(input, list_area(ctx.content), ctx.theme);
    if (!list_.activated()) return;
    list_.clear_activated();
    if (ready_.empty()) {
        ctx.nav.push(ScreenId::Models);
        return;
    }
    start(ctx);
}

void ConvertDialog::start(UiContext& ctx) {
    if (ctx.svc.jobs == nullptr) {
        ctx.toast("Conversion is not available on this build");
        return;
    }
    if (photos_.empty()) {
        ctx.toast("No photos selected");
        return;
    }
    const std::string model = model_id();
    if (model.empty()) {
        ctx.toast("No model ready");
        return;
    }

    const Status queued = ctx.svc.jobs->enqueue(photos_, model, album_);
    if (!queued.ok()) {
        ctx.toast(queued.message());
        return;
    }
    // Remember the choice for the next conversion; a failure only loses that.
    if (ctx.svc.models != nullptr) ctx.svc.models->select(model);
    // No toast: the folder's new tiles and the conversion chip show it.

    // Back to the folder the photos were picked for.
    const int folder = ctx.nav.find(ScreenId::Scenes);
    if (folder >= 0) {
        ctx.nav.pop_to(static_cast<std::size_t>(folder) + 1);
    } else {
        ctx.nav.pop();
    }
}

std::string ConvertDialog::message() const {
    if (!jobs_available_) return "Conversion is not available on this build";
    return {};
}

void ConvertDialog::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;

    // Left: the first photo and where the scenes will go.
    const Rect preview{content.x, content.y, kPreviewWidth, kPreviewHeight};
    if (!preview_.empty()) {
        if (photos_.size() > 1) {
            canvas.fill_round_rect(preview.x + 16, preview.y - 10, preview.w - 32, 16, 8,
                                   Rgba{255, 255, 255, 48});
        }
        canvas.blit_cover(preview.x, preview.y, preview.w, preview.h, preview_, theme.radius);
    } else {
        canvas.fill_round_rect(preview.x, preview.y, preview.w, preview.h, theme.radius,
                               theme.panel);
    }
    int y = preview.bottom() + 18;
    canvas.text(preview.x, y, photo_count(photos_.size()), theme.heading_size, theme.text);
    y += UiCanvas::line_height(theme.heading_size);
    const std::string folder = album_.empty() ? std::string(kAllScenesAlbumName) : album_;
    canvas.text(preview.x, y, fit_text("into " + folder, theme.body_size - 2, preview.w),
                theme.body_size - 2, theme.dim_text);

    // Right: the question and the models.
    const Rect area = list_area(content);
    const std::string note = message();
    std::string heading = ready_.empty() ? "No model installed yet" : "Convert with";
    canvas.text(area.x, content.y + 4, heading, theme.heading_size, theme.text);
    list_.draw(canvas, area, theme);
    if (!note.empty()) {
        const int note_y = area.y + static_cast<int>(list_.rows().size()) * theme.row_height + 16;
        canvas.text(area.x + 24, note_y, note, theme.body_size - 2, theme.warning);
    }
}

std::vector<Hint> ConvertDialog::hints() const {
    if (ready_.empty()) return {Hint{"Cross", "Open Models"}, Hint{"Circle", "Back"}};
    return {Hint{"Cross", "Convert"}, Hint{"Circle", "Back"}};
}

}  // namespace splat
