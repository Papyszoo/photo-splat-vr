// W34: viewer.env's defaults in a no-viewer.env run, which is what a release
// package ships. v0.1 leaked the dev bench's 30-minute auto-exit into the
// default (SECONDS absent became 1800), so the title quit to the home screen
// without warning. The rule now lives in platform/ps5/viewer/viewer_env_defaults.h
// so main.cpp and this test read the same code.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "../platform/ps5/viewer/viewer_env_defaults.h"

TEST_CASE("vr defaults leave the session without an auto-exit") {
    // No viewer.env: SECONDS absent. 0 means "no cap", never the bench's 1800 s.
    const viewer::VrDefaults absent = viewer::apply_vr_defaults(false, 60, false, 0.25f);
    CHECK(absent.seconds == 0);
    CHECK(absent.comfort_radius == splat::kDefaultVrComfortRadius);

    // A dev viewer.env may still cap the run.
    CHECK(viewer::apply_vr_defaults(true, 1800, true, 0.5f).seconds == 1800);
    // SECONDS=0 is the same contract as an absent key: no auto-exit.
    CHECK(viewer::apply_vr_defaults(true, 0, false, 0.0f).seconds == 0);

    // A given comfort radius is kept, whatever it is.
    CHECK(viewer::apply_vr_defaults(false, 0, true, 0.75f).comfort_radius == 0.75f);
}

TEST_CASE("the vr session cap is zero or the seconds given") {
    CHECK(viewer::vr_cap_seconds(0) == 0);
    CHECK(viewer::vr_cap_seconds(-5) == 0);
    CHECK(viewer::vr_cap_seconds(1) == 1);
    CHECK(viewer::vr_cap_seconds(1800) == 1800);
}
