// The panel's small persistent state: `state/ui.json` under the app's data
// directory. It remembers the model the converter uses, the last album the user
// browsed and whether tooltips are on. A missing, unreadable or malformed file
// is not an error: every field falls back to its default, so a hand-edited file
// can never keep the panel from starting.
//
// The host owns one UiState (loaded once at startup); the model service writes
// selected_model through it. The file is replaced atomically, like the model
// state records.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <string>

#include "common/path.h"
#include "common/status.h"

namespace splat {

struct UiState {
    // Backend name the converter would load, "" = nothing selected. There is no
    // default model: the user picks one.
    std::string selected_model;
    // Album folder last browsed, "" = the loose-scene album.
    std::string last_album;
    bool tooltips = true;

    // ---- comfort and display (VR8) -----------------------------------------
    // The on-screen settings are the same values the viewer applies; they are
    // stored as plain numbers/booleans so the VR loop can read them every
    // frame without parsing anything.
    //
    // Scale pivot: 0 = front (5th-percentile depth in the head-forward cone),
    // 1 = centre (median depth in that cone, with the near content kept at
    // least 0.5 m away), 2 = maquette (the default: shrinking glides the scene
    // onto a table point 0.7 m ahead, then scale about that point). VR9.
    int scale_around = 2;
    // Metres/second at full stick (the quadratic curve's peak) and
    // degrees/second at full right stick.
    float move_speed = 2.5f;
    float turn_speed = 40.0f;
    // Screen-space AA low-pass, Mip-Splatting's eps2d in pixels^2: 0.3
    // (Standard, the published value and the default) or 0.5 (Soft).
    float edge_softness = 0.3f;
    // true = linear-light blending (the current sRGB target), false =
    // gamma-space/sRGB blending like Spark. Defaults to the current look.
    bool blend_linear = true;
};

// The values the Settings rows offer, in order, and the clamp a loaded
// ui.json goes through. They are the only accepted numbers: a hand-edited
// file cannot put an arbitrary speed in the viewer.
inline constexpr float kMoveSpeeds[] = {0.75f, 1.5f, 2.5f, 4.0f};
inline constexpr float kTurnSpeeds[] = {20.0f, 40.0f, 60.0f, 90.0f};
inline constexpr float kEdgeSoftness[] = {0.3f, 0.5f};

// The nearest offered value, so a missing/unknown file value falls back to
// the default instead of a speed nothing displays.
inline int nearest_move_speed_index(float value) {
    int best = 2;  // 2.5 m/s
    for (int i = 0; i < 4; ++i) {
        if (std::abs(kMoveSpeeds[i] - value) < std::abs(kMoveSpeeds[best] - value)) best = i;
    }
    return best;
}
inline int nearest_turn_speed_index(float value) {
    int best = 1;  // 40 deg/s
    for (int i = 0; i < 4; ++i) {
        if (std::abs(kTurnSpeeds[i] - value) < std::abs(kTurnSpeeds[best] - value)) best = i;
    }
    return best;
}
inline int nearest_edge_softness_index(float value) {
    return std::abs(kEdgeSoftness[1] - value) < std::abs(kEdgeSoftness[0] - value) ? 1 : 0;
}

// The Settings row's name for a scale mode (0 front, 1 centre, 2 maquette).
inline const char* scale_mode_name(int scale_around) {
    if (scale_around == 1) return "Centre";
    if (scale_around == 2) return "Maquette";
    return "Front";
}

// <state dir>/ui.json
Path ui_state_path(const Path& state_dir);

// Reads the file. Missing, unreadable, oversized or garbage content yields the
// defaults; individual fields of the wrong type are ignored.
UiState load_ui_state(const Path& state_dir);

// Writes the file atomically (temporary + rename), creating the directory.
Status save_ui_state(const Path& state_dir, const UiState& state);

}  // namespace splat
