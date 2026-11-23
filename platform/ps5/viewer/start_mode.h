// The start decision when VR cannot start: with no PS VR2, a headset that is
// off, or a Hmd2/VrTracker2 init that fails for any reason, the title runs the
// same session on the TV alone instead of returning to the home screen
// (user decision, 2026-11-22: "If VR is not connected app shouldn't be blocked
// from starting. Preview and controlling already works on TV, menus too.").
//
// A release install runs MODE=vr, and run_vr() opens the TV output before it
// brings Hmd2 up (vr_mode.cpp). That ordering is what makes the fallback cheap:
// by the time the init fails, the TV already has the title's own picture and
// the panel canvas is the same one the VR session uses, so the session only has
// to skip the headset side - no Hmd2 init, no eye buffers, no copy into them,
// no submission or reprojection, no mirror - and pace its frames from a timer.
// The camera is a fixed seated head pose at the capture point, so the sticks
// and triggers move the scene exactly as they do in VR.
//
// Two things still end the title as through 0.6.1:
//  - no VideoOut port (the TV output could not be opened): a TV-only session
//    with no TV has nothing to show, so "continue on the TV" cannot be honoured;
//  - the missing-export-table card (issue #2) and the user answered O, or the
//    card never reached the screen. That is the user's answer, not a failure.
// Any other init failure has no better place to be than the TV, and the card's
// X and its 120 s timeout both continue.
//
// Pure and header-only (no Vulkan, no Hmd2, no console), so
// tests/tv_only_test.cpp pins the table the title runs.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace viewer {

struct NoVrStart {
    // A VideoOut port the session can put its picture on (the TV output that
    // was already open, or one opened for the fallback).
    bool tv_port = false;
    // The failure is hmd2::Error::missing_table_library: the firmware card is
    // the answer, not a silent fallback.
    bool firmware_card = false;
    // The card was presented on the TV (a card that failed to present asked
    // nobody anything).
    bool card_shown = false;
    // X, or the card's 120 s timeout: continue on the TV. False for O.
    bool card_continue = false;
};

// True: run the session on the TV only. False: return the VR failure status,
// which puts the console back on the home screen (the 0.6.1 behaviour).
inline bool runs_without_vr(const NoVrStart& start) {
    if (!start.tv_port) return false;
    if (start.firmware_card) return start.card_shown && start.card_continue;
    return true;
}

}  // namespace viewer
