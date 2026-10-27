// viewer.env's mode-dependent defaults, applied once after every key has been
// read (main.cpp). A release package ships no viewer.env, so a fresh install
// runs on exactly these values.
//
// Two of them are the product's, not the bench's:
//  - SECONDS absent means no auto-exit (0). v0.1 defaulted to the dev bench's
//    30-minute cap, so the title quit to the home screen half an hour in with
//    no warning. A dev run still caps itself by setting SECONDS in viewer.env.
//  - COMFORT_RADIUS absent keeps the spec's standing radius (2.0 m) in VR,
//    while the TV path keeps 0.25 m: the VR user stands at the capture point.
//
// Pure header-only (no console, no Vulkan), so tests/viewer_env_test.cpp drives
// the same code the title compiles.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/viewer_state.h"

namespace viewer {

struct VrDefaults {
    int seconds = 0;  // 0 = no auto-exit
    float comfort_radius = splat::kDefaultVrComfortRadius;
};

// MODE=vr, after the keys were read: a given SECONDS is kept as it is (0 = no
// cap), an absent one becomes 0, and an absent COMFORT_RADIUS becomes the VR
// standing radius.
inline VrDefaults apply_vr_defaults(bool seconds_given, int seconds, bool comfort_given,
                                    float comfort_radius) {
    VrDefaults defaults;
    defaults.seconds = seconds_given ? seconds : 0;
    defaults.comfort_radius = comfort_given ? comfort_radius : splat::kDefaultVrComfortRadius;
    return defaults;
}

// The VR session's cap from the SECONDS value: 0 or less means the session has
// no cap at all (the panel's Quit, the quit file and a lost pose still end it).
inline int vr_cap_seconds(int seconds) { return seconds > 0 ? seconds : 0; }

}  // namespace viewer
