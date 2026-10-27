// Scripted viewer input: a text file of button presses, stick positions, frame
// dumps and marks, replayed as if a pad had produced them.
//
// Overnight nobody wears the headset, so the TV viewer is driven by a script
// (docs/ui-v1-plan.md section 6). This file owns the format and the player; it
// is core code with no platform dependency, so the parser and the timing are
// checked on the desktop.
//
// One event per line, `#` at the start of a line is a comment, blank lines are
// ignored and CRLF is accepted. Times are seconds from ScriptPlayer::start()
// and never decrease:
//
//   <t> press <Button>              down for kPressSeconds, then up
//   <t> down <Button>               down until an up line
//   <t> up <Button>
//   <t> hold <Button> <secs>        down now, up after <secs>
//   <t> stick <lx> <ly> <rx> <ry>   each -1..1, held until the next stick line
//   <t> shot <name>                 dump a frame as <name>.png
//   <t> mark <free text>            a note for the log
//   <t> quit                        end the run
//
// Buttons are Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Create and
// the d-pad's Up Down Left Right (core/ui/pad_bits.h). A shot name is 1-48 of
// A-Za-z0-9._- so it is safe as a file name.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace splat {

// How long a `press` holds its button: long enough for a 60 Hz viewer to see
// the edge, short enough to feel like a tap.
constexpr double kPressSeconds = 0.1;

// One parsed line. `press` and `hold` keep their duration; ScriptPlayer derives
// both edges from it, so the parser stays a pure text-to-events step.
struct ScriptEvent {
    enum class Kind { Press, Down, Up, Hold, Stick, Shot, Mark, Quit };

    Kind kind = Kind::Press;
    double time = 0.0;      // seconds from ScriptPlayer::start()
    uint32_t button = 0;    // Press, Down, Up, Hold
    double duration = 0.0;  // Press (kPressSeconds), Hold (the line's secs)
    float lx = 0.0f;        // Stick
    float ly = 0.0f;
    float rx = 0.0f;
    float ry = 0.0f;
    std::string text;       // Shot name, Mark text
};

// A script's parsed events, in line order. Errors from parse_input_script name
// the 1-based line they came from.
class InputScript {
public:
    const std::vector<ScriptEvent>& events() const { return events_; }
    bool empty() const { return events_.empty(); }

    // Seconds from start() until the last event is over: the end of the last
    // press or hold, not just the last line's time.
    double duration() const;

private:
    friend Result<InputScript> parse_input_script(std::string_view text);

    std::vector<ScriptEvent> events_;
};

// Parses the format above. Every error starts with "line <n>: ": an unknown
// button, a bad number, a time that goes backwards, a missing argument, a stick
// outside -1..1 or a shot name that cannot be a file name.
Result<InputScript> parse_input_script(std::string_view text);

// One advance()'s result: the pad state at the sampled time and the one-shot
// events that fell inside the step. `buttons` is the full held word (all
// buttons down at that time), not an edge; the caller derives edges against the
// previous frame, exactly as it does for a real pad.
struct ScriptFrame {
    uint32_t buttons = 0;
    float lx = 0.0f;
    float ly = 0.0f;
    float rx = 0.0f;
    float ry = 0.0f;
    std::vector<std::string> shots;  // `shot` names in script order
    std::vector<std::string> marks;  // `mark` texts in script order
    bool quit = false;
};

// Replays an InputScript against absolute time. Every event whose time has
// passed since the previous advance() fires exactly once, in script order, so a
// long frame (a scene load, a first frame) never skips a shot or a mark. The
// button word returned is the state after all of them: a press shorter than one
// frame is collapsed, like a real pad polled after the fact.
class ScriptPlayer {
public:
    explicit ScriptPlayer(InputScript script);

    // Re-arms the player at t = 0. Called by the constructor.
    void start();

    // `seconds_since_start` is compared with the event times. A value smaller
    // than the previous call is treated as the previous value: time never runs
    // backwards, so no event can fire twice.
    ScriptFrame advance(double seconds_since_start);

    // True once every event has fired and no button is held any more (a `hold`
    // has ended; a `down` with no `up` keeps the player unfinished).
    bool finished() const;

private:
    enum class StepKind { Down, Up, Stick, Shot, Mark, Quit };

    // One primitive on the timeline: a press or a hold is two of these.
    struct Step {
        double time = 0.0;
        StepKind kind = StepKind::Down;
        uint32_t button = 0;
        float lx = 0.0f;
        float ly = 0.0f;
        float rx = 0.0f;
        float ry = 0.0f;
        std::string text;
    };

    void apply(const Step& step, ScriptFrame& frame);

    InputScript script_;
    std::vector<Step> steps_;
    size_t next_ = 0;
    uint32_t buttons_ = 0;
    int held_[32] = {};  // per button bit: 0 = up, N = N overlapping downs
    float lx_ = 0.0f;
    float ly_ = 0.0f;
    float rx_ = 0.0f;
    float ry_ = 0.0f;
    double now_ = 0.0;
    bool quit_ = false;
};

}  // namespace splat
