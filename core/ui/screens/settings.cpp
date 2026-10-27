// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "ui/screen_stack.h"

namespace splat {
namespace {

std::string ready_text(const UiServices& services) {
    if (services.models == nullptr) return {};
    int ready = 0;
    for (const ModelView& model : services.models->list()) {
        if (model.state == "ready") ++ready;
    }
    return ready == 0 ? std::string("None installed") : std::to_string(ready) + " ready";
}

std::string speed_text(float metres_per_second) {
    char text[24];
    std::snprintf(text, sizeof text, "%g m/s", static_cast<double>(metres_per_second));
    return text;
}

std::string turn_text(float degrees_per_second) {
    char text[24];
    std::snprintf(text, sizeof text, "%.0f deg/s", static_cast<double>(degrees_per_second));
    return text;
}

std::string softness_text(float eps) {
    return eps >= 0.4f ? "Soft (0.5)" : "Standard (0.3)";
}

std::string blend_text(bool linear) { return linear ? "Linear" : "sRGB"; }

}  // namespace

UiState& SettingsScreen::state(UiContext& ctx) {
    return ctx.svc.state != nullptr ? *ctx.svc.state : local_state_;
}

void SettingsScreen::refresh_rows(const UiContext& ctx) {
    const UiState& values = ctx.svc.state != nullptr ? *ctx.svc.state : local_state_;
    std::vector<ListRow> rows(ItemCount);
    rows[ComfortHeader] = ListRow{"Comfort", "", "", true, false, true};
    rows[ScaleAround] = ListRow{"Scale", "Maquette on a table, front or centre", "",
                                false, false, false};
    rows[MoveSpeed] = ListRow{"Move speed", "Full-deflection stick speed", "", false, false, false};
    rows[TurnSpeed] = ListRow{"Turn speed", "Full-deflection turn rate", "", false, false, false};
    rows[DisplayHeader] = ListRow{"Display", "", "", true, false, true};
    rows[EdgeSoftness] = ListRow{"Edge softness", "Screen-space splat filter (eps2d)", "", false,
                                 false, false};
    rows[Blending] = ListRow{"Blending", "Linear light, or gamma like Spark", "", false, false,
                             false};
    rows[SystemHeader] = ListRow{"System", "", "", true, false, true};
    rows[Models] = ListRow{"Models", "Download, licences and storage", ready_text(ctx.svc),
                           ctx.svc.models == nullptr, false, false};
    rows[Conversions] = ListRow{"Conversions", "Photos being turned into scenes", "",
                                ctx.svc.jobs == nullptr, false, false};
    rows[Quit] = ListRow{"Quit", "Close Photo Splat VR", "", false, false, false};

    rows[ScaleAround].right = scale_mode_name(values.scale_around);
    rows[MoveSpeed].right = speed_text(values.move_speed);
    rows[TurnSpeed].right = turn_text(values.turn_speed);
    rows[EdgeSoftness].right = softness_text(values.edge_softness);
    rows[Blending].right = blend_text(values.blend_linear);
    list_.set_rows(std::move(rows));
}

void SettingsScreen::on_enter(UiContext& ctx, const ScreenArgs& /*args*/) {
    if (ctx.svc.state != nullptr) local_state_ = *ctx.svc.state;
    refresh_rows(ctx);
}

void SettingsScreen::save(UiContext& ctx) {
    if (ctx.svc.state == nullptr) return;
    const Status saved = save_ui_state(ctx.svc.paths.state, *ctx.svc.state);
    if (!saved.ok()) {
        ctx.toast("Could not save settings");
        if (ctx.svc.log) ctx.svc.log("ui: settings: " + saved.message());
    }
}

void SettingsScreen::cycle(UiContext& ctx, int item, int direction) {
    if (direction == 0) return;
    UiState& values = state(ctx);
    auto step = [direction](int index, int count, int* out) {
        const int next = std::clamp(index + direction, 0, count - 1);
        *out = next;
        return next != index;
    };
    int changed = 0;
    switch (item) {
        case ScaleAround: {
            // Maquette -> Front -> Centre -> Maquette: the default first.
            values.scale_around = (values.scale_around + 1) % 3;
            changed = 1;
            break;
        }
        case MoveSpeed: {
            int index = nearest_move_speed_index(values.move_speed);
            changed = step(index, 4, &index);
            values.move_speed = kMoveSpeeds[index];
            break;
        }
        case TurnSpeed: {
            int index = nearest_turn_speed_index(values.turn_speed);
            changed = step(index, 4, &index);
            values.turn_speed = kTurnSpeeds[index];
            break;
        }
        case EdgeSoftness: {
            const int index = values.edge_softness >= 0.4f ? 0 : 1;
            values.edge_softness = kEdgeSoftness[index];
            changed = 1;
            break;
        }
        case Blending: {
            values.blend_linear = !values.blend_linear;
            changed = 1;
            break;
        }
        default: return;
    }
    if (!changed) return;
    if (ctx.svc.state != nullptr) local_state_ = *ctx.svc.state;
    refresh_rows(ctx);
    save(ctx);
}

void SettingsScreen::update(UiContext& ctx, const UiInput& input) {
    // Jobs and downloads finish while Settings stays open, so the summaries
    // are refreshed every frame, without a row rebuild.
    list_.set_right(Models, ready_text(ctx.svc));
    list_.update(input, ctx.content, ctx.theme);

    // Left/Right is the quick way to change a setting without activating it;
    // Cross (or a click) does the same on the focused row.
    const int focused = list_.focus();
    if ((input.pressed & (Left | Right)) != 0) {
        cycle(ctx, focused, (input.pressed & Right) != 0 ? 1 : -1);
        return;
    }
    if (!list_.activated()) return;
    list_.clear_activated();

    switch (focused) {
        case ScaleAround:
        case MoveSpeed:
        case TurnSpeed:
        case EdgeSoftness:
        case Blending:
            cycle(ctx, focused, 1);
            break;
        case Models: ctx.nav.push(ScreenId::Models); break;
        case Conversions: ctx.nav.push(ScreenId::Queue); break;
        case Quit: ctx.command(UiCommand{UiCommand::Quit, std::string(), 0}); break;
        default: break;
    }
}

void SettingsScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    // The conversion status is the top bar's chip; it is not repeated here.
    list_.draw(canvas, content, ctx.theme);
}

std::vector<Hint> SettingsScreen::hints() const {
    return {Hint{"Cross", "Change"}, Hint{"Circle", "Back"}};
}

}  // namespace splat
