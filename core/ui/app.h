// The panel app: owns the screen stack, the services reference, the theme, the
// toast and the pending commands, and implements the host's IPanelContent.
//
// Chrome: the deep-blue backdrop, the screen title at the top left, the
// content area the screen paints, the button prompts at the bottom right
// (built from Screen::hints()), a small "Converting" indicator at the top right
// while a job runs, and the toast card in the same corner.
//
// Navigation is handled here, before the screen sees the frame: Circle first
// asks the screen (Screen::handle_back, e.g. a popup closes), then pops one
// screen; at the root it emits ResumeScene when a scene is loaded. Options
// opens Settings, and closes it again from Settings or any screen above it.
// The screen only receives the remaining actions.
//
// draw() returns false while nothing changed, so the host uploads the overlay
// texture only on input, a toast change, a navigation step, or while a job is
// running (its progress animates).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ui/canvas.h"
#include "ui/screen.h"
#include "ui/screen_stack.h"
#include "ui/theme.h"
#include "ui/ui_input.h"

namespace splat {

class UiApp : public IPanelContent {
public:
    explicit UiApp(UiServices& services, UiTheme theme = default_theme());
    ~UiApp() override = default;

    UiApp(const UiApp&) = delete;
    UiApp& operator=(const UiApp&) = delete;

    // Screens are registered by the host after construction:
    //   register_default_screens(app.registry());
    ScreenRegistry& registry() { return registry_; }

    // Pushes the root (the folder browser) once. update() also does this on
    // the first frame, so a host that only registers screens and feeds input
    // needs no extra call.
    void start();
    bool started() const { return started_; }

    void update(const UiInput& input) override;
    bool draw(UiCanvas& canvas) override;

    // Pops the oldest pending command; null when there is none.
    std::optional<UiCommand> take_command();

    // The host tells the app whether a scene is behind the panel: Circle at
    // the root then emits ResumeScene instead of doing nothing.
    void set_scene_loaded(bool loaded);
    bool scene_loaded() const { return scene_loaded_; }

    // Dims the button prompts while tooltips are off (W27 persists the flag).
    void set_tooltips(bool on);
    bool tooltips() const { return tooltips_; }

    void toast(std::string text, double seconds = 2.5);
    bool toast_visible() const { return toast_seconds_ > 0.0; }
    const std::string& toast_text() const { return toast_text_; }
    double toast_remaining() const { return toast_seconds_; }

    ScreenStack& nav() { return nav_; }
    const ScreenStack& nav() const { return nav_; }
    UiContext& context() { return context_; }
    const UiTheme& theme() const { return theme_; }

    // Marks the next draw as needed (a host that changes something outside the
    // app, e.g. the tooltips flag on the scene layer).
    void invalidate() { dirty_ = true; }

private:
    Rect content_rect(uint32_t canvas_width, uint32_t canvas_height) const;
    void tick_services(double dt);
    void update_toast(double dt);
    void handle_navigation(const UiInput& input, UiInput& local);
    void draw_backdrop(UiCanvas& canvas);
    void draw_title(UiCanvas& canvas);
    void draw_hint_bar(UiCanvas& canvas);
    void draw_status(UiCanvas& canvas);

    UiServices& services_;
    UiTheme theme_;
    ScreenRegistry registry_;
    ScreenStack nav_;
    UiContext context_;
    // Direction key repeat (VR9): holding a d-pad direction or the left stick
    // folds repeat edges into each frame's `pressed`, so every screen and
    // widget gets the same acceleration.
    UiRepeat repeat_;

    std::string toast_text_;
    double toast_seconds_ = 0.0;
    bool tooltips_ = true;
    bool scene_loaded_ = false;
    bool dirty_ = true;
    bool started_ = false;
    uint32_t canvas_width_ = 1280;
    uint32_t canvas_height_ = 720;
    // A running job repaints about 4 times a second instead of every frame: the
    // progress bar moves, but a 3.5 MB overlay upload at 60 Hz is waste.
    double busy_redraw_ = 0.0;
    bool was_busy_ = false;
    // Pointer movement changes the focus (Sense laser), so it must redraw even
    // without a click.
    bool pointer_valid_ = false;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;
    std::vector<UiCommand> commands_;
    // The backdrop gradient, rendered once per canvas size and copied in.
    std::vector<uint8_t> backdrop_;
    uint32_t backdrop_width_ = 0;
    uint32_t backdrop_height_ = 0;
};

}  // namespace splat
