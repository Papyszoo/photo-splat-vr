#pragma once

#include <cstdint>

#include "ui/canvas.h"

namespace splat {

// One frame of panel input, produced by the host (pad_input mapping on the
// console, keys on the desktop). `pressed`/`held` are UiAction bit masks:
// pressed = went down this frame, held = down now, so a screen may repeat.
// `pointer_valid` + (`px`, `py`) carry the Sense laser (or mouse) position in
// canvas pixels; `click` is a press on the pointer, valid only with
// pointer_valid. `dt` is the frame time in seconds for timed repeats (the 1 s
// recentre hold uses the same clock).
enum UiAction : uint32_t {
    Up = 1u << 0,
    Down = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    Select = 1u << 4,     // Cross / R2
    Back = 1u << 5,       // Circle
    PagePrev = 1u << 6,   // L1
    PageNext = 1u << 7,   // R1
    Secondary = 1u << 8,  // Square
    Tooltips = 1u << 9,   // Triangle
    Menu = 1u << 10,      // Options
    // Triangle inside the panel opens the focused item's context menu (folder
    // and scene actions); the scene layer still reads the bit as Tooltips.
    Context = Tooltips,
};

struct UiInput {
    uint32_t pressed = 0;
    uint32_t held = 0;
    bool pointer_valid = false;
    float px = 0.0f;
    float py = 0.0f;
    bool click = false;
    double dt = 0.0;
};

// ---- key repeat (VR9) ------------------------------------------------------
// Holding a direction moves the focus like a keyboard: one step on the press,
// then the first repeat after kRepeatDelaySeconds, accelerating from
// kRepeatStartHz to kRepeatEndHz over kRepeatRampSeconds. The repeat is
// generated once, in UiApp, and folded into the frame's `pressed` bits, so
// every list, grid, popup and menu gets it without knowing about it (the user:
// "I can't move fast, I have to press my joystick multiple times").
inline constexpr double kRepeatDelaySeconds = 0.35;
inline constexpr double kRepeatStartHz = 8.0;
inline constexpr double kRepeatEndHz = 20.0;
inline constexpr double kRepeatRampSeconds = 1.2;

class UiRepeat {
public:
    // The four direction bits, in a fixed order.
    static constexpr uint32_t kDirections[4] = {Up, Down, Left, Right};

    // Advances one frame. `pressed` holds this frame's edges (the first step is
    // the caller's; it is not returned again this frame), `held` the buttons
    // that are down now. Returns the direction bits that repeat on this frame.
    // A held direction without an edge (the panel opened with the stick already
    // pushed) also starts the delay, so it repeats too.
    uint32_t update(uint32_t pressed, uint32_t held, double dt);

    void reset();

    // The current repeat interval, seconds: exposed for the tests and the log.
    static double interval_for_held(double held_seconds);

private:
    struct Key {
        bool down = false;
        double held = 0.0;   // seconds since this direction went down
        double next = 0.0;   // `held` at which the next repeat is due
    };
    Key keys_[4];
};

// A panel screen. The host calls update() every frame and draw() when it wants
// pixels; draw() returns true only when it changed the canvas, so an idle
// screen costs no overlay upload.
class IPanelContent {
public:
    virtual ~IPanelContent() = default;
    virtual void update(const UiInput& input) = 0;
    virtual bool draw(UiCanvas& canvas) = 0;
};

}  // namespace splat
