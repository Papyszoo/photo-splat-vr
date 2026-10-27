// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/app.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "ui/button_glyph.h"
#include "ui/text_fit.h"
#include "ui/widgets.h"

namespace splat {
namespace {

constexpr int kTitleTop = 30;
constexpr int kContentTop = 112;
constexpr int kHintBand = 76;     // bottom band holding the button prompts
constexpr int kGlyphHeight = 26;  // button prompt disc
constexpr int kHintGap = 28;      // between two prompt/label pairs
constexpr int kStatusTop = 34;
constexpr int kToastHeight = 52;

float smooth_mix(float a, float b, float t) { return a + (b - a) * t; }

// 4x4 ordered dither, so the dark gradient does not band on the TV.
constexpr float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

}  // namespace

UiApp::UiApp(UiServices& services, UiTheme theme)
    : services_(services),
      theme_(theme),
      nav_(registry_),
      context_{services_, theme_, nav_} {
    context_.toast_sink = [this](std::string text, double seconds) {
        toast(std::move(text), seconds);
    };
    context_.command_sink = [this](UiCommand command) { commands_.push_back(std::move(command)); };
    context_.invalidate_sink = [this] { invalidate(); };
    nav_.set_context(&context_);
    nav_.set_unavailable_handler(
        [this](const std::string& name) { toast(name + " not available yet"); });
    context_.content = content_rect(canvas_width_, canvas_height_);
}

Rect UiApp::content_rect(uint32_t canvas_width, uint32_t canvas_height) const {
    // The chrome bands scale with the theme too (VR9): the VR panel's 0.7 theme
    // puts the title and the content area at the same physical place as the TV's.
    const int content_top = theme_.scaled(kContentTop);
    const int hint_band = theme_.scaled(kHintBand);
    return Rect{theme_.margin, content_top, static_cast<int>(canvas_width) - 2 * theme_.margin,
                static_cast<int>(canvas_height) - content_top - hint_band};
}

void UiApp::start() {
    if (started_) return;
    if (!registry_.has(ScreenId::Albums)) return;
    started_ = true;
    nav_.push(ScreenId::Albums);
    dirty_ = true;
}

void UiApp::tick_services(double dt) {
    if (dt <= 0.0) return;
    if (services_.jobs != nullptr) services_.jobs->tick(dt);
    if (services_.models != nullptr) services_.models->tick(dt);
    if (services_.captures != nullptr) services_.captures->tick(dt);
}

void UiApp::update_toast(double dt) {
    if (toast_seconds_ <= 0.0) return;
    toast_seconds_ -= dt;
    if (toast_seconds_ > 0.0) return;
    toast_seconds_ = 0.0;
    toast_text_.clear();
    dirty_ = true;
}

void UiApp::handle_navigation(const UiInput& input, UiInput& local) {
    if ((input.pressed & Back) != 0) {
        local.pressed &= ~static_cast<uint32_t>(Back);
        Screen* screen = nav_.top();
        if (screen != nullptr && screen->handle_back(context_)) {
            dirty_ = true;
        } else if (nav_.size() > 1) {
            nav_.pop();
            dirty_ = true;
        } else if (scene_loaded_) {
            commands_.push_back(UiCommand{UiCommand::ResumeScene, std::string(), 0});
            dirty_ = true;
        }
    }
    if ((input.pressed & Menu) != 0) {
        local.pressed &= ~static_cast<uint32_t>(Menu);
        const int settings = nav_.find(ScreenId::Settings);
        if (settings > 0) {
            nav_.pop_to(static_cast<std::size_t>(settings));
        } else if (settings < 0) {
            nav_.push(ScreenId::Settings);
        }
        dirty_ = true;
    }
}

void UiApp::update(const UiInput& input) {
    tick_services(input.dt);
    update_toast(input.dt);
    start();
    if (nav_.empty()) return;

    if (input.pressed != 0 || input.click) dirty_ = true;
    // A pointer that moved changes the hovered row/cell (Sense laser).
    if (input.pointer_valid) {
        if (!pointer_valid_ || input.px != pointer_x_ || input.py != pointer_y_) dirty_ = true;
        pointer_valid_ = true;
        pointer_x_ = input.px;
        pointer_y_ = input.py;
    } else {
        pointer_valid_ = false;
    }
    // A running job animates its progress, about 4 times per second.
    const bool busy = services_.jobs != nullptr && services_.jobs->busy();
    if (busy) {
        busy_redraw_ += input.dt;
        if (busy_redraw_ >= 0.25) {
            busy_redraw_ = 0.0;
            dirty_ = true;
        }
    }
    if (busy != was_busy_) {
        dirty_ = true;
        busy_redraw_ = 0.0;
        was_busy_ = busy;
    }

    UiInput local = input;
    // VR9: one repeat for every list, grid and popup. The injected edges are
    // ordinary presses by the time a screen sees them, and they count as a
    // change so the host redraws the moved focus.
    const uint32_t repeats = repeat_.update(input.pressed, input.held, input.dt);
    if (repeats != 0) {
        local.pressed |= repeats;
        dirty_ = true;
    }
    handle_navigation(input, local);
    nav_.update(context_, local);
}

bool UiApp::draw(UiCanvas& canvas) {
    if (!dirty_) return false;
    dirty_ = false;

    canvas_width_ = canvas.width();
    canvas_height_ = canvas.height();
    const Rect content = content_rect(canvas_width_, canvas_height_);
    context_.content = content;

    draw_backdrop(canvas);
    draw_title(canvas);
    nav_.draw(context_, canvas, content);
    draw_hint_bar(canvas);
    draw_status(canvas);
    return true;
}

void UiApp::draw_backdrop(UiCanvas& canvas) {
    const uint32_t w = canvas.width();
    const uint32_t h = canvas.height();
    if (backdrop_.size() != canvas.pixels().size() || backdrop_width_ != w ||
        backdrop_height_ != h) {
        backdrop_.assign(static_cast<std::size_t>(w) * h * 4, 255);
        backdrop_width_ = w;
        backdrop_height_ = h;
        // A vertical gradient with one soft light near the top left, like a
        // lit stage behind the tiles.
        const float glow_x = 0.24f * static_cast<float>(w);
        const float glow_y = -0.18f * static_cast<float>(h);
        const float glow_r = 0.95f * static_cast<float>(w);
        const float glow_rgb[3] = {38.0f, 92.0f, 196.0f};
        const Rgba top = theme_.background_top;
        const Rgba bottom = theme_.background_bottom;
        for (uint32_t y = 0; y < h; ++y) {
            const float t = h > 1 ? static_cast<float>(y) / static_cast<float>(h - 1) : 0.0f;
            const float base[3] = {smooth_mix(top.r, bottom.r, t), smooth_mix(top.g, bottom.g, t),
                                   smooth_mix(top.b, bottom.b, t)};
            for (uint32_t x = 0; x < w; ++x) {
                const float dx = static_cast<float>(x) - glow_x;
                const float dy = static_cast<float>(y) - glow_y;
                float g = 1.0f - std::sqrt(dx * dx + dy * dy) / glow_r;
                g = g > 0.0f ? g * g * 0.42f : 0.0f;
                const float dither = (kBayer[(y & 3u) * 4 + (x & 3u)] + 0.5f) / 16.0f;
                uint8_t* pixel = backdrop_.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                for (int c = 0; c < 3; ++c) {
                    const float value = smooth_mix(base[c], glow_rgb[c], g) + dither - 0.5f;
                    pixel[c] = static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
                }
                pixel[3] = 255;
            }
        }
    }
    // A straight copy; the title drawn next marks the canvas dirty.
    canvas.pixels() = backdrop_;
}

void UiApp::draw_title(UiCanvas& canvas) {
    // The title takes the whole width up to the status chip (or the margin).
    int right = static_cast<int>(canvas.width()) - theme_.margin;
    const int chip = conversion_chip_width(conversion_status(services_.jobs), theme_);
    if (chip > 0) right -= chip + 24;
    const std::string title =
        fit_text(nav_.title(), theme_.title_size, right - theme_.margin);
    canvas.text(theme_.margin, theme_.scaled(kTitleTop), title, theme_.title_size, theme_.text);
}


void UiApp::draw_hint_bar(UiCanvas& canvas) {
    const std::vector<Hint> hints = nav_.hints();
    if (hints.empty()) return;
    const int size = theme_.small_size;
    // The prompt discs and their spacing are design-time pixels: scale them with
    // the theme, or the VR band would be physically larger than the TV's.
    const int glyph_height = std::max(1, theme_.scaled(kGlyphHeight));
    const int gap = theme_.scaled(kHintGap);
    const int disc_gap = theme_.scaled(10);
    const Rgba plate = tooltips_ ? theme_.light : theme_.faint_text;
    const Rgba ink = theme_.background_bottom;
    const Rgba label_color = tooltips_ ? theme_.dim_text : theme_.faint_text;

    // Right-aligned group, read left to right.
    int total = 0;
    for (const Hint& hint : hints) {
        total += button_glyph_width(hint.button, glyph_height) + disc_gap +
                 static_cast<int>(UiCanvas::text_width(hint.label, size)) + gap;
    }
    total -= gap;
    const int cy = static_cast<int>(canvas.height()) - theme_.scaled(kHintBand) / 2;
    int x = static_cast<int>(canvas.width()) - theme_.margin - total;
    const int text_y = UiCanvas::text_top_centered(cy - glyph_height / 2, glyph_height, size);
    for (const Hint& hint : hints) {
        x += draw_button_glyph(canvas, x, cy, hint.button, glyph_height, plate, ink) + disc_gap;
        x += static_cast<int>(canvas.text(x, text_y, hint.label, size, label_color)) + gap;
    }
}

void UiApp::draw_status(UiCanvas& canvas) {
    const int right = static_cast<int>(canvas.width()) - theme_.margin;
    const int status_top = theme_.scaled(kStatusTop);
    const int toast_height = theme_.scaled(kToastHeight);
    if (toast_visible()) {
        // A notification card in the top right corner.
        const int size = theme_.body_size - 2;
        const std::string text = fit_text(toast_text_, size, 560);
        const int width = static_cast<int>(UiCanvas::text_width(text, size)) + 2 * 22 + 6;
        const int x = right - width;
        canvas.glow_round_rect(x, status_top - 10, width, toast_height, 14, 18,
                               Rgba{0, 0, 0, 110});
        canvas.fill_round_rect(x, status_top - 10, width, toast_height, 14, theme_.card);
        canvas.fill_round_rect(x + 10, status_top + 4, 4, toast_height - 28, 2, theme_.accent);
        canvas.text(x + 28, UiCanvas::text_top_centered(status_top - 10, toast_height, size), text,
                    size, theme_.text);
        return;
    }
    // The one conversion status: the chip, top right.
    draw_conversion_chip(canvas, right, status_top + 12, conversion_status(services_.jobs), theme_);
}

std::optional<UiCommand> UiApp::take_command() {
    if (commands_.empty()) return std::nullopt;
    UiCommand command = std::move(commands_.front());
    commands_.erase(commands_.begin());
    return command;
}

void UiApp::set_scene_loaded(bool loaded) {
    if (scene_loaded_ == loaded) return;
    scene_loaded_ = loaded;
    dirty_ = true;
}

void UiApp::set_tooltips(bool on) {
    if (tooltips_ == on) return;
    tooltips_ = on;
    dirty_ = true;
}

void UiApp::toast(std::string text, double seconds) {
    toast_text_ = std::move(text);
    toast_seconds_ = seconds;
    dirty_ = true;
}

}  // namespace splat
