// PSVR2 hot-plug (platform/ps5/viewer/hotplug.h): the rules a session uses to
// follow the headset in both directions (0.7): a TV-only session adopts one
// that was turned on after the session started ("If I now connect PSVR2 will
// it work?"), and a VR session whose headset is turned off comes back to the
// TV.
//
// The watch (hmd2::headset_ready / hmd2::device_status, vr_mode.cpp) is the
// only part that needs a console; everything that decides is in the header and
// is pinned here: only a TV-only, switchable session adopts a headset, only on
// a headset the probe has seen on, and never twice inside the retry window;
// only a VR session and only a status the watch has actually seen leave 0 goes
// back to the TV. The firmware-without-an-export-table case (the issue #2 card,
// X -> TV) is the one that must never switch.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include "../platform/ps5/viewer/hotplug.h"

namespace {

viewer::HotplugState plugged(bool tv_only = true, bool switchable = true,
                             double available_s = 10.0, double now_s = 10.0,
                             double last_attempt_s = -1.0,
                             double retry_s = viewer::kHotplugRetrySeconds) {
    viewer::HotplugState state;
    state.tv_only = tv_only;
    state.switchable = switchable;
    state.available_s = available_s;
    state.now_s = now_s;
    state.last_attempt_s = last_attempt_s;
    state.retry_s = retry_s;
    return state;
}

}  // namespace

TEST_CASE("a headset the probe has seen on switches a TV-only session") {
    CHECK(viewer::hotplug_should_switch(plugged()));
    // The probe's first answer can be later than the session start; the time it
    // was seen only has to exist.
    CHECK(viewer::hotplug_should_switch(plugged(true, true, 0.0, 0.0)));
}

TEST_CASE("no switch without a TV-only session or a switchable firmware") {
    // A VR session does not probe at all, but the rule must say so too.
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(false, true)));
    // The missing-export-table card answered X: the headset would fail the same
    // way it failed at start (0.7 brief, rule 4).
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(true, false)));
    // Both wrong is still no switch.
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(false, false)));
}

TEST_CASE("a headset that is off or absent never switches") {
    // -1 is the probe's "not available": plugged in but off (status 2), gone, or
    // an Hmd2 call that failed. No attempt, however long the session ran.
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(true, true, -1.0)));
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(true, true, -1.0, 100000.0, -1.0)));
}

TEST_CASE("a failed switch waits the retry window, then tries again") {
    constexpr double kRetry = 30.0;
    // The attempt at t=10 did not end in VR; the probe still sees the headset.
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(true, true, 10.0, 10.0, 10.0, kRetry)));
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(true, true, 10.0, 39.9, 10.0, kRetry)));
    // Exactly the window is the first allowed moment (the same `<` the header
    // uses, so the boundary is deliberate).
    CHECK(viewer::hotplug_should_switch(plugged(true, true, 10.0, 40.0, 10.0, kRetry)));
    CHECK(viewer::hotplug_should_switch(plugged(true, true, 10.0, 100.0, 10.0, kRetry)));
    // A clock that went backwards cannot unlock an attempt either.
    CHECK_FALSE(viewer::hotplug_should_switch(plugged(true, true, 10.0, 5.0, 10.0, kRetry)));
}

TEST_CASE("the retry window is a parameter, the product default is 30 s") {
    CHECK(viewer::kHotplugRetrySeconds == doctest::Approx(30.0));
    CHECK(viewer::kHotplugProbeSeconds == doctest::Approx(2.0));
    // A test (or a future setting, which this release does not add) can pass its
    // own window; zero means the next frame may try again.
    CHECK(viewer::hotplug_should_switch(plugged(true, true, 10.0, 10.0, 10.0, 0.0)));
}

TEST_CASE("the decision table is the four inputs") {
    // Exhaustive over the booleans; no attempt has been made yet, so the retry
    // window never limits a case. A future field cannot change a case by
    // accident without this table.
    for (int mask = 0; mask < 8; ++mask) {
        const bool tv_only = (mask & 1) != 0;
        const bool switchable = (mask & 2) != 0;
        const bool available = (mask & 4) != 0;
        CAPTURE(mask);
        CHECK(viewer::hotplug_should_switch(
                  plugged(tv_only, switchable, available ? 10.0 : -1.0, 20.0, -1.0)) ==
              (tv_only && switchable && available));
    }
}

TEST_CASE("a headset status that left 0 takes a VR session back to the TV") {
    viewer::VrOffState off;
    off.vr = true;
    off.off_s = 10.0;
    CHECK(viewer::hotplug_should_leave_vr(off));
    // The first answer is enough, however late it came.
    off.off_s = 0.0;
    CHECK(viewer::hotplug_should_leave_vr(off));
}

TEST_CASE("the VR -> TV rule never acts on a headset that is on or unknown") {
    viewer::VrOffState off;
    // A TV-only session has no Hmd2 session to leave; the rule says so too.
    off.vr = false;
    off.off_s = 10.0;
    CHECK_FALSE(viewer::hotplug_should_leave_vr(off));
    // -1 is the watch's "no answer, or the status is 0": a headset that is on,
    // a failed status call and the time before the first probe all never drop
    // VR.
    off.vr = true;
    off.off_s = -1.0;
    CHECK_FALSE(viewer::hotplug_should_leave_vr(off));
}

TEST_CASE("the VR -> TV decision table is the two inputs") {
    for (int mask = 0; mask < 4; ++mask) {
        const bool vr = (mask & 1) != 0;
        const bool seen_off = (mask & 2) != 0;
        viewer::VrOffState off;
        off.vr = vr;
        off.off_s = seen_off ? 10.0 : -1.0;
        CAPTURE(mask);
        CHECK(viewer::hotplug_should_leave_vr(off) == (vr && seen_off));
    }
}
