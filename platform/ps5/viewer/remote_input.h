// The PS5 Media Remote (0.7): the user's remote as one more scePad device,
// mapped onto the same button/action table as the DualSense (pad_input.h).
//
// The remote is an ordinary scePad port: type 16 (REMOTE_CONTROL) for the
// system user 0xff (SDL's PS5_Remote_Init and AnyPS5's SystemRemote agree;
// runs/media-remote/scan.out section 1.1). The console probe of 2026-11-23
// (runs/v0.7/console-klog-070*.txt, DECISIONS.md "Update after the console
// probe") settled what it sends on 13.60:
//
//   - the D-pad and Options arrive in the normal 32-bit button word (up 0x10,
//     right 0x20, down 0x40, left 0x80, Options 0x08);
//   - Enter is the key byte 0x0d at 0x6C and the hold is reported (down for
//     3 s); Back is the key byte 0x0f;
//   - play/pause, rewind and fast-forward reached the app not at all.
//
// The mapping (DECISIONS.md, 2026-11-23):
//   D-pad                  -> menu navigation; in a scene left/right are the
//                             previous/next scene (as L1/R1) and up/down do
//                             nothing
//   Enter                  -> Cross (a 1 s hold recentres, as on the pad)
//   Back                   -> Circle
//   Options                -> Options (tap opens the menu, hold recentres)
// No walking, turning or zooming with the remote: it has no stick and no
// trigger, and the console probe showed no other key the title could use.
//
// This header is the pure mapping only: no clock, no I/O, no scePad. The
// viewer's reader (vr_mode.cpp) opens the port and feeds one State here, so
// tests/remote_input_test.cpp checks the exact pad word a DualSense would have
// produced.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "pad_input.h"

namespace viewer {
namespace remote {

// scePadOpen's remote port and the system user (SDL's PS5_Remote_Init).
inline constexpr int kPadType = 16;
inline constexpr int kUserIdSystem = 0xff;

// The ScePadData bytes this reader uses: the connected byte at 0x4C and the
// media key code at 0x6C (SDL reads PS5_PadData's unknown[3]). The key offset
// is what the console probe confirmed: Enter 0x0d, Back 0x0f.
inline constexpr size_t kConnectedOffset = 0x4C;
inline constexpr size_t kKeyOffset = 0x6C;
inline constexpr uint8_t kKeyEnter = 0x0d;
inline constexpr uint8_t kKeyBack = 0x0f;

// One frame's remote state. `connected` is the pad state's connected byte;
// `buttons` the word at +0x00; `key` the media key code at 0x6C (0 when none).
struct State {
    bool connected = false;
    uint32_t buttons = 0;
    uint8_t key = 0;
};

// The pure mapping: one remote state -> the pad button word the DualSense
// would have produced. Only the remote's own four controls are let through
// (D-pad, Enter, Back, Options): a stray bit from another device cannot leak.
// A disconnected remote and a sample the system menu intercepted map to
// nothing, so an unplugged remote cannot leave a button held.
inline uint32_t map(const State& state) {
    if (!state.connected) return 0;
    if ((state.buttons & pad::kIntercepted) != 0) return 0;
    uint32_t out = state.buttons & (pad::kUp | pad::kDown | pad::kLeft | pad::kRight |
                                    pad::kOptions);
    if (state.key == kKeyEnter) out |= pad::kCross;
    if (state.key == kKeyBack) out |= pad::kCircle;
    return out;
}

// The scene role of the same word. The D-pad stays the panel's focus in the
// menu, so this is a second reading of the two bits, not a second button:
// left is the previous scene and right the next (the DualSense's L1/R1). The
// up/down bits have no scene action, and left+right cancel like the old
// digital stick did.
inline uint32_t scene_actions_for(uint32_t buttons) {
    const bool left = (buttons & pad::kLeft) != 0;
    const bool right = (buttons & pad::kRight) != 0;
    if (left == right) return 0;
    return left ? pad::kPrevScene : pad::kNextScene;
}

}  // namespace remote
}  // namespace viewer
