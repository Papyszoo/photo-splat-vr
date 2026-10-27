// L1/R1 scene switching over the renderer's one-deep preload slot.
//
// The renderer has one staging buffer set next to the live scene: a scene is
// uploaded into it and then committed (a descriptor swap), which is how a
// switch costs 0.2 ms instead of ~95 ms. The viewer keeps the *next* playlist
// scene in it while the user looks at the current one.
//
// That slot is one deep and directional: it holds the next scene, never the
// previous one. VR5's switch only committed the slot when it happened to hold
// the requested index and otherwise fell through to an on-demand read - which
// calls stage_scene() while the forward preload still occupies the slot, is
// refused ("a scene is already staged"), and leaves the viewer on the current
// scene. The user's session lost 22 of 61 scene presses that way, every one of
// them an L1 (runs/console_session_10/vr_test3.log; the "several times on scene
// 13" report). R1 in the same state always worked, because R1's target *was*
// the staged scene.
//
// The rule this file encodes: **the forward preload never vetoes a switch.**
// Anything in the slot that is not the requested scene is released first, the
// requested scene is uploaded (or the worker's already-read copy is used), and
// then committed. A slot whose own bookkeeping still claims to be occupied -
// the exact refusal in the log - is released and uploaded once more.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace viewer {

// The one-deep staging slot as the switch sees it. vr_mode.cpp implements this
// over SplatRenderer + ScenePreloader; tests/scene_switch_test.cpp over a fake
// that behaves like SplatRenderer::stage_scene() (a second stage is refused).
class SwitchSlot {
public:
    virtual ~SwitchSlot() = default;
    // Playlist index the slot currently holds, -1 when it is empty.
    virtual long staged() const = 0;
    // True while the slot holds staged GPU buffers, even when no playlist index
    // is known for them: this is the state the renderer's own refusal
    // ("a scene is already staged") reports, and the reason switch_scene()
    // retries once after a failed upload.
    virtual bool holds_buffers() const = 0;
    // Frees the slot. The live scene and its GPU buffers are untouched, which
    // is what makes this safe on the frame of an input press.
    virtual void release() = 0;
    // Makes the slot hold `index`: reads the file (or takes the preload
    // worker's result) and uploads it. False on a read or upload failure.
    virtual bool upload(std::size_t index) = 0;
    // Commits the slot's scene, which must be `index`. False if it is not.
    virtual bool activate(std::size_t index) = 0;
};

// What happened, for the per-switch log line.
struct SwitchReport {
    bool switched = false;
    bool used_staged = false;   // the slot already held the target: a commit
    bool freed_preload = false;  // the forward preload was in the way and dropped
    bool retried = false;        // the slot still claimed to be occupied once
};

// Switches to playlist `target` whatever the slot holds. The caller applies the
// new scene (playlist index, median depth, label) only when `switched`.
inline SwitchReport switch_scene(SwitchSlot& slot, std::size_t target) {
    SwitchReport report;
    const long held = slot.staged();
    if (held >= 0 && static_cast<std::size_t>(held) == target) {
        // The forward preload is exactly what was asked for: commit it.
        if (slot.activate(target)) {
            report.switched = true;
            report.used_staged = true;
            return report;
        }
        // It would not commit: fall through to a fresh upload.
        slot.release();
    } else if (held >= 0) {
        // The forward preload is not the target (an L1 that goes back, or two
        // presses within one preload). It is not a veto: drop it.
        report.freed_preload = true;
        slot.release();
    }
    if (!slot.upload(target)) {
        // A slot that still holds buffers is released once more: the renderer's
        // own "a scene is already staged" refusal must not cost the press (this
        // is the field failure this file exists for). A read error leaves the
        // slot empty and is not retried.
        if (!slot.holds_buffers()) return report;
        report.retried = true;
        slot.release();
        if (!slot.upload(target)) return report;
    }
    report.switched = slot.activate(target);
    return report;
}

}  // namespace viewer
