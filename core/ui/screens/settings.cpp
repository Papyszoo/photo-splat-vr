// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/screens/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "gallery/visibility.h"
#include "ui/screen_stack.h"
#include "ui/screens/photos.h"

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

// "2 shares" / "None yet": the System section's one-line summary of the
// Network locations row. With a passcode set only the locations the visitor may
// see are counted: Settings is reachable by a visitor, and the count must match
// the Locations screen behind it (stage H).
std::string locations_text(const UiServices& services) {
    if (services.locations == nullptr) return {};
    // Every configured location counts (stage U1): the passcode hides scenes
    // on a location, not its row in Settings.
    const std::size_t count = services.locations->list().size();
    if (count == 0) return "None yet";
    return std::to_string(count) + (count == 1 ? " location" : " locations");
}

// "Off" / "Protected": never a count and never a folder name. The visitor who
// reaches Settings may learn that the feature exists, not what it hides.
std::string visibility_text() {
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    return visibility->enabled || visibility->damaged ? "Protected" : "Off";
}

}  // namespace

UiState& SettingsScreen::state(UiContext& ctx) {
    return ctx.svc.state != nullptr ? *ctx.svc.state : local_state_;
}

void SettingsScreen::refresh_rows(const UiContext& ctx) {
    const UiState& values = ctx.svc.state != nullptr ? *ctx.svc.state : local_state_;
    std::vector<ListRow> rows(ItemCount);
    rows[ComfortHeader] = ListRow{"Comfort", "", "", true, false, true};
    rows[ScaleAround] = ListRow{"Scale", "Capture zooms; Centre/Front scale the scene in place", "",
                                false, false, false};
    rows[MoveSpeed] = ListRow{"Move speed", "Full-deflection stick speed", "", false, false, false};
    rows[TurnSpeed] = ListRow{"Turn speed", "Full-deflection turn rate", "", false, false, false};
    rows[DisplayHeader] = ListRow{"Display", "", "", true, false, true};
    rows[EdgeSoftness] = ListRow{"Edge softness", "Screen-space splat filter (eps2d)", "", false,
                                 false, false};
    rows[Blending] = ListRow{"Blending", "Linear light, or gamma like Spark", "", false, false,
                             false};
    // The one thumbnail size for every entry grid. The slider draws its own
    // value text in the row; the row's `right` stays empty so the two do not
    // collide.
    rows[ThumbnailSize] = ListRow{"Thumbnail size", "How big the tiles are", "", false, false,
                                  false};
    rows[SystemHeader] = ListRow{"System", "", "", true, false, true};
    rows[Models] = ListRow{"Models", "Download, licences and storage", ready_text(ctx.svc),
                           ctx.svc.models == nullptr, false, false};
    rows[Conversions] = ListRow{"Conversions", "Photos being turned into scenes", "",
                                ctx.svc.jobs == nullptr, false, false};
    rows[Locations] = ListRow{"Network locations", "SMB shares and NFS exports the console can read",
                              locations_text(ctx.svc), ctx.svc.locations == nullptr, false,
                              false};
    rows[VisibleFolders] = ListRow{"Visible folders", "Choose which folders a visitor can see",
                                   visibility_text(), false, false, false};
    rows[Quit] = ListRow{"Quit", "Close Photo Splat VR", "", false, false, false};

    rows[ScaleAround].right = scale_mode_name(values.scale_around);
    rows[MoveSpeed].right = speed_text(values.move_speed);
    rows[TurnSpeed].right = turn_text(values.turn_speed);
    rows[EdgeSoftness].right = softness_text(values.edge_softness);
    rows[Blending].right = blend_text(values.blend_linear);
    slider_.set_value(values.thumbnail_size);
    list_.set_rows(std::move(rows));
}

std::string SettingsScreen::thumbnail_columns_text(const UiContext& ctx, int size) const {
    // Two grids draw thumbnails at a step: the folder screens (the panel's whole
    // content area) and the photo picker (its grid starts below the tab strip).
    // Stage TF2 gives both the same column count at every step - the tile takes
    // the width the step's columns leave - so this is one number. The range form
    // stays for a layout where a shorter area needs another column: the label
    // says "about" how many fit per row, so it must never name a count no grid
    // draws.
    const ThumbnailLabel folder_label{kThumbnailCaptionLines, true};
    const ThumbnailLabel photo_label{kThumbnailCaptionLines, false};
    const int folder = thumbnail_columns(ctx.content, ctx.theme, size, folder_label);
    const int photos = thumbnail_columns(PhotosScreen::grid_area(ctx.content, ctx.theme),
                                         ctx.theme, size, photo_label);
    const int low = std::min(folder, photos);
    const int high = std::max(folder, photos);
    return low == high ? std::to_string(low) : std::to_string(low) + "-" + std::to_string(high);
}

std::string SettingsScreen::thumbnail_text(const UiContext& ctx) const {
    const int size = ctx.thumbnail_size();
    return std::string(thumbnail_size_name(size)) + " \xC2\xB7 " +
           thumbnail_columns_text(ctx, size) + " per row";
}

int SettingsScreen::thumbnail_text_width(const UiContext& ctx) const {
    // The widest label any step draws. The slider lays its track out from it, so
    // the track and the knob do not shift when the step (and with it the label's
    // width) changes under a drag, and the hit test and the drawing use exactly
    // the same numbers.
    int widest = 0;
    for (int size = 0; size < kThumbnailSizeCount; ++size) {
        const std::string text =
            std::string(thumbnail_size_name(size)) + " \xC2\xB7 " +
            thumbnail_columns_text(ctx, size) + " per row";
        widest = std::max(widest, static_cast<int>(UiCanvas::text_width(text, ctx.theme.small_size)));
    }
    return widest;
}

Rect SettingsScreen::slider_area(const Rect& row, const UiTheme& theme) const {
    const int inset = std::max(4, theme.scaled(24));
    const int width = std::min(std::max(0, row.w - 2 * inset), std::max(1, theme.scaled(560)));
    return Rect{row.right() - inset - width, row.y, width, row.h};
}

void SettingsScreen::set_thumbnail_size(UiContext& ctx, int size) {
    UiState& values = state(ctx);
    const int clamped = clamp_thumbnail_size(size);
    if (clamped == clamp_thumbnail_size(values.thumbnail_size)) return;
    values.thumbnail_size = clamped;
    if (ctx.svc.state != nullptr) local_state_ = *ctx.svc.state;
    refresh_rows(ctx);
    if (ctx.svc.log != nullptr && ctx.svc.state != nullptr) {
        ctx.svc.log("ui: thumbnail size: " + std::string(thumbnail_size_name(clamped)) + " (" +
                    thumbnail_columns_text(ctx, clamped) + " per row)");
    }
    save(ctx);
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
            // Right/Cross: Capture (default) -> Front -> Centre -> Capture.
            // Left walks the same ring backwards.
            values.scale_around = ((values.scale_around + direction) % 3 + 3) % 3;
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
        case ThumbnailSize: {
            // The slider's pad step: one notch per press, clamped at the ends.
            // set_thumbnail_size() refreshes, logs and saves.
            set_thumbnail_size(ctx, clamp_thumbnail_size(values.thumbnail_size) + direction);
            return;
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
    list_.set_right(Locations, locations_text(ctx.svc));
    list_.set_right(VisibleFolders, visibility_text());
    list_.update(input, ctx.content, ctx.theme);

    // The thumbnail slider takes a pointer press or drag on its row even when
    // another row has the focus: the x under the laser is the step. A click on
    // that row must not also activate it as a plain row, so its activation is
    // cleared here, before the Cross handling below. Only the track (Slider::
    // hit_rect) changes the value; the "Default · 7 per row" text is a label,
    // so a press there leaves the size as it is.
    if (input.pointer_valid && (input.click || input.pointer_down)) {
        const Rect row = list_.row_rect(ThumbnailSize, ctx.content, ctx.theme);
        const Rect area = row.empty() ? Rect{} : slider_area(row, ctx.theme);
        if (!area.empty() && area.contains(input.px, input.py)) {
            const Rect hit = Slider::hit_rect(area, thumbnail_text_width(ctx), ctx.theme);
            if (hit.contains(input.px, input.py)) {
                set_thumbnail_size(
                    ctx, slider_.value_at(
                             input.px, Slider::track_rect(area, thumbnail_text_width(ctx),
                                                          ctx.theme)));
                list_.set_focus(ThumbnailSize);
            }
            list_.clear_activated();
            return;
        }
    }

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
        case ThumbnailSize:
            cycle(ctx, focused, 1);
            break;
        case Models: ctx.nav.push(ScreenId::Models); break;
        case Conversions: ctx.nav.push(ScreenId::Queue); break;
        case Locations: ctx.nav.push(ScreenId::Locations); break;
        case VisibleFolders: ctx.nav.push(ScreenId::VisibleFolders); break;
        case Quit: ctx.command(UiCommand{UiCommand::Quit, std::string(), 0}); break;
        default: break;
    }
}

void SettingsScreen::draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) {
    // The conversion status is the top bar's chip; it is not repeated here.
    list_.draw(canvas, content, ctx.theme);
    // The thumbnail size slider lives inside its row: the pads change it with
    // Left/Right, the laser drags the knob, and the value text says about how
    // many tiles fit per row.
    const Rect row = list_.row_rect(ThumbnailSize, content, ctx.theme);
    if (row.empty()) return;
    slider_.set_value(ctx.thumbnail_size());
    slider_.draw(canvas, slider_area(row, ctx.theme), thumbnail_text(ctx), thumbnail_text_width(ctx),
                 list_.focus() == ThumbnailSize, ctx.theme);
}

std::vector<Hint> SettingsScreen::hints() const {
    return {Hint{"Cross", "Change"}, Hint{"Circle", "Back"}};
}

}  // namespace splat
