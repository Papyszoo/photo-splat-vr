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
#include <set>
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
    Locations,        // the configured network shares
    LocationEdit,     // one location's fields
    VisibleFolders,   // the passcode and the allow-list (stage H)
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
        case ScreenId::Locations: return "Network locations";
        case ScreenId::LocationEdit: return "Location";
        case ScreenId::VisibleFolders: return "Visible folders";
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
    // Per-item integers for a screen that needs them, parallel to `list`: the
    // photo picker's clockwise quarter turns (missing entries are 0).
    std::vector<int> turns;
    // The visible-folders key of each entry in `list`, parallel to `list`: the
    // photo picker hands its source key to the convert dialog so the queue never
    // has to guess which tab a photo came from (missing entries are empty).
    std::vector<std::string> sources;
    // The free-text keyboard ("a" == "free-text"): where the typed value goes,
    // whether the field draws one '*' per character, and the length cap. A
    // screen sets on_text on the arguments it pushes and reads the value in the
    // callback; the keyboard pops itself afterwards.
    std::function<void(const std::string&)> on_text;
    bool masked = false;
    int max_length = 64;
    // The keyboard's title for free text ("" keeps the default), and the minimum
    // length free text must reach before OK is enabled (0 = only non-empty, the
    // location editor's rule). The Visible folders passcode uses both.
    std::string title;
    int min_length = 0;
    // True for a field whose empty value is meaningful (the location editor's
    // Port: empty is the protocol default). Without it the keyboard's "type a
    // value" rule makes the value impossible to clear back.
    bool allow_empty = false;
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
    // The configured network shares: the Locations screen edits them, the photo
    // picker takes a tab from each photo_source, and a location path in a scene
    // library is read through the VFS. Null: the feature is not in this build.
    ILocationService* locations = nullptr;
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
    // The scene folder the viewer currently has open ("" = none). The host
    // updates it as scenes load; the Scenes shelf refuses to rotate that folder
    // because the renderer is holding the version it read.
    Path open_scene;
    // The rotate job ids whose completion has already been shown as a toast.
    // Session-wide so a turn that finishes while its shelf is off screen is
    // announced once on the way back, and history from an earlier launch (seeded
    // on the first shelf entry) is not announced again.
    std::set<std::string> announced_turns;
    bool turns_seeded = false;
};

// A request to the host, emitted from a screen through UiContext::command().
struct UiCommand {
    // OpenPath: load this scene file from where it lies (a network location or
    // any path the panel browsed), through the VFS and without copying it into
    // the local library. The host makes it a one-entry playlist.
    // HideScene: the visible-folders policy changed (a tick, a new passcode,
    // turning it off). The host checks the scene the viewer has open and closes
    // it when it is no longer allowed, so a hidden scene never stays on screen
    // or resumes behind the panel.
    enum Kind { ViewScene, ResumeScene, Quit, OpenPath, HideScene } kind = ResumeScene;
    std::string album;  // ViewScene: album folder name ("" = loose scenes)
    int index = 0;      // ViewScene: scene index in that album
    Path path;          // OpenPath: the scene file (.psplat)
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
    // The one thumbnail size index every entry grid lays out from
    // (core/ui/widgets.h, stage T): the saved setting clamped, or the default
    // when this context has no state (unit tests, the preview).
    int thumbnail_size() const;
};

}  // namespace splat
