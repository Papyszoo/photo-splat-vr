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
#include <cstddef>
#include <string>

#include "common/path.h"
#include "common/status.h"

namespace splat {

class ILocationService;

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
    // Scale mode: 0 = front (nearest visible surface fixed), 1 = centre (the
    // visible content's 3D centre fixed), 2 = capture (the default: a camera
    // zoom about the photo's capture point; the scene geometry and position do
    // not change, the image shrinks/grows toward the centre). All three scale
    // in place down to kMinSceneScale (0.001x) and grow to 4x.
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

    // ---- thumbnail size (stage T) ------------------------------------------
    // The one size setting for every screen that shows entries as thumbnails
    // (files and folders alike): an index into kThumbnailSizes, 0 smallest ..
    // 4 largest. A change applies at once and every grid re-lays out from
    // core/ui/widgets.h's thumbnail metrics. An older ui.json has no value and
    // loads the default; a hand-edited one is clamped.
    int thumbnail_size = 2;

    // ---- output folder (0.6.0) ---------------------------------------------
    // Where every new scene is saved: "" = this console (the default), else a
    // folder on a network location as a location path ("smb0:/scenes/Made").
    // Settings > Output folder writes it by browsing the location's folders.
    // load_ui_state() keeps only a value valid_output_folder() accepts, so a
    // hand-edited or foreign file falls back to the console.
    std::string output_folder;
};

// The longest output folder this build stores. A path this long is not one the
// folder browser produced.
inline constexpr std::size_t kMaxOutputFolderChars = 1024;

// True for a canonical location path the folder browser could have produced:
// `<id>:/<a>/<b>` with a valid location id (core/vfs/io.h), no empty, "." or
// ".." component, no trailing slash (except the bare root `<id>:/`), no
// backslash or control character, at most kMaxOutputFolderChars. A local path,
// a relative path and "" (the console) are not.
bool valid_output_folder(const std::string& text);

// The canonical spelling of a folder the browser stands in (`smb0:/a//b/` ->
// `smb0:/a/b`), or "" when it cannot be a valid output folder.
std::string canonical_output_folder(const Path& folder);

// Where a new scene goes. A network library the picker was opened for keeps
// winning (the user browsed there on purpose), otherwise the Output folder. In
// both cases the scene lands in `<root>/<album>/<scene>`, the album being the
// shelf's own (empty = the loose scenes, directly in the root): the Output folder
// is a library like the console's, with the same albums. `from_setting` tells
// which of the two applied. An empty `root` is this console's own scene root,
// and `album` then is the shelf's.
struct SceneTarget {
    Path root;
    std::string album;
    bool from_setting = false;
};

// Resolves the target for a conversion or an import started from the shelf
// `shelf_root`/`shelf_album`. Fails, with a text fit for a toast, when the
// Output folder names a network location that is no longer configured (a scene
// is never saved on the console behind the user's back) or when the console
// album's name is not valid_album_name() and so cannot be made on the share.
Result<SceneTarget> scene_target(const UiState* state, const ILocationService* locations,
                                 const Path& shelf_root, const std::string& shelf_album);

// A folder on a network location in plain words, for a row or a destination
// line: "<location name> > a > b" (the folders below the share), the bare name at
// the share's top, "<id> (removed)" when the location is gone. Never the raw
// `smb0:/...` path.
std::string network_folder_text(const ILocationService* locations, const Path& folder);

// The Settings row's value: "Console", or network_folder_text() of the folder.
std::string output_folder_text(const UiState* state, const ILocationService* locations);

// The thumbnail size slider's five steps, smallest to largest, and the default
// (the middle). They are the only accepted values: load_ui_state clamps.
inline constexpr int kThumbnailSizeCount = 5;
inline constexpr int kThumbnailSizeDefault = 2;

// A number read from ui.json to a step. The clamp happens on the number, before
// any integer cast: a hand-edited 1e308 is the largest step, -3 the smallest,
// and neither overflows (lround of an int-sized value would).
inline int thumbnail_size_from_number(double value) {
    if (!(value > 0.0)) return 0;  // negatives, -0, NaN
    if (value >= static_cast<double>(kThumbnailSizeCount - 1)) return kThumbnailSizeCount - 1;
    return static_cast<int>(std::lround(value));
}

inline int clamp_thumbnail_size(int value) {
    return value < 0 ? 0 : (value >= kThumbnailSizeCount ? kThumbnailSizeCount - 1 : value);
}

// The slider's step name ("Smallest", "Small", "Default", "Large", "Largest").
inline const char* thumbnail_size_name(int value) {
    switch (clamp_thumbnail_size(value)) {
        case 0: return "Smallest";
        case 1: return "Small";
        case 2: return "Default";
        case 3: return "Large";
        default: return "Largest";
    }
}

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

// The Settings row's name for a scale mode.
inline const char* scale_mode_name(int scale_around) {
    if (scale_around == 0) return "Front";
    if (scale_around == 1) return "Centre";
    return "Capture";
}

// <state dir>/ui.json
Path ui_state_path(const Path& state_dir);

// Reads the file. Missing, unreadable, oversized or garbage content yields the
// defaults; individual fields of the wrong type are ignored.
UiState load_ui_state(const Path& state_dir);

// Writes the file atomically (temporary + rename), creating the directory.
Status save_ui_state(const Path& state_dir, const UiState& state);

}  // namespace splat
