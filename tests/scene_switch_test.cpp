// VR6: the L1/R1 scene switch must land whatever the one-deep preload slot
// holds (platform/ps5/viewer/scene_switch.h).
//
// The field failure (runs/console_session_10/vr_test3.log, 2026-10-26 23:17
// session): 22 of 61 scene presses did nothing, every one of them an L1 -
// `viewer: vr FAIL stage ...: a scene is already staged (staying on ...)`. The
// user's report: "L1 sometimes did nothing: on scene 13 going back would not
// work, several times." The slot holds the *next* scene, so an R1 always found
// its target staged and committed it, while an L1 (going back) needed a scene
// the slot never had - and the upload was refused while the forward preload
// occupied the slot.
//
// These tests drive the same policy with a fake slot that behaves like
// SplatRenderer (a stage is refused while the slot holds a scene), so the
// console's failure is reproduced on the desktop and cannot come back.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "../platform/ps5/viewer/scene_switch.h"

namespace {

// A one-deep staging slot with SplatRenderer's semantics: upload() while a
// scene is staged fails ("a scene is already staged"); activate() only works
// for the index it holds. `stale_flag` reproduces the field state the log
// shows, where the renderer's buffer flag and the viewer's index disagreed.
class FakeSlot final : public viewer::SwitchSlot {
public:
    explicit FakeSlot(long current) : current_(current) {}

    long staged() const override { return staged_; }
    bool holds_buffers() const override { return buffers_; }

    void release() override {
        ++releases;
        staged_ = -1;
        buffers_ = false;
    }

    bool upload(std::size_t index) override {
        uploads.push_back(index);
        if (buffers_) return false;   // "a scene is already staged"
        if (fail_upload_index == static_cast<long>(index)) return false;
        staged_ = static_cast<long>(index);
        buffers_ = true;
        return true;
    }

    bool activate(std::size_t index) override {
        if (staged_ != static_cast<long>(index)) return false;
        current_ = staged_;
        staged_ = -1;
        buffers_ = false;
        ++activations;
        return true;
    }

    // Test hooks.
    long current() const { return current_; }
    int releases = 0;
    int activations = 0;
    std::vector<std::size_t> uploads;
    long fail_upload_index = -1;
    bool stale_flag = false;

    // Puts a scene in the slot as a finished preload would.
    void stage(long index) {
        staged_ = index;
        buffers_ = true;
    }
    // The renderer's buffers occupied while the viewer's index says empty.
    void forget_index() {
        staged_ = -1;
        buffers_ = true;
    }

private:
    long current_ = 0;
    long staged_ = -1;
    bool buffers_ = false;
};

}  // namespace

TEST_CASE("viewer L1 goes back while the next scene is preloaded") {
    // Scenes 0..5; the user is on 4 and the forward preload holds 5, exactly
    // the state of the failing session (on 13, da3-small staged as 14's).
    FakeSlot slot(4);
    slot.stage(5);

    const viewer::SwitchReport report = viewer::switch_scene(slot, 3);
    REQUIRE(report.switched);
    CHECK(report.used_staged == false);
    CHECK(report.freed_preload);
    CHECK(report.retried == false);
    CHECK(slot.current() == 3);
    CHECK(slot.releases == 1);
    CHECK(slot.staged() == -1);
    // Same single press of R1 afterwards still commits the preload.
    slot.stage(4);
    const viewer::SwitchReport forward = viewer::switch_scene(slot, 4);
    CHECK(forward.switched);
    CHECK(forward.used_staged);
    CHECK(forward.freed_preload == false);
}

TEST_CASE("viewer R1 commits the preloaded scene without touching it") {
    FakeSlot slot(4);
    slot.stage(5);
    const viewer::SwitchReport report = viewer::switch_scene(slot, 5);
    REQUIRE(report.switched);
    CHECK(report.used_staged);
    CHECK(report.freed_preload == false);
    CHECK(slot.releases == 0);
    CHECK(slot.uploads.empty());
    CHECK(slot.current() == 5);
}

TEST_CASE("viewer a press lands while a preload is still being read") {
    // The worker is reading scene 5 but has not finished: the slot is empty
    // (staged() == -1) and holds no buffers. The press for 3 must read it
    // itself - the worker is the caller's problem (it is cancelled there) and
    // the slot must end up holding the scene it was asked for.
    FakeSlot slot(4);
    const viewer::SwitchReport report = viewer::switch_scene(slot, 3);
    REQUIRE(report.switched);
    CHECK(report.used_staged == false);
    CHECK(report.freed_preload == false);
    CHECK(slot.uploads.size() == 1);
    CHECK(slot.uploads[0] == 3);
    CHECK(slot.current() == 3);
}

TEST_CASE("viewer a press lands when the slot's flag and buffers disagree") {
    // The field failure in one object: the viewer's index says the slot is
    // empty while the renderer still holds buffers, so the first upload is
    // refused ("a scene is already staged"). Before VR6 this cost the press;
    // now it is released and retried once.
    FakeSlot slot(4);
    slot.stage(5);
    slot.forget_index();
    const viewer::SwitchReport report = viewer::switch_scene(slot, 3);
    REQUIRE(report.switched);
    CHECK(report.retried);
    CHECK(report.freed_preload == false);   // nothing was known to be in the way
    CHECK(slot.current() == 3);
    CHECK(slot.uploads.size() == 2);
    CHECK(slot.releases == 1);
}

TEST_CASE("viewer a press that cannot read the scene leaves the viewer be") {
    // A read failure is not retried and does not commit anything: the viewer
    // stays on the scene it had and the slot is left empty for the next press.
    FakeSlot slot(4);
    slot.stage(5);
    slot.fail_upload_index = 3;
    const viewer::SwitchReport report = viewer::switch_scene(slot, 3);
    CHECK_FALSE(report.switched);
    CHECK(report.retried == false);   // no buffers held: a real read failure
    CHECK(slot.current() == 4);
    CHECK(slot.staged() == -1);
    // The next press works.
    slot.fail_upload_index = -1;
    const viewer::SwitchReport second = viewer::switch_scene(slot, 3);
    CHECK(second.switched);
    CHECK(slot.current() == 3);
}

TEST_CASE("viewer pressing the scene already on screen does not reload it") {
    // L1/R1 wrap: pressing next on the last scene asks for ... the same index in
    // a one-scene playlist; with two scenes, scene 1's "previous" is scene 0's
    // preloaded 0. Nothing here may stage the live scene again.
    FakeSlot slot(0);
    slot.stage(0);
    const viewer::SwitchReport report = viewer::switch_scene(slot, 0);
    CHECK(report.switched);
    CHECK(report.used_staged);
    CHECK(slot.uploads.empty());
}
