// The start decision when VR cannot start (platform/ps5/viewer/start_mode.h):
// the table the title runs when the PS VR2 is missing, switched off, or its
// Hmd2/VrTracker2 export tables do not match the build. The user's decision of
// 2026-11-22 is that a missing headset must not block the app
// ("If VR is not connected app shouldn't be blocked from starting"), so every
// failure except the two that have nothing to show - or that the user ended on
// purpose - continues on the TV.
//
// The firmware card's own words are checked in tests/firmware_message_test.cpp;
// this file checks who may continue and who exits, plus the headset status the
// fallback starts from (hmd2.h's headset_on: only status 0 is "on"). No Vulkan,
// no Hmd2, no console: the same headers the title compiles.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include "../platform/ps5/hmd2/hmd2.h"
#include "../platform/ps5/viewer/start_mode.h"

namespace {

viewer::NoVrStart start(bool tv_port, bool firmware_card = false, bool card_shown = false,
                        bool card_continue = false) {
    viewer::NoVrStart out;
    out.tv_port = tv_port;
    out.firmware_card = firmware_card;
    out.card_shown = card_shown;
    out.card_continue = card_continue;
    return out;
}

}  // namespace

TEST_CASE("only device status 0 is a headset that is on") {
    // The measured table (runs/issue-1-crash console klogs, "GetDeviceInformation
    // ->"): status 0 is a headset that is on (worn 0 or 1; sessions get tracked)
    // and status 2 is one that is plugged in but switched off (never tracked).
    // hmd2::init() stops for anything but 0 right after that call, before
    // reprojection and VR mode, so the decision above takes the session to the
    // TV instead of the 300 s pose wait the off headset used to cause.
    CHECK(hmd2::headset_on(0));
    CHECK_FALSE(hmd2::headset_on(2));
    // "Other" is not on either: an unknown status word is not a green light.
    CHECK_FALSE(hmd2::headset_on(1));
    CHECK_FALSE(hmd2::headset_on(3));
    CHECK_FALSE(hmd2::headset_on(0xffffffffu));
    // What vr_mode.cpp does with the failure this causes: no firmware card, the
    // port is open, so the session continues on the TV.
    CHECK(viewer::runs_without_vr(start(true)));
}

TEST_CASE("no headset, or any Hmd2 failure, runs the session on the TV") {
    // The exact case the user reported: PS VR2 not connected. The port is open
    // (the session opened it before Hmd2, tv_output.h) and nothing else matters.
    CHECK(viewer::runs_without_vr(start(true)));
    // The same for any other init failure: a headset switched off, a tracker or
    // reprojection call that fails, an external-buffer refusal. start_mode.h
    // does not see the failure's name, only that it is not the firmware card.
    CHECK(viewer::runs_without_vr(start(true, false, false, false)));
}

TEST_CASE("no TV port means no TV session") {
    // A TV-only session with no VideoOut port has no display at all: the title
    // keeps 0.6.1's exit rather than running blind.
    CHECK_FALSE(viewer::runs_without_vr(start(false)));
    // That holds even for a non-firmware failure, and for a card the user
    // answered X on (there is no port to continue on).
    CHECK_FALSE(viewer::runs_without_vr(start(false, true, true, true)));
}

TEST_CASE("the firmware card's X and its 120 s continue, O exits") {
    // Missing export table: the card is the answer, so the session continues
    // only when the card was shown and said so (X or the timeout).
    CHECK(viewer::runs_without_vr(start(true, true, true, true)));
    // O, or a card that was never presented: the home screen, as through 0.6.1.
    CHECK_FALSE(viewer::runs_without_vr(start(true, true, true, false)));
    CHECK_FALSE(viewer::runs_without_vr(start(true, true, false, false)));
    // A card answered X without being shown cannot happen; it must not open a
    // session either.
    CHECK_FALSE(viewer::runs_without_vr(start(true, true, false, true)));
}

TEST_CASE("the expected no-headset starts are logged without FAIL") {
    // vr_mode.cpp's klog line loses "FAIL" for the two ways a launch without a
    // usable headset is expected to end: the headset is switched off (the
    // measured status-2 step) and there is no headset session to open (the
    // console's 0xffffffff from sceHmd2Open, docs/console-runbook.md). Every
    // real init error keeps it, and so does the firmware card's own path.
    CHECK(hmd2::expected_fallback({"PSVR2 is off", 2, nullptr}));
    CHECK(hmd2::expected_fallback({"sceHmd2Open", -1, nullptr}));
    // Real errors: a module, the tracker, a reprojection or eye-buffer call.
    const int32_t init_refused = static_cast<int32_t>(0x80990001u);
    CHECK_FALSE(hmd2::expected_fallback({"sceHmd2Initialize", init_refused, nullptr}));
    CHECK_FALSE(hmd2::expected_fallback({"sceVrTracker2Initialize", -1, nullptr}));
    CHECK_FALSE(hmd2::expected_fallback({"eye buffer direct memory", -1, nullptr}));
    // A missing export table is the firmware card's start, not a no-headset one.
    CHECK_FALSE(hmd2::expected_fallback({"libSceHmd2", 0, "libSceHmd2"}));
    // An open that failed some other way, and "nothing failed", are not expected.
    const int32_t open_refused = static_cast<int32_t>(0x80990002u);
    CHECK_FALSE(hmd2::expected_fallback({"sceHmd2Open", open_refused, nullptr}));
    CHECK_FALSE(hmd2::expected_fallback({"", 0, nullptr}));
    CHECK_FALSE(hmd2::expected_fallback({nullptr, 0, nullptr}));
}

TEST_CASE("the decision is the four flags and nothing else") {
    // Exhaustive: 2^4 inputs, so a future field cannot change a case by accident
    // without this table. The rule is the three checks in start_mode.h.
    for (int mask = 0; mask < 16; ++mask) {
        const bool tv_port = (mask & 1) != 0;
        const bool firmware_card = (mask & 2) != 0;
        const bool card_shown = (mask & 4) != 0;
        const bool card_continue = (mask & 8) != 0;
        const bool expected =
            tv_port && (!firmware_card || (card_shown && card_continue));
        CAPTURE(mask);
        CHECK(viewer::runs_without_vr(start(tv_port, firmware_card, card_shown, card_continue)) ==
              expected);
    }
}
