// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/models.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "ui/model_service.h"
#include "ui/screen_stack.h"
#include "ui/text_fit.h"

namespace splat {
namespace {

constexpr int kFooterHeight = 36;

std::string size_text(uint64_t bytes) {
    char text[32];
    if (bytes >= 1000000000ull) {
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    } else if (bytes >= 1000000ull) {
        std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / 1e6);
    } else {
        std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1e3);
    }
    return text;
}

std::string percent_text(float fraction) {
    return std::to_string(static_cast<int>(std::lround(fraction * 100.0f))) + "%";
}

std::string state_text(const ModelView& view) {
    if (view.state == "ready") return "Ready";
    if (view.state == "license_required") return "Licence needed";
    if (view.state == "accepted") return view.fraction > 0.0f ? "Paused" : "Not downloaded";
    if (view.state == "downloading") return "Downloading " + percent_text(view.fraction);
    if (view.state == "verifying") return "Verifying " + percent_text(view.fraction);
    if (view.state == "converting") return "Preparing " + percent_text(view.fraction);
    if (view.state == "failed") return "Failed";
    std::string text = view.state;
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = static_cast<char>(text[0] - 32);
    return text.empty() ? "Unknown" : text;
}

std::string sublabel_text(const ModelView& view) {
    // Plain words only: the licence is named on the licence step, not here.
    std::string text = view.blurb;
    if (view.backend_seconds > 0.0) {
        char seconds[48];
        std::snprintf(seconds, sizeof(seconds), "about %.0f s per photo",
                      std::max(1.0, view.backend_seconds));
        if (!text.empty()) text += "  \xC2\xB7  ";
        text += seconds;
    }
    return text;
}

}  // namespace

bool ModelsScreen::is_active(const ModelView& view) {
    return view.state == "downloading" || view.state == "verifying" ||
           view.state == "converting";
}

void ModelsScreen::on_enter(UiContext& ctx, const ScreenArgs& /*args*/) {
    confirm_.close();
    refresh(ctx);
    if (ctx.svc.models != nullptr) {
        const std::string selected = ctx.svc.models->selected();
        for (std::size_t index = 0; index < models_.size(); ++index) {
            if (models_[index].id == selected) {
                list_.set_focus(static_cast<int>(index));
                break;
            }
        }
    }
    sync_rows(ctx);
}

void ModelsScreen::refresh(UiContext& ctx) {
    if (ctx.svc.models == nullptr) {
        models_.clear();
        return;
    }
    models_ = ctx.svc.models->list();
}

void ModelsScreen::sync_rows(UiContext& ctx) {
    std::vector<ListRow> rows;
    rows.reserve(models_.size());
    std::string key;
    for (const ModelView& view : models_) {
        ListRow row;
        row.label = view.name;
        row.sublabel = sublabel_text(view);
        row.right = state_text(view) + "  \xC2\xB7  " + size_text(view.size_bytes);
        row.dim = !view.offered;
        rows.push_back(std::move(row));

        key += view.id + "|" + view.state + "|" + percent_text(view.fraction) + "|" +
               view.message + ";";
    }
    // The last row is the default model, changed in place with Cross: the
    // remembered one, else the first ready one - exactly what the model choice
    // offers first.
    default_name_.clear();
    std::string first_ready;
    if (ctx.svc.models != nullptr) {
        const std::string selected = ctx.svc.models->selected();
        for (const ModelView& view : models_) {
            if (view.state != "ready") continue;
            if (first_ready.empty()) first_ready = view.name;
            if (view.id == selected) default_name_ = view.name;
        }
    }
    if (default_name_.empty()) default_name_ = first_ready;
    const bool any_ready = !first_ready.empty();
    rows.push_back(ListRow{"Default model", "Offered first when you convert photos",
                           default_name_.empty() ? std::string("None") : default_name_,
                           !any_ready});
    list_.set_rows(std::move(rows));

    // The download advances on the worker thread with no input: repaint when the
    // visible state changed (about once per percent).
    if (key != status_key_) {
        status_key_ = std::move(key);
        ctx.invalidate();
    }
}

const ModelView* ModelsScreen::focused() const {
    const int index = list_.focus();
    if (index < 0 || index >= static_cast<int>(models_.size())) return nullptr;
    return &models_[static_cast<std::size_t>(index)];
}

int ModelsScreen::active_index() const {
    for (std::size_t index = 0; index < models_.size(); ++index) {
        if (is_active(models_[index])) return static_cast<int>(index);
    }
    return -1;
}

bool ModelsScreen::handle_back(UiContext& /*ctx*/) {
    if (!confirm_.is_open()) return false;
    confirm_.close();
    return true;
}

void ModelsScreen::update(UiContext& ctx, const UiInput& input) {
    if (ctx.svc.models == nullptr) return;
    refresh(ctx);
    sync_rows(ctx);

    if (confirm_.is_open()) {
        if (confirm_.update(input, ctx.content) == ConfirmCard::Choice::Confirm) {
            for (const ModelView& view : models_) {
                if (view.id != confirm_id_) continue;
                const Status removed = ctx.svc.models->remove(view.id);
                ctx.toast(removed.ok() ? view.name + " deleted" : removed.message());
            }
            refresh(ctx);
            sync_rows(ctx);
        }
        return;
    }

    list_.update(input, ctx.content, ctx.theme);

    if ((input.pressed & Secondary) != 0) {
        secondary_action(ctx);
        list_.clear_activated();
        return;
    }
    if (!list_.activated()) return;
    list_.clear_activated();
    select_action(ctx);
}

void ModelsScreen::cycle_default(UiContext& ctx) {
    std::vector<const ModelView*> ready;
    for (const ModelView& view : models_) {
        if (view.state == "ready") ready.push_back(&view);
    }
    if (ready.empty()) {
        ctx.toast("Download a model first");
        return;
    }
    const std::string current = ctx.svc.models->selected();
    std::size_t next = 0;
    for (std::size_t index = 0; index < ready.size(); ++index) {
        if (ready[index]->id == current) next = (index + 1) % ready.size();
    }
    const Status selected = ctx.svc.models->select(ready[next]->id);
    if (!selected.ok()) ctx.toast(selected.message());
    refresh(ctx);
    sync_rows(ctx);
}

void ModelsScreen::select_action(UiContext& ctx) {
    if (on_default_row()) {
        cycle_default(ctx);
        return;
    }
    const ModelView* view = focused();
    if (view == nullptr || view->state == "ready") return;

    if (view->state == "license_required") {
        ScreenArgs args;
        args.a = view->id;
        ctx.nav.push(ScreenId::Licence, args);
        return;
    }
    if (view->state == "accepted" || view->state == "failed") {
        const Status started = ctx.svc.models->download(view->id);
        ctx.toast(started.ok() ? "Downloading " + view->name : started.message());
        refresh(ctx);
        sync_rows(ctx);
        return;
    }
    ctx.toast(view->name + " is " + state_text(*view) + "; wait or press Square to cancel");
}

void ModelsScreen::secondary_action(UiContext& ctx) {
    const ModelView* view = focused();
    if (view == nullptr) return;

    if (is_active(*view)) {
        const Status cancelled = ctx.svc.models->cancel(view->id);
        ctx.toast(cancelled.ok() ? "Cancelling " + view->name : cancelled.message());
        return;
    }

    // A destructive step: Cancel / Delete, Cancel focused first.
    uint64_t bytes = model_installed_bytes(view->id);
    if (bytes == 0) bytes = view->size_bytes;
    confirm_id_ = view->id;
    confirm_.open("Delete \u201C" + view->name + "\u201D?",
                  "Frees " + model_installed_text(bytes) + ". You can download it again later.",
                  "Delete");
}

void ModelsScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    const UiTheme& theme = ctx.theme;
    if (ctx.svc.models == nullptr) {
        canvas.text(content.x, content.y + 8, "Models are not available in this build",
                    theme.heading_size, theme.text);
        return;
    }

    Rect list_area = content;
    list_area.h = std::max(0, list_area.h - kFooterHeight);
    list_.draw(canvas, list_area, theme);

    const int index = active_index();
    if (index >= 0) {
        const int visible = list_.visible_rows(list_area, theme);
        const int row = index - list_.scroll();
        if (row >= 0 && row < visible) {
            draw_row_progress(canvas, list_area, row,
                              list_.rows()[static_cast<std::size_t>(index)].right,
                              models_[static_cast<std::size_t>(index)].fraction,
                              static_cast<int>(list_.rows().size()) > visible, theme);
        }
    }

    // A failed row's reason is not in the row (the columns are fixed): show it
    // under the list so an async download failure is readable.
    const ModelView* current = focused();
    if (current != nullptr && current->state == "failed" && !current->message.empty()) {
        const int size = theme.small_size;
        const std::string message = fit_text(current->name + ": " + current->message, size,
                                             content.w - 48);
        canvas.text(content.x + 24, content.bottom() - UiCanvas::line_height(size), message, size,
                    theme.warning);
    }
    if (confirm_.is_open()) {
        canvas.fill_rect(0, 0, static_cast<int>(canvas.width()), static_cast<int>(canvas.height()),
                         theme.veil);
        confirm_.draw(canvas, content, theme);
    }
}

std::vector<Hint> ModelsScreen::hints() const {
    if (confirm_.is_open()) return {Hint{"Cross", "Choose"}, Hint{"Circle", "Cancel"}};
    if (on_default_row()) return {Hint{"Cross", "Change"}, Hint{"Circle", "Back"}};
    const ModelView* view = focused();
    if (view == nullptr) return {Hint{"Circle", "Back"}};
    if (is_active(*view)) return {Hint{"Square", "Cancel"}, Hint{"Circle", "Back"}};
    std::vector<Hint> hints;
    if (view->state == "license_required") hints.push_back(Hint{"Cross", "Licence"});
    if (view->state == "accepted" || view->state == "failed") {
        hints.push_back(Hint{"Cross", "Download"});
    }
    if (view->state == "ready" || view->state == "accepted" || view->state == "failed") {
        hints.push_back(Hint{"Square", "Delete"});
    }
    hints.push_back(Hint{"Circle", "Back"});
    return hints;
}

}  // namespace splat
