// PSVR2 hot-plug (0.7): a TV-only session adopts the headset when it is turned
// on, and a VR session goes back to the TV when it is turned off, instead of
// needing a restart.
//
// The user asked "If I now connect PSVR2 will it work?" about the 0.6.2 TV-only
// session, which picked VR or TV once at start. The answer is now yes: while
// the session runs without Hmd2 (start_mode.h), a probe thread calls
// hmd2::headset_ready() every kHotplugProbeSeconds - off the render thread, so
// a slow Hmd2 call cannot cost a frame - and records the time it first sees a
// headset that is on. Once a frame the render thread asks this header's pure
// rule whether to start the switch. The switch itself (Hmd2 init, the stereo
// renderer, the waiting-for-the-headset placement) is vr_mode.cpp's job and
// runs on the render thread, between two frames.
//
// The reverse is the same watch on a live session: while Hmd2 is open, a thread
// calls hmd2::device_status() every kHotplugProbeSeconds and records the first
// answer that is not status 0 (the start's own rule: status 0 = on, status 2 =
// plugged in but switched off; worn is not used - taking the headset off while
// it stays on is normal). hotplug_should_leave_vr() then asks the render thread
// to tear VR down and come up on the TV, keeping the scene, menu, downloads and
// conversion jobs. A call that fails is not an answer: only a successful status
// read that is not 0 counts, so a transient Hmd2 error cannot drop VR.
//
// The rule is deliberately small, and the tests pin it:
//  - only a session that is on the TV, and only when the switch is possible at
//    all. A firmware with no export table (the issue #2 card, X -> TV) gets no
//    probe and no Hmd2 call after the start - vr_mode.cpp does not start the
//    watch there - and is never switched: the headset would fail the same way
//    it did at start, and the card already answered the question;
//  - a headset the probe has not seen on is no reason to touch Hmd2;
//  - after an attempt that did not end in VR, the next one waits
//    kHotplugRetrySeconds: a switch that keeps failing must neither fill the
//    klog nor open/close Hmd2 in a loop;
//  - the VR -> TV rule only acts in VR and only on a status the watch has
//    actually seen leave 0.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace viewer {

// The probe's own rate. Two seconds is the brief's example: fast enough that
// putting the headset on feels immediate, slow enough that the Hmd2
// open/status/close cycle is nothing next to the frame loop.
inline constexpr double kHotplugProbeSeconds = 2.0;

// How long a failed switch waits before the next attempt. Thirty seconds keeps
// a persistent Hmd2 failure to two outcome lines a minute; the first attempt is
// never delayed (the probe's first answer is immediate).
inline constexpr double kHotplugRetrySeconds = 30.0;

// Everything the decision needs. Times are seconds on one clock (now_ms()/1000
// in the title); -1 means "never" for the two timestamps.
struct HotplugState {
    // The session is running without Hmd2 and could still adopt one.
    bool tv_only = false;
    // A switch is possible at all: not the missing-export-table firmware. False
    // also freezes any switch, whatever the other fields say.
    bool switchable = false;
    // When the probe last saw a headset that is on; -1 when it has not. The
    // probe clears it when the headset answers anything else, so an unplugged
    // or switched-off headset cannot leave a stale "available" behind.
    double available_s = -1.0;
    double now_s = 0.0;
    // The last switch attempt, successful or not; -1 before the first.
    double last_attempt_s = -1.0;
    // How long after an attempt the next one may start. The default is the
    // product constant; a test passes its own.
    double retry_s = kHotplugRetrySeconds;
};

// True when the session should start the switch now. Pure: no clock, no Hmd2,
// no thread - vr_mode.cpp supplies the state and does the work.
inline bool hotplug_should_switch(const HotplugState& state) {
    if (!state.tv_only || !state.switchable) return false;
    if (state.available_s < 0.0) return false;
    if (state.last_attempt_s >= 0.0 && state.now_s - state.last_attempt_s < state.retry_s)
        return false;
    return true;
}

// The VR session's watch: the device status of the open Hmd2 session is polled
// off the render thread (hmd2::device_status, vr_mode.cpp), and `off_s` is the
// first probe that answered with a status other than 0, or -1 while the headset
// is on or no answer has arrived. Only a session that is in VR acts. A failed
// status call is not recorded as off: the watch writes -1 then, so only an
// answer Hmd2 actually gave can drop VR.
struct VrOffState {
    bool vr = false;
    double off_s = -1.0;
};

inline bool hotplug_should_leave_vr(const VrOffState& state) {
    return state.vr && state.off_s >= 0.0;
}

}  // namespace viewer
