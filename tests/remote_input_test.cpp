// The PS5 Media Remote (0.7): the pure mapping from a remote state to the pad
// input a DualSense would have produced (platform/ps5/viewer/remote_input.h),
// after the console probe of 2026-11-23 (DECISIONS.md, "Update after the
// console probe").
//
// Everything that can be decided offline is pinned here: the port, the key
// byte and its two codes (Enter 0x0d, Back 0x0f), the D-pad and Options in the
// button word, the whitelist (no other pad button may leak), Enter -> Cross
// with the same one-second recentre, Back -> Circle, Options -> Options, the
// D-pad left/right -> previous/next scene in a scene, and the panel map seeing
// the same word. No clock, no I/O, no console.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>

#include "../platform/ps5/viewer/pad_input.h"
#include "../platform/ps5/viewer/remote_input.h"

namespace {

// One remote state: `buttons` is the word the remote delivers (already the pad
// layout), `key` the media key byte at 0x6C.
viewer::remote::State remote_state(uint32_t buttons = 0, uint8_t key = 0,
                                   bool connected = true) {
    viewer::remote::State state;
    state.connected = connected;
    state.buttons = buttons;
    state.key = key;
    return state;
}

// The mapped word from a connected remote.
uint32_t map_rest(uint32_t buttons, uint8_t key = 0) {
    return viewer::remote::map(remote_state(buttons, key));
}

}  // namespace

TEST_CASE("viewer the Media Remote is the remote scePad port for the system user") {
    // SDL's PS5_Remote_Init and AnyPS5 agree; scan.out 1.1. A change here would
    // open the DualSense's port instead.
    CHECK(viewer::remote::kPadType == 16);          // REMOTE_CONTROL
    CHECK(viewer::remote::kUserIdSystem == 0xff);   // system
    // The key byte SDL reads as PS5_PadData.unknown[3]; the console probe
    // confirmed it carries Enter and Back.
    CHECK(viewer::remote::kKeyOffset == 0x6C);
    CHECK(viewer::remote::kConnectedOffset == 0x4C);
    // The two codes the probe saw: Enter (the hold is reported) and Back.
    CHECK(viewer::remote::kKeyEnter == 0x0d);
    CHECK(viewer::remote::kKeyBack == 0x0f);
}

TEST_CASE("viewer the Media Remote's D-pad and Options map to the same pad bits") {
    using viewer::pad::kDown;
    using viewer::pad::kLeft;
    using viewer::pad::kOptions;
    using viewer::pad::kRight;
    using viewer::pad::kUp;

    // D-pad keeps its meaning, so the panel's navigation is the pad's own.
    CHECK(map_rest(kUp) == kUp);
    CHECK(map_rest(kDown) == kDown);
    CHECK(map_rest(kLeft) == kLeft);
    CHECK(map_rest(kRight) == kRight);
    CHECK(map_rest(kOptions) == kOptions);
    // A combination gives exactly its mapped bits.
    CHECK(map_rest(kUp | kLeft | kOptions) == (kUp | kLeft | kOptions));
}

TEST_CASE("viewer Enter and Back are the key byte, not button-word bits") {
    using viewer::pad::kCircle;
    using viewer::pad::kCross;
    using viewer::pad::kDown;

    // The console probe: Enter = 0x0d, Back = 0x0f; the button word does not
    // carry them. A Cross/Circle bit in the word is not the remote's.
    CHECK(map_rest(0, viewer::remote::kKeyEnter) == kCross);
    CHECK(map_rest(0, viewer::remote::kKeyBack) == kCircle);
    CHECK(map_rest(viewer::pad::kCross) == 0);
    CHECK(map_rest(viewer::pad::kCircle) == 0);
    // The key does not replace the word: Options + Enter is both.
    CHECK(map_rest(viewer::pad::kOptions, viewer::remote::kKeyEnter) ==
          (viewer::pad::kOptions | kCross));
    CHECK(map_rest(kDown, viewer::remote::kKeyBack) == (kDown | kCircle));
    // A media key the title has no use for invents no button: play/pause (22,
    // 26, 45), rewind 23, fast-forward 24, stop 25.
    for (const uint8_t key : {22, 23, 24, 25, 26, 45}) {
        CAPTURE(key);
        CHECK(map_rest(0, key) == 0);
    }
}

TEST_CASE("viewer no other pad button leaks through the Media Remote") {
    // Create, Square, Triangle, L1/R1, L2/R2, L3/R3 have no remote control.
    CHECK(map_rest(viewer::pad::kCreate | viewer::pad::kSquare | viewer::pad::kTriangle |
                   viewer::pad::kL1 | viewer::pad::kR1 | viewer::pad::kL2 |
                   viewer::pad::kR2 | viewer::pad::kL3 | viewer::pad::kR3) == 0);
    // The system menu's intercepted bit means the same on every pad: the whole
    // sample is neutral while the PS menu is up, Enter/Back included.
    CHECK(map_rest(viewer::pad::kUp | viewer::pad::kOptions | viewer::pad::kIntercepted) == 0);
    CHECK(map_rest(viewer::pad::kIntercepted, viewer::remote::kKeyEnter) == 0);
}

TEST_CASE("viewer the Media Remote's D-pad left/right step the scene") {
    using viewer::pad::kDown;
    using viewer::pad::kLeft;
    using viewer::pad::kNextScene;
    using viewer::pad::kPrevScene;
    using viewer::pad::kRight;
    using viewer::pad::kUp;

    // In a scene the two D-pad directions are the DualSense's L1/R1: previous
    // and next. The same bits still reach the panel, where they are focus.
    CHECK(viewer::remote::scene_actions_for(map_rest(kLeft)) == kPrevScene);
    CHECK(viewer::remote::scene_actions_for(map_rest(kRight)) == kNextScene);
    // Up/down have no scene action, and left+right cancel.
    CHECK(viewer::remote::scene_actions_for(map_rest(kUp)) == 0);
    CHECK(viewer::remote::scene_actions_for(map_rest(kDown)) == 0);
    CHECK(viewer::remote::scene_actions_for(map_rest(kLeft | kRight)) == 0);
    // The scene actions are exactly the L1/R1 ones the existing table gives.
    CHECK(viewer::remote::scene_actions_for(map_rest(kLeft)) ==
          viewer::pad::actions_for(viewer::pad::kL1));
    CHECK(viewer::remote::scene_actions_for(map_rest(kRight)) ==
          viewer::pad::actions_for(viewer::pad::kR1));
}

TEST_CASE("viewer the Media Remote reaches the panel's one input map") {
    using viewer::pad::kCircle;
    using viewer::pad::kCross;
    using viewer::pad::kDown;
    using viewer::pad::kLeft;
    using viewer::pad::kOptions;
    using viewer::pad::kRight;
    using viewer::pad::kUp;

    // The panel takes the mapped word through pad::ui_actions_for, the same
    // call a DualSense word takes: D-pad navigation, Enter = Select, Back =
    // Back, Options = Menu. There is no page key on the remote.
    CHECK(viewer::pad::ui_actions_for(map_rest(kUp)) == splat::Up);
    CHECK(viewer::pad::ui_actions_for(map_rest(kDown)) == splat::Down);
    CHECK(viewer::pad::ui_actions_for(map_rest(kLeft)) == splat::Left);
    CHECK(viewer::pad::ui_actions_for(map_rest(kRight)) == splat::Right);
    CHECK(viewer::pad::ui_actions_for(map_rest(0, viewer::remote::kKeyEnter)) == splat::Select);
    CHECK(viewer::pad::ui_actions_for(map_rest(0, viewer::remote::kKeyBack)) == splat::Back);
    CHECK(viewer::pad::ui_actions_for(map_rest(kOptions)) == splat::Menu);

    // Enter is the pad's Cross, so the scene's one-second recentre hold reads
    // the same held bit a DualSense Cross press produces.
    CHECK((map_rest(0, viewer::remote::kKeyEnter) & kCross) != 0);
    viewer::pad::RecentreHold hold;
    viewer::pad::RecentreHold::Step step = hold.update(true, 10.0);
    CHECK_FALSE(step.completed);
    step = hold.update(true, 11.0);
    CHECK(step.completed);
    // Back is Circle and cannot recentre; a media key is not Cross either.
    CHECK((map_rest(0, viewer::remote::kKeyBack) & kCross) == 0);
    CHECK((map_rest(0, viewer::remote::kKeyEnter) & kCircle) == 0);
}

TEST_CASE("viewer a disconnected Media Remote maps to nothing") {
    // An unplugged remote must not leave a button down: the whole word,
    // Enter/Back included, maps to nothing, and no scene action is produced.
    const uint32_t word = viewer::remote::map(
        remote_state(viewer::pad::kUp | viewer::pad::kLeft | viewer::pad::kOptions,
                     viewer::remote::kKeyEnter, false));
    CHECK(word == 0);
    CHECK(viewer::remote::scene_actions_for(word) == 0);
}
