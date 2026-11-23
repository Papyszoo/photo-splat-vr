// The viewer title's VR mode (MODE=vr, the default): see vr_mode.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace viewer {

// viewer.env keys the VR path uses. The TV path's SIZE/FLIP_RATE/SWAY_PERIOD
// keys have no meaning here: the eye buffers are the headset's size and Hmd2
// owns the cadence.
struct VrOptions {
    std::string scene;        // SCENE=<folder>, empty = the gallery's first
    std::string scenes_dir;   // SCENES_DIR
    std::string shader_dir;   // the directory with the six .spv modules
    int cap_seconds = 0;      // SECONDS; 0 = no auto-exit (viewer.env only)
    float comfort_radius = 0.25f;  // COMFORT_RADIUS, metres
    float world_scale = 1.0f;      // WORLD_SCALE
    bool preload = true;           // PRELOAD=0 turns the next-scene preload off
    // MIRROR=0: no TV mirror. Otherwise (the default) Hmd2's social screen
    // shows the headset's view on the TV, which is what a system recording
    // captures. It is the TV only when the title's own output (tv_output below)
    // is off or could not be opened.
    bool mirror = true;
    // TV_OUTPUT=0: no TV output of the title's own (viewer.env, on by default).
    // The TV then shows the Hmd2 mirror as through 0.5.x. With it on, the title
    // opens the main VideoOut port at the start of the session and puts the
    // scene without the VR window or HUD there, plus the flat menu while it is
    // open (tv_output.h); a port that will not open falls back to the mirror.
    bool tv_output = true;
    // kern.sdk_version as main.cpp logged it (0x0A010000 for 10.01); 0 when it
    // could not be read. Only the "no export table" card's title uses it
    // (firmware_message.h): the tables themselves are chosen by anchor, never
    // by version.
    uint32_t firmware = 0;
};

// Runs one VR session against the headset and returns the title's exit status:
// 0 for a normal end (the panel's Quit, the quit file or SECONDS), 1 when no
// usable pose arrived or the app's own pass check failed, 2-8 for the setup
// steps and the GPU failures, as the TV path numbers them. All logging is
// `viewer: ...`.
//
// The headset is optional (2026-11-22): when Hmd2 cannot start - no PS VR2, a
// headset that is off, a firmware whose libSceHmd2/libSceVrTracker2 export
// tables do not match this build, or any other init failure - the same session
// continues on the TV alone instead of returning 6 to the home screen
// (start_mode.h). There is then no eye buffer, no submission and no
// reprojection, the TV output opened for the session is the only display, the
// frames are paced by a timer, and the camera is a fixed seated head pose at
// the capture point; the DualSense drives the menu and the scene exactly as in
// VR. Only two cases still return 6: no VideoOut port to run on, and the
// missing-export-table card answered with O (its X and its 120 s continue).
//
// No scene anywhere in the gallery is not a failure: the session comes up with
// an empty view and the panel's folders home, so a fresh install can pick or
// add a scene. An empty gallery never ends the title. A boot scene that will
// not read (corrupt, truncated, unreadable, another format) is skipped for the
// next one in the album, and when none reads the session takes that same
// empty-view path; a scene that fails later leaves the previous view up.
//
// The session can write /data/ps5-splat-vr/logs/session-<UTC>.csv (one file per
// run, one row per submitted frame, flushed every second and size-capped) for
// matching a recorded video; viewer.env's POSE_LOG=1 turns it on, off is the
// default (a per-frame /data write on the render thread). See
// docs/console-runbook.md, step 1d, for the columns. A /data that refuses the
// write only costs that file.
//
// The session does its own teardown on every path (the pads and the Sense
// controllers closed, VR mode off, buffers unregistered, display/Hmd2/tracker
// closed, Vulkan destroyed unless a GPU wait timed out), so the caller only has
// to end the title.
int run_vr(const VrOptions& options);

}  // namespace viewer
