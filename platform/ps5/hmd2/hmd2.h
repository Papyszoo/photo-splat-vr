// hmd2 - PS VR2 output for a PS5 payload, without a GPU API.
//
// The frame path is PS5VR's (GPL-3.0; see THIRD_PARTY_NOTICES.md): libSceHmd2 /
// libSceVrTracker2 / libSceVrHand are loaded at runtime and their functions
// found by sceKernelDlsym or, where the library does not export them, at this
// firmware's offsets after an anchor-string check (hmd2_exports.h, generated
// from tools/export-tables/tables.json). The picture goes to the headset the
// way PS VR2 titles submit it: a VideoOut port opened with bus type 32, three
// side-by-side eye buffers registered as type 0x10 layers, and per frame
// sceHmd2ReprojectionBeginFrame / GetPredictedDisplayTime / SetParam plus
// sceVideoOutSubmitFlip. The pacing, vblank gate and abandoned-frame re-submit
// are HLA-PSVR2's (GPL-3.0-or-later, Copyright (C) 2026 theghostonline).
//
// What is ours: the eye buffers are plain direct memory that this library
// fills from the CPU (64 KiB R_X tiles, B8G8R8A8) instead of AGC render
// targets, so a payload can draw without linking AGC or RADV. A title that
// renders with RADV can hand over its own host-visible buffers instead
// (Config::external): three linear RGBA8 images from vkMapMemory, the layout
// HLA-PSVR2 submits on the same console.
//
// The frame cadence is HLA-PSVR2's: the frame interval and the predicted
// display time from Hmd2 pace the loop, and a frame that was begun but never
// submitted is shown again with the last image so Hmd2's queue always frees.
//
// Typical use:
//   hmd2::set_logger(line);
//   if (!hmd2::init({})) return 1;
//   while (...) {
//       if (!hmd2::wait_slot() || !hmd2::begin_frame()) break;
//       hmd2::EyeBuffer eye = hmd2::acquire();
//       draw(eye);                       // hmd2::fill_rect / put_pixel
//       if (!hmd2::submit()) break;
//   }
//   hmd2::shutdown();
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hmd2 {

// One line per event: module loading, each Hmd2/VideoOut call's return code,
// buffer registration, frame errors. Never NULL-checked, so set it first.
using Logger = void (*)(const char *line);
void set_logger(Logger logger);

// One caller-owned eye image. `bytes` must cover 2*width*height*4 (tightly
// packed, no padding).
struct ExternalImage {
    void *address;
    size_t bytes;
};

struct Config {
    // Hmd2 user. -1 asks the console for the first logged-in user and falls
    // back to 0xff (the id PS5VR passes) when none is reported.
    int user_id = -1;
    // Eye buffer size per eye, square; the side-by-side buffer is 2x this
    // wide. Must be a multiple of 128 (the tile width).
    uint32_t eye_size = 1024;
    // Registered buffers. 3 is what PS5VR uses (one being scanned out, one
    // being written, one queued).
    int buffers = 3;

    // Eye images the caller owns, in place of the tiled direct memory above.
    // Each is one linear RGBA8 side-by-side image (2 * per-eye width x height
    // x 4 bytes) at a 256-byte-aligned address Hmd2 can sample -- on this
    // console the pointer vkMapMemory returned for a RADV host-visible buffer
    // (HLA-PSVR2's path). When set, the library allocates no eye memory,
    // registers these, and acquire() returns them; `buffers` is replaced by
    // `external_count`, and the per-eye size by image_width / 2 by
    // image_height. Nothing in the external path is assumed square.
    const ExternalImage *external = nullptr;
    int external_count = 0;
    uint32_t image_width = 0;    // side-by-side: 2 * per-eye width
    uint32_t image_height = 0;

    // The TV mirror (Hmd2's social screen): the compositor shows the headset's
    // view on the TV, so a system recording of the title captures it. Off by
    // default -- a library default that changes what the TV shows is the
    // caller's decision. Applied before VR mode; a refusal costs the TV, never
    // the session (mirroring() says whether it is on).
    bool mirror = false;
};

struct Pose {
    float pos[3];   // tracker space, metres
    float quat[4];  // x, y, z, w: head orientation in tracker space
    bool tracked;   // false: pos is 0 and quat identity (no tracking result)
};

// The three poses of one tracker result: the headset's device pose and the two
// eye poses, all in tracker space. `left`/`right` are untracked when the
// tracker reported no valid per-eye pose or an implausible one (the library
// checks that the two points are a head's width apart, 2-12 cm); the caller can
// then fall back to `head` +- the headset's IPD.
struct Poses {
    Pose head;
    Pose left;
    Pose right;
};

// Half-angle tangents of the headset's field of view, as Hmd2 reports them.
struct Fov {
    float out, in, top, bottom;
};

// The buffer to draw this frame into. With the library's own buffers the
// pixels are 64 KiB R_X tiled and B8G8R8A8 in memory (use pixel_offset /
// fill_rect / put_pixel, never index `pixels` directly); in external mode they
// are the caller's linear RGBA8 image and `width` x `height` are its real size.
struct EyeBuffer {
    int index;
    uint8_t *pixels;
    uint32_t width;    // 2 * eye width
    uint32_t height;   // eye height
};

struct DeviceState {
    uint32_t status;   // Hmd2 device status word (0 = ok)
    bool worn;         // the headset is on a head
    uint32_t lens_mm;  // lens-to-eye distance Hmd2 reports
};

// Whether the device status word means the headset is on and usable. Measured
// on hardware (console klogs, runs/issue-1-crash): status 0 is a headset that is
// on (worn 0 or 1, and sessions get tracked); status 2 is a headset that is
// plugged in but switched off (worn 0, never tracked). Only 0 counts as on:
// init() stops for anything else before reprojection and VR mode, so the
// viewer's TV-only session takes over instead of waiting for a pose that never
// comes (vr_mode.cpp, start_mode.h).
//
// Pure and header-only, so tests/tv_only_test.cpp pins the table without the
// SDK.
inline bool headset_on(uint32_t status) { return status == 0; }

// Where init() got to when it failed: the failing call and its return code.
struct Error {
    const char *step;  // "" when nothing failed
    int32_t rc;
    // The library whose export table did not match this firmware, when that is
    // why the step failed: "libSceHmd2" or "libSceVrTracker2". Null for every
    // other failure, including a table that matched but lacked a name. The
    // viewer shows its "not supported on this firmware" card when this is set.
    const char *missing_table_library;
};

// The init failures that are the expected way to start without a usable
// headset, so the viewer's klog line says so without "FAIL" (vr_mode.cpp):
//  - "PSVR2 is off": the headset is connected but switched off, the measured
//    device status 2 (init() stops right after GetDeviceInformation);
//  - "sceHmd2Open" with 0xffffffff: no headset session to open at all (the
//    return the console gives with no headset, docs/console-runbook.md).
// Every other failure - a module load, a missing export table, the tracker or
// reprojection - is a real init error and keeps "FAIL".
inline bool expected_fallback(const Error &error) {
    if (error.step == nullptr) return false;
    if (std::strcmp(error.step, "PSVR2 is off") == 0) return true;
    return std::strcmp(error.step, "sceHmd2Open") == 0 && error.rc == -1;
}

// Per-frame averages of the pacing since the last take_pace(), in
// milliseconds. nap: slept before the frame; call: BeginFrame itself; ahead:
// how far the predicted display time was at BeginFrame's return; step: the
// gap between consecutive predicted display times (16.7 ms = every 120 Hz
// refresh is being filled).
struct PaceStats {
    float nap_ms, call_ms, ahead_ms, step_ms;
};

// What VideoOut says about the last flip: `shown` is the argument (our frame
// number) of the latest flip the display took, `pending` is positive while a
// flip is queued, `rc` is the call's return code.
struct FlipInfo {
    uint64_t shown;
    int pending;
    int32_t rc;
};

bool init(const Config &config);   // false on failure; a failed init is cleaned up and can be retried
void shutdown();
bool active();

// Whether a PSVR2 is plugged in and switched on, for a session that already
// runs without VR (the viewer's TV-only session, 0.7: the user turns the headset
// on and the running session switches instead of needing a restart).
//
// The only device-status call this firmware exports is
// sceHmd2GetDeviceInformation (hmd2_funcs.h, and the 13.60 export list). It takes
// no handle, and HLA-PSVR2 calls it between Initialize and Open (psvr2.c:350),
// which would make a cheaper probe - but that one-shot diagnostic does not prove
// the no-handle answer follows the live 2 -> 0 change of a headset being switched
// on. So this probe uses the path the console klogs do prove (console-klog-067:
// status 2 on an open session with the headset off), and init() uses for its
// guard: Initialize, Open, GetDeviceInformation, Close, Terminate. Nothing of the
// VR bring-up runs - no tracker, no VrHand, no reprojection, no VR mode and no
// VideoOut port - so the display does not change mode and the TV keeps its
// picture. It writes nothing to the logger (the caller polls it every
// kHotplugProbeSeconds) and returns false on any call failure or a status word
// that headset_on() does not accept.
//
// Cheap with the modules a failed init already loaded: they stay resident, so
// the call is five exports and no module load. Only the console can confirm that
// (a) opening and terminating an Hmd2 session without VR mode is invisible on
// the TV, and (b) the open-session status word turns 2 -> 0 when the headset is
// switched on while the title is in TV-only mode.
bool headset_ready();

// The device status of the session init() opened (0 = the headset is on and
// usable; 2 = plugged in but switched off - the same word headset_on() takes).
// False when init() has not succeeded, the export is missing, or the call
// failed: `status` is then unknown and must not be read as "off". No log line,
// and nothing of this function writes the library's state.
//
// This is the VR session's counterpart of headset_ready(): while Hmd2 is open,
// the viewer's watch thread (vr_mode.cpp) calls it every 2 s off the render
// thread, and the headset going off is the cue to leave VR and continue on the
// TV. Only sceHmd2GetDeviceInformation is called - no BeginFrame/SetParam/flip
// runs from the watch - and that call is serialised with the frame thread's
// own status read (device_state) by one lock in hmd2.cpp, so the two threads
// never run the export at the same time. The watch is joined before every
// shutdown/init, so no Hmd2 session is opened or closed under it.
bool device_status(uint32_t &status);

// Holds the frame cadence: naps to the next 60 Hz slot measured from the
// previous BeginFrame (HLA-PSVR2's rule, so a slow frame is not held back and
// a fast one is not queued). Call once per frame, before begin_frame().
bool wait_slot();

// Hmd2 BeginFrame for the next frame, its predicted display time, and the head
// pose for that time. Retries while the reprojection queue is full, re-submits
// a frame two or more behind that was begun but never submitted (otherwise its
// queue slot never frees), and naps to display - period - latency - 1.5 ms when
// the previous frame needed little time.
bool begin_frame();

// The buffer to draw; call after begin_frame() and before submit(). In
// external mode its pixels are the caller's mapped image.
EyeBuffer acquire();

// Hmd2 SetParam + VideoOut SubmitFlip for the frame begin_frame() opened, then
// advance to the next buffer.
bool submit();

// The pose Hmd2 predicted for the frame begin_frame() opened (the same value
// begin_frame() queried). Convenience for "next frame"; use the overload below
// when you need a pose at a specific time. This is also the pose submit() hands
// Hmd2 as the one the image was rendered from, so the caller must render from
// exactly these values (and this display time): a render pose that differs from
// the submitted one is reprojected as if the head had moved that much.
Pose predicted_pose();

// The head pose of predicted_pose() plus the two eye poses from the same
// tracker result (sceVrTracker2GetResult +0xa0/+0xb0 and +0xe0/+0xf0, which
// HLA-PSVR2 reads as left and right eye position and orientation).
Poses predicted_poses();

// The pose Hmd2 predicts at an explicit display time: microseconds on the
// console clock (sceKernelGetProcessTime), normally display_time_us() or a
// little later for audio/spatialisation or extrapolation. Returns an untracked
// pose (origin, identity) when the tracker is not up.
Pose predicted_pose(uint64_t display_time_us);
uint64_t display_time_us();   // Hmd2's predicted display time for this frame
Fov field_of_view();
DeviceState device_state();   // cached, polled at most every 0.5 s
bool worn();

// -- Sense controllers -----------------------------------------------------------------
// Register one Sense controller's scePad handle with VrTracker2 so GetResult
// reports its pose. `right_hand` picks the device id HLA-PSVR2 registers the
// pair with: sceVrTracker2RegisterDevice(side ? 1 : 2, handle), i.e. 1 for the
// right controller and 2 for the left (psvr2.c:469). Returns false before
// init(), when the tracker is down, for a negative handle, or when the export
// is missing; registering the same handle again is a no-op that returns true.
// The registration is unregistered in shutdown().
//
// UNVERIFIED on hardware: this is the first read of a controller pose in this
// title. The morning headset check in docs/console-runbook.md section 1f is what
// proves it; the desktop test covers only the ray maths.
bool register_controller(int tracker_handle, bool right_hand);

// One controller's pose from VrTracker2, in tracker space: position in metres,
// quaternion (x, y, z, w). It is the same result block and decode as the head
// pose (+4 valid, +0x60 position, +0x70 orientation; HLA-PSVR2 psvr2.c:683-690,
// 827-870), queried for `tracker_handle` (the pad handle passed to
// register_controller) at this frame's predicted display time. False when the
// tracker or export is missing, GetResult fails, the result is not valid, or the
// pose is implausible (read_eye_pose's finite/range check). On false the
// outputs are left at the origin and identity, and the caller must not use them.
//
// UNVERIFIED on hardware: see register_controller().
bool controller_pose(int tracker_handle, float pos[3], float quat[4]);

int eye_size();     // per-eye width
int eye_height();   // per-eye height (PSVR2: 1984 wide by 2048 high)
int buffer_count();
// Bytes of one eye image; the tiled layout is larger than width*height*4 when
// the width is not a whole number of 128-pixel tiles.
size_t image_bytes();
// Hmd2's TV mirror (Config::mirror): true once the compositor took it. The
// display buffer's byte count is what Hmd2 asked for (its resolution is the
// system's, not the title's).
bool mirroring();
// Turns the mirror on after init(), when Config::mirror was false (the viewer
// asks for it only when its own TV output cannot be used, 0.6.0). The sequence
// is the one init() runs before VR mode; this one runs with VR mode already on,
// so it can be refused, and false then means the TV keeps the console UI.
bool start_mirror();
size_t mirror_display_bytes();
int refresh_hz();             // output mode asked for: 120
int frames_submitted();       // submit() calls that reached SetParam+SubmitFlip
int frame_errors();
int repeated_frames();        // abandoned frames re-shown with the last image
int queue_full_waits();       // BeginFrame retries because Hmd2's queue was full
int missed_frames();          // display times skipped, from the predicted-time step
int firmware_table();         // the table's fw tag (e.g. 1360, 1000) when one bound the modules, 0 if dlsym did
// The user the Hmd2 session was opened for: Config::user_id, or the console's
// first logged-in user (pick_user), after init(). 0xff when none was reported.
// A title that opens the DualSense with scePadOpen needs the same id.
int user_id();
Error error();

// Pacing and the display's own view of the flips, for the caller's 1 Hz log.
PaceStats take_pace();        // averages since the last call, then reset
FlipInfo flip_status();

// -- eye buffer drawing -----------------------------------------------------
// Byte offset of (x, y) in a tiled eye buffer of the given width. The tiling
// is the colour block's SW_64KB_R_X: 64 KiB tiles of 128x128 pixels, tile
// index row-major, the in-tile address from the 64KB_R_X swizzle.
size_t pixel_offset(uint32_t x, uint32_t y, uint32_t width);

// A pixel in B8G8R8A8 memory order.
uint32_t pixel(uint8_t r, uint8_t g, uint8_t b);

uint32_t get_pixel(const EyeBuffer &buffer, uint32_t x, uint32_t y);
void put_pixel(const EyeBuffer &buffer, uint32_t x, uint32_t y, uint32_t value);
void fill_rect(const EyeBuffer &buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               uint32_t value);

}  // namespace hmd2
