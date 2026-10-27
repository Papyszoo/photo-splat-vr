// One panel screen and the context it talks to. The host drives a ScreenStack
// (core/ui/screen_stack.h) through UiApp (core/ui/app.h); a screen never owns
// the canvas, the theme or a service.
//
// A screen becomes visible in on_enter(): it runs when the screen is pushed or
// replaces another one, and again when a pop reveals it, so a screen can re-list
// the filesystem on return (W10 needs exactly that). draw() gets the content
// rectangle below the title bar and above the hint bar; update() sees the same
// rectangle as UiContext::content (the host keeps both in sync).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "ui/canvas.h"
#include "ui/rect.h"
#include "ui/services.h"
#include "ui/theme.h"
#include "ui/ui_input.h"

namespace splat {

enum class ScreenId {
    Albums,  // the root: folders of scenes
    Scenes,
    Photos,  // the photo picker behind a folder's "+" tile
    ConvertDialog,
    Settings,
    Queue,
    Models,
    Licence,
    Keyboard,
    Controls,
    AlbumPick,
};

// Display name, used for the "not available yet" toast.
inline const char* screen_id_name(ScreenId id) {
    switch (id) {
        case ScreenId::Albums: return "Scenes";
        case ScreenId::Scenes: return "Folder";
        case ScreenId::Photos: return "Photos";
        case ScreenId::ConvertDialog: return "Convert";
        case ScreenId::Settings: return "Settings";
        case ScreenId::Queue: return "Conversions";
        case ScreenId::Models: return "Models";
        case ScreenId::Licence: return "Licence";
        case ScreenId::Keyboard: return "Keyboard";
        case ScreenId::Controls: return "Controls";
        case ScreenId::AlbumPick: return "Move";
    }
    return "Screen";
}

// Arguments a screen is pushed with. The meaning is per screen: `a`/`b` are
// names or modes, `list` is a list of paths or ids, `n` is a count or index.
struct ScreenArgs {
    std::string a;
    std::string b;
    std::vector<std::string> list;
    int n = 0;
};

// One button/label pair in the bottom hint bar: {"Cross", "Open"} renders as
// the cross prompt followed by "Open" (core/ui/button_glyph.h).
struct Hint {
    std::string button;
    std::string label;
};

// A panel screen. update() is called once per frame with the (already
// navigation-filtered) input; draw() paints into the content rectangle and may
// assume the canvas has been cleared and the chrome drawn.
struct UiContext;

class Screen {
public:
    virtual ~Screen() = default;

    virtual void on_enter(UiContext& /*ctx*/, const ScreenArgs& /*args*/) {}
    virtual void update(UiContext& ctx, const UiInput& input) = 0;
    virtual void draw(UiContext& ctx, UiCanvas& canvas, const Rect& content) = 0;
    virtual std::vector<Hint> hints() const { return {}; }
    virtual std::string title() const = 0;
    // Circle, before the app pops the screen: true when the screen used it
    // itself (closing a popup, going up a folder), so nothing is popped.
    virtual bool handle_back(UiContext& /*ctx*/) { return false; }
    // A dialog drawn over the screen below it (dimmed), which keeps its title.
    virtual bool overlay() const { return false; }
};

// Where the app keeps its data. Paths are supplied by the host: the console
// uses /data/ps5-splat-vr, the desktop build a temp or user directory.
struct UiPaths {
    Path scenes;  // scene root; albums are folders under it
    Path inbox;   // uploaded photos
    Path state;   // ui.json, model state
    Path mnt = "/mnt";  // volume aliases (usbN, extN); the console's sandbox mount
    Path data;          // the console's data root (/data), browsable in the photo picker
};

// Everything a screen may reach. Null services are unavailable features.
struct UiState;

struct UiServices {
    UiPaths paths;
    IJobService* jobs = nullptr;
    IModelService* models = nullptr;
    ICaptureService* captures = nullptr;
    // The system text-entry dialog. Null or unavailable: the panel keyboard
    // draws its own QWERTY grid (the fallback until the IME spike lands).
    ITextInput* text_input = nullptr;
    // The panel's persistent state (ui.json): the Settings screen reads and
    // writes the comfort/display values through it, and the VR loop reads the
    // same object every frame. Null in a unit test/preview that does not care.
    UiState* state = nullptr;
    // Host line sink (the viewer passes viewer::line, so a screen's log line
    // lands in klog). Empty in builds without a log reader.
    std::function<void(const std::string&)> log;
};

// A request to the host, emitted from a screen through UiContext::command().
struct UiCommand {
    enum Kind { ViewScene, ResumeScene, Quit } kind = ResumeScene;
    std::string album;  // ViewScene: album folder name ("" = loose scenes)
    int index = 0;      // ViewScene: scene index in that album
};

class ScreenStack;

// The screen's view of the app: services, theme, navigation, and the two
// effects a screen can trigger. UiApp fills the sinks and the content rectangle;
// a screen only calls toast()/command()/nav.
struct UiContext {
    UiServices& svc;
    const UiTheme& theme;
    ScreenStack& nav;

    // Set by UiApp: the area below the title bar and above the hint bar.
    Rect content;

    // Set by UiApp. A context built without them (unit tests) silently drops
    // toasts and commands.
    std::function<void(std::string, double)> toast_sink;
    std::function<void(UiCommand)> command_sink;
    // Async work (a thumbnail arriving) calls this so the host repaints even
    // without input.
    std::function<void()> invalidate_sink;

    void toast(std::string text, double seconds = 2.5) {
        if (toast_sink) toast_sink(std::move(text), seconds);
    }
    void command(UiCommand command) {
        if (command_sink) command_sink(std::move(command));
    }
    void invalidate() {
        if (invalidate_sink) invalidate_sink();
    }
};

}  // namespace splat
