// The viewer title's VR mode: a SHARP scene, world-locked in the PSVR2 at the
// predicted head pose, 60 frames rendered and reprojected into the 120 Hz
// output by Hmd2 (the vr_hello display path).
//
// What one session does:
//
//   1. read the scene list (ScenePlaylist) and the .psplat the TV path would
//      show, then bring RADV up (the caller logged the pools);
//   2. create the three host-visible eye buffers Hmd2 samples (eye_buffers.h),
//      bring Hmd2 up in external-buffer mode, and read the headset's field of
//      view: per-eye tangents out/in/top/bottom, which are asymmetric (PSVR2:
//      1.842 / 0.947 horizontally);
//   3. create the C6 renderer in stereo at 1984x2048 per eye, with the headset
//      FOV as the projection and the per-eye principal-point offset that makes
//      each eye's frustum asymmetric (RendererOptions::eye_center_shift);
//   4. wait, up to five minutes, until Hmd2 reports the headset worn and the
//      tracker a valid pose that holds still for half a second (the user may be
//      setting up the play area first, and the tracker redefines its local
//      frame for a second or two after the session opens - a scene anchored
//      before that reset ends up ~90 deg to the side). The capture camera is
//      then placed at that pose through the recentre path, looking forward, at
//      world scale 1.0, and the scene stays anchored there;
//   5. render one frame per 60 Hz slot with hmd2's pacing, copy it into the eye
//      buffer Hmd2 gave us (GPU copy, no CPU pixels), submit, and repeat until
//      the panel's Quit item, the quit file, or the 30-minute cap;
//   6. tear down on every path: the pads (Sense left, right, then the
//      DualSense), VR mode off, buffers unregistered, display, Hmd2, tracker,
//      then the Vulkan objects.
//
// While it runs, the session writes a head-pose and input log to
// /data/ps5-splat-vr/logs/session-<UTC>.csv (one row per submitted frame: the
// raw Hmd2 pose the frame was rendered from, the scene-frame yaw/pitch/roll,
// every pad's buttons, the action bits, the scene's scale and offset, the
// recentre hold's progress, the scene and the event), so a recorded video can be
// matched to it; once a second it adds a compact pose line to klog. The table
// of columns is in docs/console-runbook.md, step 1d.
//
// Input is the DualSense and the two PSVR2 Sense controllers, opened through
// scePad for the same user the Hmd2 session uses (hmd2::user_id()): Options opens
// the panel (on the DualSense or the right Sense controller; the panel's Quit
// item ends the session), L1/R1 step through the scenes, L2/R2 grow/shrink the
// scene about the cone pivot (Front: 5th-percentile depth, the default; Centre:
// median with a 0.5 m near guard; the viewer never moves), the left stick walks
// and the right stick rises and turns (the sticks move the viewer, i.e. the
// world moves the other way), and Cross held for one second recentres (a short
// press does nothing, and R3 does nothing; the hold's progress is on the
// head-locked overlay). The stick speeds come from Settings (2.5 m/s and
// 40 deg/s by default) and ease in/out over 150 ms. Create does nothing, on
// either device. All three go through one action/analogue table (pad_input.h);
// any of them can trigger an action and each is optional.
//
// W16/VR8: the panel is the TV's panel (ui_setup.h: the same UiApp and UiHost the
// TV path builds). In VR it is a lazy follower (hmd_view.h PanelFollower): a
// quad 1.6 m ahead, parallel to the view plane (yaw and pitch, never roll), that
// re-centres over ~0.3 s once the head has turned ~17 deg away. The old
// world-locked, yaw-only plane was the keystoned panel of the 2026-10-27
// recording. While it is up the scene keeps rendering behind it and the scene's
// own controls (Cross hold, sticks, L2/R2, L1/R1) are suspended; the panel reads
// the same button word and left stick through pad::ui_actions_for (ui_mode.h).
// The menu's ViewScene loads the album's scene through the preload/switch path
// below (scene_switch.h) and makes that album the L1/R1 playlist; Quit ends the
// session through the normal teardown. Settings (Comfort/Display) writes
// ui.json through UiServices::state; this loop applies the values every frame.
//
// A scene switch keeps the world placement, scale and offset and uses the
// preloaded scene when it fits the spec's +64 MiB preload budget; otherwise it
// drops that preload and reads the file on demand and the log says so. Either
// way the last submitted frame keeps being reprojected by Hmd2 while the switch
// happens - the headset never shows black - and either way the press lands: the
// forward preload is never allowed to veto an L1 (scene_switch.h).
//
// The background is black: the renderer's clear colour and the comfort fade's
// backdrop are the same value (RendererOptions::backdrop), set to 0 here, so
// the void around the photo and the fade past the comfort radius are black in
// both eyes and on the TV mirror, which shows the same eye buffers.
//
// Bounded waits, leak-on-timeout: the display wait, the pose wait and every
// copy fence have a timeout. If a GPU wait fails, the Hmd2/VideoOut side is
// still shut down (the headset must not be left in VR mode) but the Vulkan
// objects are deliberately leaked - nothing may be freed under a GPU that may
// still read it - and the log says so.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr_mode.h"

#include <sys/stat.h>
#include <time.h>

#include <cerrno>
#include <ctime>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gallery/gallery.h"
#include "render/grab.h"
#include "render/hmd_view.h"
#include "render/overlay_hud.h"
#include "render/scene_label.h"
#include "render/scene_playlist.h"
#include "render/splat_renderer.h"
#include "render/text_raster.h"
#include "render/viewer_state.h"
#include "render/vulkan_context.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "ui/pointer.h"

#include "../hmd2/hmd2.h"

#include "convert_gate.h"
#include "eye_buffers.h"
#include "overlay_refresh.h"
#include "pad_input.h"
#include "scene_switch.h"
#include "title_common.h"
#include "ui_mode.h"
#include "ui_setup.h"

// The pads, through the SDK's stubs. Button data is read as opaque bytes: every
// ScePadData firmware puts the button bitmask first, and this title only needs
// that word (PS5VR reads the same structure through the same call). The Sense
// controllers are opened as scePad types 3 (left) and 4 (right) and read with
// scePadVrControllerReadState, whose 0x68-byte state has the same button word
// at +0x00 and a connected bit at +0x40 (HLA-PSVR2 psvr2.c:458-473, 517-581).
extern "C" {
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, void* param);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, void* data);
int scePadVrControllerReadState(int handle, void* state);
int scePadClose(int handle);
}

namespace viewer {
namespace {

using viewer::line;
using viewer::now_ms;

// ---- constants -------------------------------------------------------------
// HLA-PSVR2's eye size (the panel is 2000x2040, rounded down to whole 64-pixel
// texels): 1984x2048 per eye, 3968x2048 side by side, the size vr_hello proved
// on hardware.
constexpr uint32_t kEyeWidth = 1984;
constexpr uint32_t kEyeHeight = 2048;
constexpr int kEyeBufferCount = 3;
// 500 ms, vr_hello's per-frame fence timeout: past that the GPU is wedged and
// nothing may be freed under it.
constexpr uint64_t kCopyTimeoutNs = 500ull * 1000 * 1000;
// Spec: preloading the next scene may cost +64 MiB.
constexpr size_t kPreloadBudgetBytes = 64u * 1024u * 1024u;
// Start-up wait for the headset.
constexpr double kPoseWaitLimitSeconds = 300.0;
constexpr double kPoseWaitLogSeconds = 5.0;
// hmd2's own frame period (60 Hz) with a little slack, for the "slow frame"
// counter: a frame that takes longer than this cannot be reprojected on time.
constexpr double kFramePeriodMs = 1000.0 / 60.0;
// The identity's data folder (ui_setup.h): the same quit file and pose-log
// folder the TV path uses, spelled from the one root.
const std::string kQuitPath = data_path("title-quit");

// How often a missing pad is retried, and no more: HLA-PSVR2's pads_open()
// retries every 2 s (psvr2.c:450-482), so a controller switched on after the
// title started is picked up without a line per attempt.
constexpr double kPadRetryMs = 2000.0;
// The Sense button word is logged at most this often (5 lines a second) when it
// changes: enough to see every press, never a line per frame.
constexpr double kPadRawLogMs = 200.0;
// scePad types: 0 is the DualSense, 3 the left Sense controller and 4 the
// right (HLA-PSVR2 psvr2.c:461).
constexpr int kPadTypeDualsense = 0;
constexpr int kPadTypeSenseLeft = 3;
constexpr int kPadTypeSenseRight = 4;
// scePadVrControllerReadState's state: the button word at +0x00, the connected
// bit at +0x40 (HLA-PSVR2 psvr2.c:517-525).
constexpr size_t kSenseStateBytes = 0x68;
constexpr size_t kSenseConnectedOffset = 0x40;
// The analogue bytes of both layouts: ScePadData has lx, ly, rx, ry, L2, R2 at
// +0x04..+0x09; a Sense state has its stick at +0x08/+0x09 (HLA-PSVR2
// psvr2.c:370-379, 530-541).
constexpr size_t kPadLeftStickX = 0x04;
constexpr size_t kPadLeftStickY = 0x05;
constexpr size_t kPadRightStickX = 0x06;
constexpr size_t kPadRightStickY = 0x07;
constexpr size_t kPadLeftTrigger = 0x08;
constexpr size_t kPadRightTrigger = 0x09;

// The Sense right controller's R2 is the panel's click when the ray is over a
// widget (W25): analogue, so a press counts past this fraction of full travel.
constexpr float kPointerClickTrigger = 0.6f;
// The pointer hit line is at most this often; the ray moves every frame.
constexpr double kPointerLogMs = 1000.0;

// The "Scale 1.6x" readout stays up this long after the last change.
constexpr double kScaleHintMs = 1500.0;
// One loop's input dt is capped at about one 60 Hz frame (plus a little): a
// scene read or a stalled frame must not turn into a jump of metres, and a
// hitch must not show up as a single double-length turn step in the CSV (the
// 2026-10-27 run's 1.7 deg steps among 0.98 deg ones). A gap longer than
// kInputHitchSeconds is a freeze (a scene load, a GPU stall): that frame gets
// dt 0 and the last image is held, instead of converting the whole hitch into
// one frame of motion.
constexpr double kMaxInputSeconds = 0.020;
constexpr double kInputHitchSeconds = 0.050;
// A scale press does not start while the head is turning faster than this
// (docs/research/vr-scale.md: the dolly term plus a sweep is uncomfortable).
constexpr double kScaleMaxHeadDegreesPerSecond = 60.0;
// The pivot marker and the vignette outlive the last scale change by this
// long; the "Scale N.Nx - mode" text uses kScaleHintMs (1.5 s).
constexpr double kScaleMarkerMs = 500.0;
constexpr float kScaleVignetteStrength = 0.35f;

// Settings -> Comfort -> Scale (UiState::scale_around): 0 front, 1 centre,
// 2 maquette (the default). One mapping for the log, the readout and the press.
splat::ScalePivotMode pivot_mode_from_setting(int scale_around) {
    if (scale_around == 0) return splat::ScalePivotMode::front;
    if (scale_around == 1) return splat::ScalePivotMode::centre;
    return splat::ScalePivotMode::maquette;
}
const char* pivot_mode_name(splat::ScalePivotMode mode) {
    switch (mode) {
        case splat::ScalePivotMode::front: return "front";
        case splat::ScalePivotMode::centre: return "centre";
        case splat::ScalePivotMode::maquette: return "maquette";
    }
    return "maquette";
}

// The HUD's scale line, padded to kScaleHudChars so the mask keeps one size.
// Maquette's wording is the headset-test-6 readout ("Maquette 0.08x"); the
// front/centre modes keep the VR8 "Scale 1.6x - front" line.
std::string scale_readout_text(splat::ScalePivotMode mode, float scale) {
    char text[splat::kScaleHudChars + 1];
    if (mode == splat::ScalePivotMode::maquette) {
        std::snprintf(text, sizeof text, "Maquette %.2fx", static_cast<double>(scale));
    } else {
        std::snprintf(text, sizeof text, "Scale %.1fx - %s", static_cast<double>(scale),
                      pivot_mode_name(mode));
    }
    std::string out(text);
    out.resize(splat::kScaleHudChars, ' ');
    return out;
}

// ---- tracker poses ---------------------------------------------------------
// The pose mapping itself lives in core/render/hmd_view.h, shared with the
// desktop test: Hmd2's tracker axes (+y up, -z forward, metres, quaternion
// x,y,z,w) to the scene's OpenCV axes, plus the placement/recentre state.
splat::HeadPoses to_head_poses(const hmd2::Poses& poses) {
    splat::HeadPoses out;
    const hmd2::Pose* source[3] = {&poses.head, &poses.left, &poses.right};
    splat::TrackedPose* target[3] = {&out.head, &out.left, &out.right};
    for (int i = 0; i < 3; i++) {
        for (int axis = 0; axis < 3; axis++) target[i]->position[axis] = source[i]->pos[axis];
        for (int axis = 0; axis < 4; axis++) target[i]->quaternion[axis] = source[i]->quat[axis];
        target[i]->tracked = source[i]->tracked;
    }
    return out;
}

// ---- pads ------------------------------------------------------------------
// scePadInit once for all three pads; each Pad only opens its own handle.
void pad_init_once() {
    static bool done = false;
    if (done) return;
    scePadInit();
    done = true;
}

// One pad: the DualSense or one Sense controller. Opening, retrying, reading and
// edge detection are the same for all three; which button means what is the one
// table in pad_input.h. Every pad is optional and independent: any of them can
// trigger an action in the same frame.
class Pad {
public:
    Pad(pad::Device device, const char* name, int type)
        : device_(device), name_(name), type_(type) {}

    // Opens the pad when it is not open, at most once per kPadRetryMs. Buttons
    // already down at open (a launch press, usually Cross or Options from the
    // home screen) stay ignored until let go. A pad that is not there is
    // reported once, then only when it appears.
    void open(int user, double now) {
        if (handle_ >= 0 || now < next_open_ms_) return;
        next_open_ms_ = now + kPadRetryMs;
        pad_init_once();
        int handle = scePadOpen(user, type_, 0, nullptr);
        if (handle < 0) handle = scePadGetHandle(user, type_, 0);
        if (handle < 0) {
            if (!reported_missing_) {
                reported_missing_ = true;
                line("viewer: vr pad: no %s controller for user %d (scePadOpen -> 0x%08x); "
                     "retrying every %.0f s, the session runs without it",
                     name_, user, static_cast<uint32_t>(handle), kPadRetryMs / 1000.0);
            }
            return;
        }
        handle_ = handle;
        reported_missing_ = false;
        connected_ = false;
        // The first read seeds the state but prints no transition line: "open"
        // is this moment's line, so a pad already connected at startup logs
        // once (open), not twice.
        poll(false);
        // Buttons already down (poll()'s held set, or the ones it ignored when a
        // Sense controller reported itself connected) are not a press.
        ignored_ |= held_;
        held_ = 0;
        if (device_ == pad::Device::dualsense) {
            line("viewer: vr pad: %s controller open for user %d (handle %d)", name_, user,
                 handle_);
        } else {
            // The raw word at open is the first answer to "does a Sense pad that
            // was never registered with VrTracker2 deliver buttons at all?"; it
            // is this line's word, so the change log starts from it.
            raw_logged_ = raw_;
            line("viewer: vr pad: %s controller open for user %d (handle %d, raw buttons 0x%08x)",
                 name_, user, handle_, raw_);
        }
    }

    void close() {
        if (handle_ >= 0) scePadClose(handle_);
        handle_ = -1;
        held_ = 0;
        ignored_ = 0;
    }

    // The pad's raw button word as last read (the ScePadData word, or the Sense
    // state's word at +0x00): what the head-pose log records per device.
    uint32_t buttons() const { return raw_; }

    // The scePad handle while open, -1 when absent. The Sense right controller's
    // handle is also VrTracker2's GetResult handle once registered (W25).
    int handle() const { return handle_; }

    // The buttons still down after the open-time presses were discarded: what
    // the recentre hold reads. Zero while the pad is closed or gone.
    uint32_t held() const { return held_; }

    // This pad's sticks and analogue triggers, decoded (0 at rest, y up
    // positive). Zero while the pad is closed or gone.
    const pad::Axes& axes() const { return axes_; }

    // Edge-triggered: this pad's actions for the buttons that went down since
    // the previous call.
    uint32_t take_pressed() {
        accumulated_ |= poll(true);
        const uint32_t out = accumulated_;
        accumulated_ = 0;
        return out;
    }

private:
    // `report_changes` is false for the read that follows open(): the open line
    // already says where the pad is, so a pad connected at startup must not also
    // print "connected" before it.
    uint32_t poll(bool report_changes) {
        if (handle_ < 0) return 0;
        uint32_t buttons = 0;
        pad::Axes axes;
        if (device_ == pad::Device::dualsense) {
            // Larger than any firmware's ScePadData (PS5VR reads 148 bytes of
            // it); only the leading button word and the six analogue bytes are
            // read.
            alignas(16) uint8_t data[256] = {};
            if (scePadReadState(handle_, data) == 0) {
                std::memcpy(&buttons, data, sizeof buttons);
                axes.left_x = pad::stick_x(data[kPadLeftStickX]);
                axes.left_y = pad::stick_y(data[kPadLeftStickY]);
                axes.right_x = pad::stick_x(data[kPadRightStickX]);
                axes.right_y = pad::stick_y(data[kPadRightStickY]);
                axes.l2 = pad::trigger_from_byte(data[kPadLeftTrigger]);
                axes.r2 = pad::trigger_from_byte(data[kPadRightTrigger]);
            }
            raw_ = buttons;
            // The system menu owns the pad: its sticks must not move the
            // scene while the user is in the PS menu (HLA-PSVR2 zeroes the
            // whole sample, psvr2.c:545-550).
            if ((buttons & pad::kIntercepted) != 0) {
                buttons = 0;
                axes = pad::Axes{};
            }
        } else {
            alignas(16) uint8_t state[kSenseStateBytes] = {};
            const bool read = scePadVrControllerReadState(handle_, state) == 0;
            const bool connected = read && (state[kSenseConnectedOffset] & 1) != 0;
            if (read) std::memcpy(&raw_, state, sizeof raw_);
            if (connected) {
                buttons = raw_;
                // A Sense controller has one stick and two analogue trigger
                // bytes. Its stick goes to its own hand's role: the left
                // controller's walks the viewer, the right one's rises and
                // turns them (view_control() reads right_x/right_y for that), so
                // the hand, not just the bytes, decides the fields. The viewer
                // then picks the role's trigger (left hand = L2 = shrink, right
                // hand = R2 = grow).
                axes = pad::sense_controller_axes(device_ == pad::Device::sense_right,
                                                  state[kPadLeftStickX], state[kPadLeftStickY],
                                                  state[kPadLeftTrigger], state[kPadRightTrigger]);
                if ((buttons & pad::kIntercepted) != 0) {
                    buttons = 0;
                    axes = pad::Axes{};
                }
            }
            if (connected != connected_) {
                // One line per transition: a controller left in a drawer must
                // not fill the log every frame.
                connected_ = connected;
                if (connected) {
                    ignored_ |= buttons;   // held while it connected: not a press
                } else {
                    held_ = 0;
                    axes = pad::Axes{};
                }
                if (report_changes) {
                    line("viewer: vr pad: %s controller %s", name_,
                         connected ? "connected" : "gone");
                }
            }
            if (!connected) {
                axes_ = pad::Axes{};
                held_ = 0;
                return 0;
            }
            if (report_changes) log_raw(raw_);
        }
        ignored_ &= buttons;   // a button is ignored only while it stays down
        buttons &= ~ignored_;
        const uint32_t pressed = pad::actions_for(buttons & ~held_);
        held_ = buttons;
        axes_ = axes;
        return pressed;
    }

    // The Sense button word, when it changes, at most kPadRawLogMs apart (a
    // change inside the rate limit is logged by the next poll after it). This is
    // the one line that tells, on the first headset run, whether a Sense pad
    // delivers buttons without sceVrTracker2RegisterDevice: the word XORed with
    // the table says whether a press reaches libScePad at all.
    void log_raw(uint32_t buttons) {
        if (buttons == raw_logged_) return;
        const double now = now_ms();
        if (now < next_raw_log_ms_) return;
        raw_logged_ = buttons;
        next_raw_log_ms_ = now + kPadRawLogMs;
        line("viewer: vr pad: %s raw buttons 0x%08x", name_, buttons);
    }

    pad::Device device_;
    const char* name_;
    int type_;
    int handle_ = -1;
    bool reported_missing_ = false;
    bool connected_ = false;
    double next_open_ms_ = 0.0;
    uint32_t raw_ = 0;            // the Sense state's word at +0x00, last read
    uint32_t raw_logged_ = 0;     // the word the log last carried
    double next_raw_log_ms_ = 0.0;
    uint32_t held_ = 0;
    uint32_t ignored_ = 0;
    uint32_t accumulated_ = 0;
    pad::Axes axes_;
};

// ---- head-pose and input log ----------------------------------------------
// One CSV per run, for matching a recorded video of a session to what the user
// did: <dataDir>/logs/session-<UTC>.csv, flushed once a second. One row
// per submitted frame plus one for an event that ends the run, so the columns
// line up with the frames Hmd2 showed (and with the `viewer: vr t=...` lines).
//
// The pose columns are Hmd2's own values, unconverted: metres, +y up, forward
// -z, quaternion x,y,z,w. The angle columns are what the renderer used, in the
// scene's frame (yaw/pitch/roll after the placement offsets), in degrees.
// Bounded: the file stops at kPoseLogMaxBytes (a 30-minute session at 60 frames
// a second is about 20 MiB) and says so in its last row and in klog.
const std::string kPoseLogDir = data_path("logs");
constexpr size_t kPoseLogMaxBytes = 32u * 1024u * 1024u;
constexpr double kRadToDeg = 57.29577951308232;

// A CSV text field: quoted, with embedded quotes doubled. Scene folder names
// come from the user, so a comma in one must not shift the columns.
void csv_text(FILE* file, const char* text) {
    std::fputc('"', file);
    for (const char* c = text; c != nullptr && *c != '\0'; ++c) {
        if (*c == '"') std::fputc('"', file);
        std::fputc(*c, file);
    }
    std::fputc('"', file);
}

class PoseLog {
public:
    // Creates the folder and this run's file. False when /data is not writable
    // (the log is optional; the session must still run) - the caller says so in
    // one klog line.
    bool open() {
        if (file_ != nullptr) return true;
        if (::mkdir(kPoseLogDir.c_str(), 0777) != 0 && errno != EEXIST) {
            error_ = errno;
            return false;
        }
        const std::time_t now = std::time(nullptr);
        std::tm utc {};
        gmtime_r(&now, &utc);
        char name[96];
        std::snprintf(name, sizeof name, "%s/session-%04d%02d%02d-%02d%02d%02d.csv",
                      kPoseLogDir.c_str(),
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                      utc.tm_sec);
        file_ = std::fopen(name, "w");
        if (file_ == nullptr) {
            error_ = errno;
            return false;
        }
        std::snprintf(path_, sizeof path_, "%s", name);
        // The header is the runbook's column list (docs/console-runbook.md).
        std::fputs("t_s,wall_utc,frame,tracked,pos_x_m,pos_y_m,pos_z_m,quat_x,quat_y,quat_z,"
                   "quat_w,yaw_deg,pitch_deg,roll_deg,dualsense_buttons,sense_left_buttons,"
                   "sense_right_buttons,actions,scale,offset_x_m,offset_y_m,offset_z_m,turn_deg,"
                   "hold_progress,scene,event\n",
                   file_);
        return true;
    }

    const char* path() const { return path_; }
    int error() const { return error_; }

    // One frame. `pose` is the raw Hmd2 tracker pose the frame was rendered
    // from; the angles are the scene-frame yaw/pitch/roll in degrees; the words
    // are each device's button bitmask and this frame's action bits. `scale`
    // and `offset` are the scene adjustment the frame was rendered with, and
    // `hold_progress` is the recentre hold's 0..1 (0 when not held).
    void row(double t_seconds, uint64_t frame, const splat::TrackedPose& pose, double yaw_deg,
             double pitch_deg, double roll_deg, uint32_t dualsense, uint32_t sense_left,
             uint32_t sense_right, uint32_t actions, float scale, const float offset[3],
             double turn_deg, float hold_progress, const char* scene, const char* event) {
        if (file_ == nullptr || capped_) return;
        // UTC with milliseconds, the format a video editor's timecode can be
        // checked against.
        timespec wall {};
        clock_gettime(CLOCK_REALTIME, &wall);
        std::tm utc {};
        const std::time_t seconds = wall.tv_sec;
        gmtime_r(&seconds, &utc);
        const int written = std::fprintf(
            file_,
            "%.3f,%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ,%llu,%d,%.4f,%.4f,%.4f,"
            "%.6f,%.6f,%.6f,%.6f,%.2f,%.2f,%.2f,0x%08x,0x%08x,0x%08x,0x%x,%.3f,"
            "%.4f,%.4f,%.4f,%.3f,%.3f,",
            t_seconds, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
            utc.tm_sec, wall.tv_nsec / 1000000L, static_cast<unsigned long long>(frame),
            pose.tracked ? 1 : 0, static_cast<double>(pose.position[0]),
            static_cast<double>(pose.position[1]), static_cast<double>(pose.position[2]),
            static_cast<double>(pose.quaternion[0]), static_cast<double>(pose.quaternion[1]),
            static_cast<double>(pose.quaternion[2]), static_cast<double>(pose.quaternion[3]),
            yaw_deg, pitch_deg, roll_deg, dualsense, sense_left, sense_right, actions,
            static_cast<double>(scale), static_cast<double>(offset[0]),
            static_cast<double>(offset[1]), static_cast<double>(offset[2]), turn_deg,
            static_cast<double>(hold_progress));
        if (written > 0) bytes_ += static_cast<size_t>(written);
        csv_text(file_, scene);
        std::fputc(',', file_);
        csv_text(file_, event);
        std::fputc('\n', file_);
        if (bytes_ >= kPoseLogMaxBytes) {
            capped_ = true;
            // One row with every field empty but the time and the reason: 23
            // empty numeric fields between t_s and the quoted scene/event.
            std::fprintf(file_,
                         "%.3f,,,,,,,,,,,,,,,,,,,,,,,\"\",\"log capped at %zu bytes\"\n",
                         t_seconds, kPoseLogMaxBytes);
            flush();
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    // Once a second (and on close): a run that is killed keeps its rows.
    void flush() {
        if (file_ != nullptr) std::fflush(file_);
    }

    void close() {
        if (file_ == nullptr) return;
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }

private:
    FILE* file_ = nullptr;
    size_t bytes_ = 0;
    int error_ = 0;
    bool capped_ = false;
    char path_[128] = {};
};

// ---- one session -----------------------------------------------------------
struct RunState {
    bool gpu_ok = true;          // false after a GPU wait timed out: teardown leaks
    bool tracking_lost = false;  // logged once when the tracker drops out
};

// A scene's two depth measures, from the CPU copy the load produced (read once
// per scene: 1.2 M splats are not scanned per frame):
//  - `median`, the scene's centre on the capture camera axis: the log's "scene
//    centre" and the depth of the point the user looks at;
//  - `near`, its nearest content: the scale pivot is never further away than
//    that, so a miniature is scaled about its own photo and not about a point
//    behind it.
struct SceneDepths {
    float median = 1.6f;
    float near = 1.6f;
    // A decimated copy of the splat positions (scene space xyz, ~50k points):
    // the per-press cone measure in HmdView. A 1.2 M-splat scene costs ~600 KiB
    // instead of the 34 MiB CPU copy, and only presses scan it.
    std::vector<float> samples;
};

SceneDepths scene_depths(const splat::SplatScene& scene) {
    const float* positions = scene.position.data();
    const size_t count = scene.count();
    SceneDepths depths;
    depths.median = splat::median_scene_depth(positions, count);
    depths.near = splat::near_scene_depth(positions, count);
    const size_t step = std::max<size_t>(1, count / 50000);
    depths.samples.reserve((count / step + 1) * 3);
    for (size_t i = 0; i < count; i += step) {
        const float x = positions[i * 3 + 0];
        const float y = positions[i * 3 + 1];
        const float z = positions[i * 3 + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
        // Only splats that carry colour: a nearly transparent floater must not
        // pull a percentile. This is the research's opacity > 0.3 test.
        const float logit = scene.opacity_logit[i];
        const float alpha = 1.0f / (1.0f + std::exp(-logit));
        if (!(alpha > 0.3f)) continue;
        depths.samples.push_back(x);
        depths.samples.push_back(y);
        depths.samples.push_back(z);
    }
    return depths;
}

// The renderer's one-deep staging slot plus the preload worker, as
// scene_switch.h's SwitchSlot (read that file first: it is why a scene press
// must be able to drop the forward preload). Owns what VR5 kept loose in the
// frame loop - which playlist index the slot holds, the worker's index, the
// preload budget's refusals and the staged scene's two depth measures - so a
// failed press cannot leave the bookkeeping disagreeing with the renderer.
class VrSwitchSlot final : public SwitchSlot {
public:
    VrSwitchSlot(splat::SplatRenderer& renderer, splat::ScenePlaylist& scenes)
        : renderer_(renderer), scenes_(scenes) {}
    ~VrSwitchSlot() override { preloader_.cancel(); }

    // Called with every scene the slot reads (on demand or preloaded), for the
    // capture-intrinsics note.
    std::function<void(const splat::LoadedScene&)> on_load;

    long staged() const override { return staged_; }
    bool holds_buffers() const override { return renderer_.has_staged_scene(); }

    void release() override {
        // Unconditional and idempotent: SplatRenderer::discard_staged_scene()
        // does nothing when the slot is free. Dropping the index too is what
        // keeps the next attempt honest.
        renderer_.discard_staged_scene();
        staged_ = -1;
    }

    // Frees the slot and forgets every index it knew. The panel's ViewScene
    // (W16) replaces the playlist this slot points at with another album's, so
    // a staged index from the old one would name a different scene; the
    // preload worker's result belongs to it as well.
    void reset() {
        release();
        preloader_.cancel();
        preload_index_ = -1;
        refused_preload_ = -1;
    }

    bool upload(std::size_t index) override {
        const splat::Path path = scenes_.entry(index).scene_file;
        // The worker's finished read is used when it is the scene asked for;
        // a read still running (or for another scene) is dropped instead of
        // waited for: the press lands now, and one more 80 ms read is cheaper
        // than a press that does nothing.
        const bool take_preload = preloader_.ready() && preloader_.path() == path;
        if (!take_preload) preloader_.cancel();
        preload_index_ = -1;
        const double started = now_ms();
        splat::Result<splat::LoadedScene> next =
            take_preload ? preloader_.take() : splat::read_scene_path(path);
        read_ms_ = take_preload ? preloader_.last_load_ms() : now_ms() - started;
        if (!next.ok()) {
            line("viewer: vr FAIL load %s: %s (staying on %s)", path.string().c_str(),
                 next.message().c_str(), current_folder());
            return false;
        }
        if (on_load) on_load(next.value());
        const SceneDepths depths = scene_depths(next.value().scene);
        const double stage_started = now_ms();
        const splat::Status staged = renderer_.stage_scene(next.value().scene);
        stage_ms_ = now_ms() - stage_started;
        if (!staged.ok()) {
            line("viewer: vr FAIL stage %s: %s (staying on %s)", path.string().c_str(),
                 staged.message().c_str(), current_folder());
            return false;
        }
        median_depth_ = depths.median;
        near_depth_ = depths.near;
        pivot_samples_ = std::move(depths.samples);
        staged_ = static_cast<long>(index);
        return true;
    }

    bool activate(std::size_t index) override {
        if (staged_ != static_cast<long>(index)) return false;
        staged_ = -1;
        if (!renderer_.has_staged_scene()) return false;  // bookkeeping was stale
        const splat::Status status = renderer_.commit_staged_scene();
        if (!status.ok()) {
            line("viewer: vr preloaded scene failed to commit: %s", status.message().c_str());
            renderer_.discard_staged_scene();
            return false;
        }
        // A new current scene: a preload refused for budget earlier may fit now.
        refused_preload_ = -1;
        return true;
    }

    // Keeps the *next* scene in the slot while the user looks at this one.
    // Called once per frame; does nothing while the slot is occupied or the
    // worker is reading.
    void prefetch_next() {
        if (scenes_.size() < 2) return;
        if (staged_ >= 0 || preloader_.running()) return;
        if (preloader_.ready()) {
            const long want = preload_index_;
            auto next = preloader_.take();
            preload_index_ = -1;
            if (!next.ok()) {
                line("viewer: vr preload failed: %s", next.message().c_str());
                return;
            }
            const double stage_started = now_ms();
            const SceneDepths depths = scene_depths(next.value().scene);
            const splat::Status staged = renderer_.stage_scene(next.value().scene);
            if (!staged.ok()) {
                line("viewer: vr preload stage failed: %s", staged.message().c_str());
                return;
            }
            if (on_load) on_load(next.value());
            median_depth_ = depths.median;
            near_depth_ = depths.near;
            pivot_samples_ = std::move(depths.samples);
            staged_ = want;
            line("viewer: vr preloaded %s: read %.1f ms, stage %.1f ms, %.1f MiB staged (+64 MiB "
                 "budget)",
                 scenes_.entry(static_cast<size_t>(want)).folder.filename().string().c_str(),
                 preloader_.last_load_ms(), now_ms() - stage_started,
                 static_cast<double>(renderer_.staged_bytes()) / (1024.0 * 1024.0));
            return;
        }
        // Idle: prefetch the next scene, unless it does not fit the budget.
        const size_t next_index = (scenes_.index() + 1) % scenes_.size();
        if (static_cast<long>(next_index) == refused_preload_) return;
        const size_t bytes = preload_score(next_index);
        if (bytes > kPreloadBudgetBytes) {
            refused_preload_ = static_cast<long>(next_index);
            line("viewer: vr preload skipped for %s: about %.1f MiB over the +64 MiB budget; it "
                 "loads on demand (about 250 ms, last frame held)",
                 scenes_.entry(next_index).folder.filename().string().c_str(),
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
            return;
        }
        preload_index_ = static_cast<long>(next_index);
        preloader_.start(scenes_.entry(next_index).scene_file);
    }

    // The scene the renderer now draws: its centre depth, its nearest content
    // and the timings of the upload that put it there, for the switch's log
    // line.
    float applied_median_depth() const { return median_depth_; }
    float applied_near_depth() const { return near_depth_; }
    const std::vector<float>& applied_pivot_samples() const { return pivot_samples_; }
    double read_ms() const { return read_ms_; }
    double stage_ms() const { return stage_ms_; }
    double commit_ms() const { return renderer_.last_commit_ms(); }

private:
    const char* current_folder() {
        folder_name_ = scenes_.current().folder.filename().string();
        return folder_name_.c_str();
    }

    // The five fp16 attribute buffers the staged scene needs, plus the
    // instance/key/value arrays when it is larger than the live scene (the
    // renderer allocates those at stage time): 14 halfs per splat, 3 vec4 of
    // instances, 4 bytes each of key and value.
    size_t preload_score(size_t index) const {
        struct stat status {};
        const splat::Path path = scenes_.entry(index).scene_file;
        if (::stat(path.string().c_str(), &status) != 0 ||
            status.st_size <= static_cast<off_t>(splat::kPsplatHeaderSize)) {
            return kPreloadBudgetBytes + 1;   // unknown size: do not preload
        }
        const size_t count = (static_cast<size_t>(status.st_size) - splat::kPsplatHeaderSize) /
                             splat::kPsplatBytesPerSplat;
        size_t bytes = count * splat::kPsplatBytesPerSplat;
        if (count > renderer_.scene_count()) {
            bytes += count * (3 * 16 + 4 + 4);
        }
        return bytes;
    }

    splat::SplatRenderer& renderer_;
    splat::ScenePlaylist& scenes_;
    splat::ScenePreloader preloader_;
    long staged_ = -1;              // playlist index in the slot, -1 = empty
    long preload_index_ = -1;       // playlist index the worker is reading
    long refused_preload_ = -1;     // index known to be over the preload budget
    float median_depth_ = 1.6f;     // the slot's scene's centre (log, depth)
    float near_depth_ = 1.6f;       // the slot's scene's nearest content (pivot clamp)
    std::vector<float> pivot_samples_;  // decimated splat positions for a press's cone
    double read_ms_ = 0.0;
    double stage_ms_ = 0.0;
    std::string folder_name_;
};

}  // namespace

int run_vr(const VrOptions& options) {
    const double session_started = now_ms();
    // A quit file from an earlier stop must not end this run: only one written
    // after the session started counts (the TV path's contract).
    const std::time_t started_wall = std::time(nullptr);
    line("viewer: vr session (mode vr): eye %ux%u per eye, 60 rendered -> 120 Hz reprojected, "
         "cap %d s",
         kEyeWidth, kEyeHeight, options.cap_seconds);

    // The head-pose and input log of this run. Optional: a /data that refuses
    // the write must not cost the session (the first title to write there
    // answers whether it can - docs/console-runbook.md, step 1a).
    PoseLog pose_log;
    if (pose_log.open()) {
        line("viewer: vr log: %s (one row per submitted frame, flushed every second, at most "
             "%zu MiB)",
             pose_log.path(), kPoseLogMaxBytes / (1024u * 1024u));
    } else {
        line("viewer: vr log: cannot write %s (errno %d); the session runs without the pose log",
             kPoseLogDir.c_str(), pose_log.error());
    }

    // ---- scene list -------------------------------------------------------
    auto playlist_result = splat::ScenePlaylist::from_folder(splat::Path(options.scenes_dir));
    if (!playlist_result.ok()) {
        line("viewer: FAIL scenes: %s", playlist_result.message().c_str());
        return 2;
    }
    splat::ScenePlaylist& scenes = playlist_result.value();
    line("viewer: vr scenes: %zu folder(s) under %s", scenes.size(),
         options.scenes_dir.c_str());
    size_t scene_index = 0;
    if (!options.scene.empty()) {
        bool found = false;
        for (size_t i = 0; i < scenes.size(); ++i) {
            if (scenes.entry(i).folder.filename().string() == options.scene) {
                scene_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            line("viewer: vr SCENE=%s is not one of the %zu folders under %s; using the first",
                 options.scene.c_str(), scenes.size(), options.scenes_dir.c_str());
        }
    }
    scenes.set_index(scene_index);

    // ---- device -----------------------------------------------------------
    const double device_started = now_ms();
    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        line("viewer: FAIL device: %s", context_result.message().c_str());
        return 4;
    }
    std::unique_ptr<splat::VulkanContext> context = context_result.take();
    const VkPhysicalDeviceProperties& properties = context->properties();
    line("viewer: vr device %s, Vulkan %u.%u.%u, up in %.1f ms", properties.deviceName,
         VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
         VK_API_VERSION_PATCH(properties.apiVersion), now_ms() - device_started);
    if (!context->timestamps_supported()) {
        line("viewer: vr GPU timestamps unavailable on this device; project/sort/draw print 0.00");
    }

    // ---- first scene, read before anything is on the headset --------------
    const splat::Path first_path = scenes.current().scene_file;
    const double scene_started = now_ms();
    auto first_result = splat::read_scene_path(first_path);
    if (!first_result.ok()) {
        line("viewer: FAIL load %s: %s", first_path.string().c_str(),
             first_result.message().c_str());
        return 3;
    }
    splat::LoadedScene loaded = first_result.take();
    line("viewer: vr scene %zu/%zu %s: %zu splats, load %.1f ms", scene_index + 1, scenes.size(),
         scenes.current().folder.filename().string().c_str(), loaded.scene.count(),
         now_ms() - scene_started);
    if (loaded.meta.intrinsics.fx > 0.0f) {
        line("viewer: vr capture intrinsics fx %.2f fy %.2f cx %.1f cy %.1f, image %ux%u",
             loaded.meta.intrinsics.fx, loaded.meta.intrinsics.fy, loaded.meta.intrinsics.cx,
             loaded.meta.intrinsics.cy, loaded.meta.intrinsics.width, loaded.meta.intrinsics.height);
    }

    // ---- eye buffers and Hmd2 ---------------------------------------------
    // Everything the teardown has to free is declared here, so one teardown can
    // also run from a failure halfway through setup. Whatever does not exist is
    // null and the destroy helpers skip it.
    splat::ps5::EyeBuffers eye_buffers;
    splat::ps5::CopyPass copy_pass;
    std::unique_ptr<splat::SplatRenderer> renderer;
    splat::VulkanImage target_image;
    splat::VulkanImage alt_target_image;
    Pad dualsense(pad::Device::dualsense, "DualSense", kPadTypeDualsense);
    Pad sense_left(pad::Device::sense_left, "left Sense", kPadTypeSenseLeft);
    Pad sense_right(pad::Device::sense_right, "right Sense", kPadTypeSenseRight);
    RunState run;
    bool hmd2_up = false;

    auto teardown = [&]() {
        const double started = now_ms();
        // The pose log first: the last rows of the run are the interesting ones.
        pose_log.close();
        // The pads first, before the headset's tracker/Hmd2 teardown, which is
        // HLA-PSVR2's order for its Sense controllers (psvr2.c:1593-1613). We
        // have nothing to unregister: the only VrTracker2 handle in this title is
        // hmd2's own (it registers the headset), and the Sense pads are never
        // registered, so closing them is all this side has to do. Pad handles
        // hold no display or GPU memory, so this does not delay stopping
        // reprojection.
        sense_left.close();
        sense_right.close();
        dualsense.close();
        // Then the headset on every path: VR mode off and the buffers
        // unregistered stop reprojection sampling memory this title is about to
        // free, and a title that ends with VrTracker2 live faults the tracker
        // (docs/research/hla-psvr2.md section 3).
        if (hmd2_up) {
            hmd2::shutdown();
            hmd2_up = false;
        }
        if (context && run.gpu_ok) {
            renderer.reset();
            splat::destroy_vulkan_image(*context, target_image);
            splat::destroy_vulkan_image(*context, alt_target_image);
            copy_pass.destroy();
            eye_buffers.destroy();
            context.reset();
        } else if (context && !run.gpu_ok) {
            // A GPU wait timed out: the device may still be running, so nothing
            // this title allocated is freed or destroyed anywhere - not even by
            // a destructor on the way out. The process ends and the system
            // reclaims it; the log says what was leaked.
            line("viewer: vr teardown: leaking the renderer, its buffers, the eye buffers and the "
                 "device (a GPU wait timed out; nothing may be freed under it)");
            copy_pass.forget();
            eye_buffers.forget();
            if (renderer) renderer.release();
            context.release();
        }
        line("viewer: vr teardown %.1f ms", now_ms() - started);
    };

    std::string error;
    if (!eye_buffers.create(context->device(), context->memory_properties(), kEyeBufferCount,
                            2 * kEyeWidth, kEyeHeight, error)) {
        line("viewer: FAIL eye buffers: %s", error.c_str());
        teardown();
        return 5;
    }
    for (int i = 0; i < eye_buffers.count(); i++) {
        line("viewer: vr eye buffer %d: %ux%u RGBA8 at %p (%llu bytes, memory type %u)", i,
             eye_buffers.width(), eye_buffers.height(), eye_buffers.mapped(i),
             static_cast<unsigned long long>(eye_buffers.bytes()), eye_buffers.memory_type(i));
    }

    hmd2::set_logger([](const char* message) { line("%s", message); });
    hmd2::Config config;
    hmd2::ExternalImage external[splat::ps5::kMaxEyeBuffers] = {};
    for (int i = 0; i < kEyeBufferCount; i++) {
        external[i].address = eye_buffers.mapped(i);
        external[i].bytes = static_cast<size_t>(eye_buffers.bytes());
    }
    config.external = external;
    config.external_count = kEyeBufferCount;
    config.image_width = 2 * kEyeWidth;
    config.image_height = kEyeHeight;
    // The TV mirror (viewer.env MIRROR, on by default): Hmd2's social screen
    // shows the headset's view on the TV, so the console's own recording of
    // this title carries it (runs/video_test2: without it the capture is black).
    config.mirror = options.mirror;
    if (!hmd2::init(config)) {
        const hmd2::Error hmd_error = hmd2::error();
        line("viewer: FAIL hmd2 init at %s (rc 0x%08x) - see the hmd2 lines above",
             hmd_error.step != nullptr && hmd_error.step[0] != '\0' ? hmd_error.step : "unknown",
             static_cast<unsigned>(hmd_error.rc));
        teardown();
        return 6;
    }
    hmd2_up = true;
    const hmd2::Fov fov = hmd2::field_of_view();
    const splat::HeadsetProjection projection =
        splat::hmd_projection({fov.out, fov.in, fov.top, fov.bottom}, kEyeWidth, kEyeHeight);
    line("viewer: vr headset: %ux%u per eye, %d buffers, refresh %d Hz output, fov tangents out "
         "%.3f in %.3f top %.3f bottom %.3f -> fx %.2f fy %.2f cx %.1f cy %.1f, per-eye principal "
         "point %+.1f / %+.1f px",
         hmd2::eye_size(), hmd2::eye_height(), hmd2::buffer_count(), hmd2::refresh_hz(),
         static_cast<double>(fov.out), static_cast<double>(fov.in), static_cast<double>(fov.top),
         static_cast<double>(fov.bottom), projection.intrinsics.fx, projection.intrinsics.fy,
         projection.intrinsics.cx, projection.intrinsics.cy, projection.eye_shift[0],
         projection.eye_shift[1]);

    // The TV mirror, once: what the TV shows and at what size. Hmd2's
    // compositor produces it next to the flips this loop already submits (no
    // extra frame, thread or copy on the title's side), so the achieved rate is
    // the display's own and is logged once with the first second's stats.
    const bool mirror_ok = hmd2::mirroring();
    if (!options.mirror) {
        line("viewer: vr mirror: off (MIRROR=0 in viewer.env); the TV keeps the console UI");
    } else if (!mirror_ok) {
        line("viewer: vr mirror: unavailable (Hmd2 refused the social screen, return codes above); "
             "VR continues, the TV keeps the console UI");
    } else {
        const size_t mirror_bytes = hmd2::mirror_display_bytes();
        // The only thing Hmd2 says about the mirror's geometry is the display
        // buffer's size. Name a resolution only when the size is exactly one
        // (four bytes per pixel); otherwise log the size and invent nothing.
        const bool known_1080p = mirror_bytes == 1920ull * 1080ull * 4ull;
        const bool known_4k = mirror_bytes == 3840ull * 2160ull * 4ull;
        line("viewer: vr mirror: on (Hmd2 social screen: the headset's view on the TV, so the "
             "console's recording captures it), display buffer %.1f MiB%s",
             static_cast<double>(mirror_bytes) / (1024.0 * 1024.0),
             known_1080p ? " = 1920x1080 at 4 bytes per pixel"
                         : known_4k ? " = 3840x2160 at 4 bytes per pixel" : "");
    }

    // ---- renderer ---------------------------------------------------------
    splat::RendererOptions renderer_options;
    renderer_options.shader_dir = options.shader_dir;
    renderer_options.eye_width = kEyeWidth;
    renderer_options.eye_height = kEyeHeight;
    renderer_options.stereo = true;
    // The eye buffer is linear RGBA8 (Hmd2's descriptor is UNORM); an sRGB
    // target keeps the premultiplied blend in linear light and the bytes Hmd2
    // samples are sRGB-encoded, the same colour path the TV viewer takes.
    renderer_options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    renderer_options.intrinsics = projection.intrinsics;
    renderer_options.eye_center_shift[0] = projection.eye_shift[0];
    renderer_options.eye_center_shift[1] = projection.eye_shift[1];
    // Black everywhere the photo is not: the clear colour and the comfort fade's
    // backdrop are one value (splat_renderer.h), so the void around the photo
    // and the fade past the comfort radius are black in both eyes and on the TV
    // mirror, which shows the same eye buffers.
    renderer_options.backdrop[0] = 0.0f;
    renderer_options.backdrop[1] = 0.0f;
    renderer_options.backdrop[2] = 0.0f;
    if (loaded.meta.intrinsics.fx > 0.0f && loaded.meta.intrinsics.width > 0) {
        renderer_options.capture_intrinsics = {
            loaded.meta.intrinsics.fx, loaded.meta.intrinsics.fy, loaded.meta.intrinsics.cx,
            loaded.meta.intrinsics.cy};
        renderer_options.capture_width = loaded.meta.intrinsics.width;
        renderer_options.capture_height = loaded.meta.intrinsics.height;
    } else {
        line("viewer: vr no capture intrinsics in meta.json; the capture-frustum fade is off");
    }
    const double renderer_started = now_ms();
    splat::Result<std::unique_ptr<splat::SplatRenderer>> renderer_result;
    auto create_renderer = [&]() {
        renderer_result = splat::SplatRenderer::create(*context, renderer_options, loaded.scene);
    };
    run_with_big_stack(create_renderer, "vr renderer");
    if (!renderer_result.ok()) {
        line("viewer: FAIL renderer: %s", renderer_result.message().c_str());
        teardown();
        return 6;
    }
    renderer = renderer_result.take();
    line("viewer: vr renderer: %ux%u stereo (side by side), shaders %s, create %.1f ms",
         renderer->eye_width(), renderer->eye_height(), options.shader_dir.c_str(),
         now_ms() - renderer_started);
    // The renderer owns the scene's GPU copy now; drop the CPU copy (~34 MiB)
    // before the preload slot allocates its own. meta.json stays: its capture
    // intrinsics are what the comfort fade was created with.
    //
    // The two depth measures are taken here, once per scene, from the CPU copy
    // the upload had: the centre (median) is what the log names and what the
    // viewer reports for the scene it switched to, and the near content bounds
    // a scale press's pivot. Neither is measured per frame - that would cost a
    // pass over 1.2 M splats.
    SceneDepths first_depths = scene_depths(loaded.scene);
    loaded.scene = splat::SplatScene{};
    line("viewer: vr scene centre %.2f m ahead on the capture camera axis, near content %.2f m "
         "(scale: the maquette (Settings > Scale) lands the cone's median centre on a table point "
         "0.7 m ahead / 0.35 m below the eyes and scales there; Front keeps the 5th-percentile "
         "depth put, Centre the median; L2/R2 never move the viewer)",
         static_cast<double>(first_depths.median), static_cast<double>(first_depths.near));

    // ---- the panel (W16) --------------------------------------------------
    // The same panel the TV path builds (ui_setup.h: one UiApp, one UiHost), over
    // the scene this session already has loaded: Options raises it in front of
    // the eyes and the scene keeps rendering behind it. viewer.env's UI key
    // decides whether the headset has a panel at all - VR default: available,
    // UI=0: none, exactly the pre-W16 session - and UI=1 boots into it.
    viewer::Options ui_options = viewer::ui_options_from_env();
    ui_options.scenes_dir = options.scenes_dir;
    bool panel_available = ui_options.ui_given ? ui_options.ui : true;
    std::unique_ptr<viewer::UiRuntime> ui_runtime;
    std::string panel_error;
    if (panel_available) {
        ui_runtime = std::make_unique<viewer::UiRuntime>(viewer::make_ui_runtime(ui_options));
        // The eye intrinsics are passed for symmetry with the TV path; the VR
        // quad only uses the pose (render/ui_panel.h vr_panel_quad).
        if (!ui_runtime->start(*renderer, viewer::UiHost::Mode::Vr, renderer_options.intrinsics)) {
            panel_error = ui_runtime->last_error();
            ui_runtime.reset();
            panel_available = false;
        }
    }
    // A failed panel must not cost the headset session: the scene still runs,
    // and the log says why the menu is not there.
    if (ui_runtime != nullptr) {
        ui_runtime->app->set_scene_loaded(true);
    }
    viewer::PanelMode panel(panel_available, panel_available && ui_options.ui);
    if (ui_runtime != nullptr) {
        line("viewer: vr ui: panel %s (UI %s, %s); Options opens and closes it, Quit is its last "
             "item",
             panel.open() ? "open at boot" : "available",
             ui_options.ui_given ? (ui_options.ui ? "=1" : "=0") : "not set, VR default",
             ui_options.ui_fake ? "fake services" : "models, jobs and captures real");
    } else if (!panel_error.empty()) {
        line("viewer: vr ui: no panel (%s); the session runs with the scene only",
             panel_error.c_str());
    } else {
        line("viewer: vr ui: off (UI=0 in viewer.env); the session runs with the scene only");
    }

    const VkExtent2D extent = renderer->target_extent();
    auto image_result = splat::create_vulkan_color_image(
        *context, renderer_options.target_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image_result.ok()) {
        line("viewer: FAIL target image: %s", image_result.message().c_str());
        teardown();
        return 6;
    }
    target_image = image_result.take();
    if (!copy_pass.create(context->device(), context->queue_family(), context->queue(), error)) {
        line("viewer: FAIL copy pass: %s", error.c_str());
        teardown();
        return 6;
    }
    splat::RenderTarget target;
    target.image = target_image.image;
    target.view = target_image.view;
    target.extent = extent;
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // The second blend-space target for Settings > Display > Blending: the
    // renderer builds pipelines for both the sRGB and the UNORM format, so the
    // loop only has to choose the image. Failure is not fatal: the setting
    // falls back to Linear and the log says why.
    splat::RenderTarget alt_target;
    bool alt_target_ready = false;
    const VkFormat alt_format = splat::blend_alt_format(renderer_options.target_format);
    if (alt_format != renderer_options.target_format) {
        auto alt_result = splat::create_vulkan_color_image(
            *context, alt_format, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (alt_result.ok()) {
            alt_target_image = alt_result.take();
            alt_target.image = alt_target_image.image;
            alt_target.view = alt_target_image.view;
            alt_target.extent = extent;
            alt_target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            alt_target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            alt_target.format = alt_format;
            alt_target_ready = true;
            line("viewer: vr display: two blend targets ready (sRGB for Linear, UNORM for sRGB); "
                 "the Settings toggle switches between them per frame");
        } else {
            line("viewer: vr display: no gamma-blend target (%s); Blending stays Linear",
                 alt_result.message().c_str());
        }
    }

    // ---- viewer state -----------------------------------------------------
    // The Settings screen writes into this same object (ui.json); the VR loop
    // reads the values every frame, so a change applies without a restart.
    // UI=0 or a failed panel falls back to the built-in defaults.
    splat::UiState default_settings;
    splat::UiState* settings =
        (ui_runtime != nullptr && ui_runtime->services.state != nullptr)
            ? ui_runtime->services.state
            : &default_settings;
    splat::HmdView headset;
    headset.set_comfort_radius(options.comfort_radius);
    headset.set_world_scale(options.world_scale);
    headset.set_median_depth(first_depths.median);
    headset.set_near_depth(first_depths.near);
    headset.set_pivot_samples(std::move(first_depths.samples));
    headset.set_scale_pivot_mode(pivot_mode_from_setting(settings->scale_around));

    // One warm-up frame before the headset is waited for: the pipeline compile
    // (ACO, seconds on a cold cache) must not happen in the first frame the
    // user sees. Nothing is submitted, so it cannot be shown. The capture pose
    // is the identity FrameView: scene origin, yaw and pitch zero.
    {
        const splat::FrameView warm;   // capture pose
        const splat::Status rendered = renderer->render(warm, target, false);
        if (!rendered.ok()) {
            line("viewer: FAIL warm-up render: %s", rendered.message().c_str());
            run.gpu_ok = false;
            teardown();
            return 7;
        }
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        line("viewer: vr warm-up frame: project %.2f sort %.2f draw %.2f ms (both eyes), %u "
             "visible splats",
             renderer->timings().project_ms, renderer->timings().sort_ms,
             renderer->timings().draw_ms, renderer->last_visible_count());
    }

    // The pads: the DualSense and both Sense controllers, for the user the Hmd2
    // session was opened for. Missing ones are retried in the loop, slowly.
    const int user = hmd2::user_id();
    const double pads_started = now_ms();
    dualsense.open(user, pads_started);
    sense_left.open(user, pads_started);
    sense_right.open(user, pads_started);
    line("viewer: vr input: Options opens the menu (DualSense or right Sense; its Quit item ends "
         "the session), hold Cross 1 s to recentre (R3 and a short press do nothing), L1/R1 change "
         "scene (a Sense grip hold grabs instead; a tap steps), L2/R2 scale the scene: by default "
         "the maquette lands on a table 0.7 m ahead and scales there (Settings > Scale), left "
         "stick walks "
         "(forward/back, right/left), right stick rises and turns right; Create does nothing, one "
         "table for all pads");

    // ---- the frame loop ---------------------------------------------------
    // One loop for the whole session, in two phases. Until Hmd2 reports the
    // headset worn and the tracker a valid pose that holds still (see
    // hmd_view.h's PlacementWait) - the user may still be setting up the play
    // area, and the tracker redefines its local frame for a second or two
    // after the session opens - each frame submits the still-empty eye buffer
    // (black) so Hmd2's queue keeps moving and the pose stays live; that is the
    // same frame path vr_hello proved. The scene is placed at that settled pose
    // and the cap starts there.
    //
    // Scene switching: the renderer's one-deep staged slot holds the next
    // scene. An L1/R1 that lands on the staged index is a commit (descriptor
    // swap, a few ms); anything else has the slot released first and the
    // requested scene read on demand (~95 ms), with the last submitted frame
    // still being reprojected by Hmd2. The forward preload must never veto a
    // press (scene_switch.h).
    bool intrinsics_noted = false;

    auto current_folder = [&]() { return scenes.current().folder.filename().string(); };

    auto note_intrinsics = [&](const splat::LoadedScene& next) {
        const splat::Intrinsics& now_intrinsics = loaded.meta.intrinsics;
        const splat::Intrinsics& next_intrinsics = next.meta.intrinsics;
        if (intrinsics_noted) return;
        if (next_intrinsics.fx == now_intrinsics.fx && next_intrinsics.cx == now_intrinsics.cx &&
            next_intrinsics.width == now_intrinsics.width) {
            return;
        }
        intrinsics_noted = true;
        line("viewer: vr note: %s has different capture intrinsics (fx %.1f vs %.1f); the "
             "capture-frustum fade keeps the first scene's, as the desktop viewer does",
             current_folder().c_str(), next_intrinsics.fx, now_intrinsics.fx);
    };

    // ---- scene label and HUD ----------------------------------------------
    // "<folder> · <model>" and "scene N/M" on a world-locked plate under the
    // bottom edge of the photo's capture frustum. The R8 mask is rasterised
    // only when the text changes (a scene switch), never per frame; the plate
    // is a 1x1 white mask with a dark tint. Both quads are drawn by the C6
    // renderer after the splats, so the label is not faded by comfort.
    //
    // The label quads are kept as the base geometry (scene coords) and moved
    // with the scene's scale and offset every frame the adjustment changes, so
    // it stays under the photo while the user scales or moves the scene. The
    // HUD (hold progress, scale readout) is head-locked and rebuilt while shown.
    // All of it shares one solid 1x1 texture; the two head-locked text masks
    // (hold, scale) are uploaded when their string changes.
    uint32_t label_text_texture = UINT32_MAX;
    uint32_t solid_texture = UINT32_MAX;
    std::vector<splat::OverlayQuad> label_base;
    bool overlay_dirty = true;
    bool hud_shown = false;
    // Whether the last quad list carried the panel (W16): the list is rebuilt on
    // the frame the panel appears or disappears, like a HUD element.
    bool overlay_ui = false;
    bool overlay_marker = false;
    float overlay_scale = -1.0f;
    float overlay_offset[3] = {0.0f, 0.0f, 0.0f};
    splat::HudLayout hud_layout;
    splat::HudTextures hud_textures;
    float hold_progress = -1.0f;
    double scale_hint_until = 0.0;
    double scale_marker_until = 0.0;
    float scale_vignette = 0.0f;
    std::string scale_text =
        scale_readout_text(headset.scale_pivot_mode(), headset.world_scale());

    // The solid texture the label plate and every HUD bar share.
    {
        const uint8_t white = 255;
        auto texture = renderer->add_overlay_texture(1, 1, &white);
        if (texture.ok()) {
            solid_texture = texture.value();
        } else {
            line("viewer: vr overlay: %s; no label plate or HUD background",
                 texture.message().c_str());
        }
    }
    hud_textures.solid = solid_texture;

    // The scale press's pivot marker: a 64x64 R8 ring, drawn as a small
    // world-locked billboard at the pivot (nothing to model, one texture).
    uint32_t marker_texture = UINT32_MAX;
    {
        constexpr int kRingSize = 64;
        std::vector<uint8_t> ring(static_cast<size_t>(kRingSize) * kRingSize, 0);
        const float centre = 0.5f * static_cast<float>(kRingSize - 1);
        for (int y = 0; y < kRingSize; ++y) {
            for (int x = 0; x < kRingSize; ++x) {
                const float dx = static_cast<float>(x) - centre;
                const float dy = static_cast<float>(y) - centre;
                const float radius = std::sqrt(dx * dx + dy * dy) / centre;
                // A soft 1.5 px ring at ~0.78 of the cell.
                const float edge = 1.0f - std::min(1.0f, std::fabs(radius - 0.78f) * 10.0f);
                ring[static_cast<size_t>(y) * kRingSize + x] =
                    static_cast<uint8_t>(std::clamp(edge, 0.0f, 1.0f) * 255.0f);
            }
        }
        auto texture = renderer->add_overlay_texture(kRingSize, kRingSize, ring.data());
        if (texture.ok()) {
            marker_texture = texture.value();
        } else {
            line("viewer: vr HUD: %s; the scale pivot has no marker", texture.message().c_str());
        }
    }

    // The hold line's mask never changes; the scale line changes one decimal at
    // a time while the trigger is held, and keeps one mask size throughout.
    {
        const splat::TextImage hold_image =
            splat::rasterise_text({splat::kHoldHudText}, hud_layout.pixel_scale);
        auto texture =
            renderer->add_overlay_texture(hold_image.width, hold_image.height,
                                          hold_image.pixels.data());
        if (texture.ok()) {
            hud_textures.hold_text = texture.value();
        } else {
            line("viewer: vr HUD: %s; the recentre hold has no text",
                 texture.message().c_str());
        }
        const splat::TextImage scale_image =
            splat::rasterise_text({scale_text}, hud_layout.pixel_scale);
        auto scale_texture =
            renderer->add_overlay_texture(scale_image.width, scale_image.height,
                                          scale_image.pixels.data());
        if (scale_texture.ok()) {
            hud_textures.scale_text = scale_texture.value();
        } else {
            line("viewer: vr HUD: %s; the scale readout has no text",
                 scale_texture.message().c_str());
        }
    }

    auto refresh_label = [&](size_t index) {
        const double started = now_ms();
        const splat::Path folder = scenes.entry(index).folder;
        const std::string folder_name = folder.filename().string();
        splat::SceneMeta meta;
        const auto meta_result = splat::read_scene_meta(splat::scene_files(folder).meta);
        if (meta_result.ok()) meta = meta_result.value();
        auto built = splat::make_scene_label(meta, folder_name, index, scenes.size());
        if (!built.ok()) {
            line("viewer: vr label: %s; the scene has no label", built.message().c_str());
            label_base.clear();
            overlay_dirty = true;
            return;
        }
        splat::SceneLabel label = built.take();

        // Without the solid texture there is no plate to draw: the failure was
        // already logged once when the texture was created.
        if (solid_texture == UINT32_MAX) {
            label_base.clear();
            overlay_dirty = true;
            return;
        }
        splat::Status status;
        if (label_text_texture == UINT32_MAX) {
            auto texture = renderer->add_overlay_texture(label.image.width, label.image.height,
                                                         label.image.pixels.data());
            if (!texture.ok()) {
                line("viewer: vr label: %s; the scene has no label", texture.message().c_str());
                label_base.clear();
                overlay_dirty = true;
                return;
            }
            label_text_texture = texture.value();
        } else {
            status = renderer->update_overlay_texture(label_text_texture, label.image.width,
                                                      label.image.height,
                                                      label.image.pixels.data());
            if (!status.ok()) {
                line("viewer: vr label texture: %s", status.message().c_str());
                return;
            }
        }
        label.plate.texture = solid_texture;
        label.text.texture = label_text_texture;
        label_base = {label.plate, label.text};
        overlay_dirty = true;
        line("viewer: vr label \"%s\" / \"%s\": %ux%u R8, %.1f cm glyphs at %.1f m, %.0f cm "
             "below the photo, %.1f x %.1f cm plate, updated in %.1f ms",
             label.title.c_str(), label.counter.c_str(), label.image.width, label.image.height,
             static_cast<double>(label.glyph_height) * 100.0,
             static_cast<double>(label.distance),
             static_cast<double>(label.top_y - label.photo_bottom_y) * 100.0,
             static_cast<double>(label.plate.width) * 100.0,
             static_cast<double>(label.plate.height) * 100.0, now_ms() - started);
    };

    // The camera's right/up in world space, the same axes the renderer builds
    // from a FrameView's yaw/pitch/roll (splat_renderer.cpp). Used to keep the
    // scale pivot marker facing the user.
    auto camera_axes = [](const splat::CameraPose& pose, float right[3], float up[3]) {
        const float cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
        const float cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
        const float cr = std::cos(pose.roll), sr = std::sin(pose.roll);
        const float f[3] = {sy * cp, sp, cy * cp};
        const float s0[3] = {cy, 0.0f, -sy};
        const float d0[3] = {f[1] * s0[2] - f[2] * s0[1], f[2] * s0[0] - f[0] * s0[2],
                             f[0] * s0[1] - f[1] * s0[0]};
        for (int axis = 0; axis < 3; ++axis) {
            right[axis] = cr * s0[axis] + sr * d0[axis];
            up[axis] = -(cr * d0[axis] - sr * s0[axis]);
        }
    };

    // Rebuilds the overlay's quad list: the scene label moved by the scene's
    // scale and offset, plus whatever HUD is up this frame, plus the panel when
    // it is visible (W16: the same UiHost quads the TV merges). Called once per
    // frame while something moves, a HUD element is visible, or the panel is
    // visible - its lazy follower rebuilds the quads every frame
    // (overlay_refresh.h); the vertex buffer is rewritten in place, so this is
    // a memcpy of a few tens of floating-point values.
    auto update_overlay = [&](const splat::FrameView& view, double now) {
        const bool hud_visible = hold_progress >= 0.0f || now < scale_hint_until;
        const bool marker_visible = now < scale_marker_until && marker_texture != UINT32_MAX;
        const bool ui_visible = ui_runtime != nullptr && ui_runtime->host->visible();
        const bool ui_changed = ui_visible != overlay_ui;
        const bool marker_changed = marker_visible != overlay_marker;
        const float scale = headset.world_scale();
        const float* offset = headset.scene_offset();
        const bool moved = scale != overlay_scale || offset[0] != overlay_offset[0] ||
                           offset[1] != overlay_offset[1] || offset[2] != overlay_offset[2];
        // `hud_shown` is the previous frame's state: the frame the last HUD
        // element disappears still has to rebuild the list without it. A visible
        // panel is live - its quads follow the head every frame - so it is
        // pushed every frame, not only on the frame it opens and closes.
        const viewer::OverlayRefresh refresh{ui_visible, hud_visible, hud_shown, moved,
                                             overlay_dirty, ui_changed, marker_changed};
        if (!viewer::overlay_needs_update(refresh)) return;
        hud_shown = hud_visible;
        overlay_ui = ui_visible;
        overlay_marker = marker_visible;
        overlay_scale = scale;
        for (int axis = 0; axis < 3; ++axis) overlay_offset[axis] = offset[axis];

        std::vector<splat::OverlayQuad> quads;
        quads.reserve(label_base.size() + 6 + (ui_visible ? 2u : 0u));
        for (const splat::OverlayQuad& base : label_base) {
            splat::OverlayQuad quad = base;
            for (int axis = 0; axis < 3; ++axis) {
                quad.center[axis] = base.center[axis] * scale + offset[axis];
            }
            quad.width = base.width * scale;
            quad.height = base.height * scale;
            quads.push_back(quad);
        }
        splat::HudState state;
        state.hold_progress = hold_progress;
        state.show_scale = now < scale_hint_until;
        const std::vector<splat::OverlayQuad> hud =
            splat::hud_quads(view.midpoint, state, hud_layout, hud_textures);
        quads.insert(quads.end(), hud.begin(), hud.end());
        if (marker_visible) {
            // A small world-locked ring at the frozen pivot: the point that
            // stays put while the scene scales around it.
            float right[3], up[3];
            camera_axes(view.midpoint, right, up);
            const float* pivot = headset.scale_pivot_world();
            splat::OverlayQuad marker;
            for (int axis = 0; axis < 3; ++axis) {
                marker.center[axis] = pivot[axis];
                marker.right[axis] = right[axis];
                marker.up[axis] = up[axis];
            }
            marker.width = 0.036f;
            marker.height = 0.036f;
            marker.texture = marker_texture;
            // A white-blue ring, fading with the press's eased vignette.
            const float alpha = 0.35f + 0.6f * std::clamp(scale_vignette, 0.0f, 1.0f);
            marker.tint[0] = 0.86f;
            marker.tint[1] = 0.93f;
            marker.tint[2] = 1.0f;
            marker.tint[3] = alpha;
            quads.push_back(marker);
        }
        // The panel last: it is the thing the user is looking at, and its quad is
        // world-locked at the pose it was opened from (UiHost, Mode::Vr).
        if (ui_visible) {
            const std::vector<splat::OverlayQuad> panel_quads = ui_runtime->host->quads();
            quads.insert(quads.end(), panel_quads.begin(), panel_quads.end());
        }

        const splat::Status status = renderer->set_overlay_quads(std::move(quads));
        if (!status.ok()) {
            line("viewer: vr overlay: %s", status.message().c_str());
            return;
        }
        overlay_dirty = false;
    };

    auto set_scale_text = [&](float scale) {
        // Fixed width (kScaleHudChars) so the mask keeps one size; the HUD's
        // quad is sized from that constant.
        std::string line_text = scale_readout_text(headset.scale_pivot_mode(), scale);
        if (scale_text == line_text) return;
        scale_text = line_text;
        const splat::TextImage image = splat::rasterise_text({scale_text}, hud_layout.pixel_scale);
        if (image.empty() || hud_textures.scale_text == UINT32_MAX) return;
        const splat::Status status = renderer->update_overlay_texture(
            hud_textures.scale_text, image.width, image.height, image.pixels.data());
        if (!status.ok()) line("viewer: vr HUD scale text: %s", status.message().c_str());
        overlay_dirty = true;
    };

    // The initial scene's label, now that the renderer and the playlist are
    // both ready. Every later scene switch refreshes it in show_scene().
    refresh_label(scenes.index());

    // The preload slot: the renderer's staging buffers, the worker that reads
    // the next scene, and the playlist bookkeeping (see VrSwitchSlot and
    // scene_switch.h). `on_load` keeps the capture-intrinsics note.
    VrSwitchSlot switch_slot(*renderer, scenes);
    switch_slot.on_load = [&](const splat::LoadedScene& next) { note_intrinsics(next); };

    // Everything after a successful switch: its log lines, the new scene's two
    // depth measures and its label. Shared by L1/R1 (over switch_slot) and the
    // panel's ViewScene, which stages the album's scene through a slot of its own
    // (W16) before it adopts that album.
    auto apply_switch = [&](size_t index, const VrSwitchSlot& slot,
                            const viewer::SwitchReport& report, double started) {
        const float median_depth = slot.applied_median_depth();
        if (report.used_staged) {
            line("viewer: vr scene %zu/%zu %s: preloaded, committed in %.1f ms (%.1f ms "
                 "device-idle included)",
                 index + 1, scenes.size(),
                 scenes.entry(index).folder.filename().string().c_str(), now_ms() - started,
                 slot.commit_ms());
        } else {
            line("viewer: vr scene %zu/%zu %s: loaded on demand%s, read %.1f ms + stage %.1f ms + "
                 "commit %.1f ms",
                 index + 1, scenes.size(), scenes.entry(index).folder.filename().string().c_str(),
                 report.freed_preload ? " (forward preload dropped for the press)" : "",
                 slot.read_ms(), slot.stage_ms(), slot.commit_ms());
            if (report.retried) {
                line("viewer: vr scene %zu/%zu %s: the staged slot still held a scene on the first "
                     "upload; released it and managed the press",
                     index + 1, scenes.size(),
                     scenes.entry(index).folder.filename().string().c_str());
            }
        }
        // A scene switch keeps the world placement: the new scene's capture
        // camera goes to the same anchor, with the same scale and offset, so
        // the user does not get pulled back to the photo's viewpoint (2026-10-26
        // feedback). Only the new scene's centre and near depth change.
        headset.set_median_depth(median_depth);
        headset.set_near_depth(slot.applied_near_depth());
        headset.set_pivot_samples(slot.applied_pivot_samples());
        line("viewer: vr scene %zu/%zu %s: %u splats, last frame held during the switch, "
             "placement kept (anchor %.2f %.2f %.2f m, scale %.2fx, scene centre %.2f m, near "
             "content %.2f m)",
             index + 1, scenes.size(), current_folder().c_str(), renderer->scene_count(),
             static_cast<double>(headset.anchor()[0]), static_cast<double>(headset.anchor()[1]),
             static_cast<double>(headset.anchor()[2]), static_cast<double>(headset.world_scale()),
             static_cast<double>(median_depth), static_cast<double>(slot.applied_near_depth()));
        refresh_label(index);
    };

    // W19: only the model-load phase holds a scene load (W18 run B: inference
    // leaves room for a 0.28 GiB scene upload next to the resident model). Both
    // scene load routes below - the L1/R1 switch and the panel's ViewScene - and
    // the forward preload ask this first; the viewer keeps browsing during the
    // inference itself.
    auto scene_loads_paused = [&]() {
        return ui_runtime != nullptr && ui_runtime->services.jobs != nullptr &&
               viewer::job_loading_model(*ui_runtime->services.jobs);
    };

    // Switches to playlist `index`: a commit when it is the preloaded scene,
    // otherwise a read-and-stage with the forward preload dropped first. Both
    // paths leave the world placement alone (the placement, scale and offset
    // are the user's; only the new scene's two depth measures change).
    auto show_scene = [&](size_t index) -> bool {
        if (scene_loads_paused()) {
            line("viewer: vr scene switch waits for the model load to finish");
            return false;
        }
        const double started = now_ms();
        const viewer::SwitchReport report = viewer::switch_scene(switch_slot, index);
        if (!report.switched) return false;
        scenes.set_index(index);
        apply_switch(index, switch_slot, report, started);
        return true;
    };

    auto service_preload = [&]() {
        if (!options.preload || scene_loads_paused()) return;
        switch_slot.prefetch_next();
    };

    struct Counters {
        uint64_t frames = 0;
        uint64_t frames_this_second = 0;
        double project_sum = 0.0;
        double sort_sum = 0.0;
        double draw_sum = 0.0;
        double copy_sum = 0.0;
        double copy_second = 0.0;
        double work_max = 0.0;
        uint64_t slow_frames = 0;
        uint64_t submit_errors = 0;
        uint64_t wait_frames = 0;
    } counters;
    bool placed = false;
    uint64_t wait_submit_errors = 0;
    const double wait_started = now_ms();
    double next_wait_log = 0.0;
    double placed_at = 0.0;
    double second_started = 0.0;
    double project_second = 0.0;
    double sort_second = 0.0;
    double draw_second = 0.0;
    double last_fade = 1.0;
    int begin_failures = 0;
    // The mirror's per-second rate: the VR port's flips the display showed
    // (each one is a fresh image for the mirror too), and its one-shot line.
    uint64_t mirror_shown = 0;
    bool mirror_logged = false;
    int status = 0;
    const char* reason = "PASS";
    // The panel's Quit item: the frame loop ends here, the one teardown below
    // runs as for SECONDS and the quit file (W16).
    bool panel_quit = false;
    double next_quit_check = 0.0;
    // W25: the Sense right controller's laser. `pointer_filter` smooths the
    // panel pixel the ray hits; `pointer_r2_down` is the previous frame's
    // trigger state for the rising edge; `pointer_registered` is the one-time
    // VrTracker2 registration (the pad handle is the tracker handle).
    splat::PointerFilter pointer_filter;
    bool pointer_r2_down = false;
    bool pointer_registered = false;
    bool pointer_register_failed_logged = false;
    double next_pointer_log = 0.0;
    // The Cross hold (one second, any pad) and the stick/trigger gestures:
    // their state machines run once per frame here, and their starts and ends
    // are logged as events. `placement_wait` is the first placement's gate
    // (hmd_view.h): worn + tracked + no tracker jump for half a second.
    pad::RecentreHold recentre_hold;
    splat::PlacementWait placement_wait;
    double last_input_ms = now_ms();
    bool scale_gesture = false;
    double scale_gesture_started = 0.0;   // for the press's ease-in
    // VR8: the locomotion velocity is eased in and out (150 ms).
    pad::LocomotionEase locomotion;
    // The previous frame's raw head quaternion, for "not during a fast head
    // turn": a press that starts while the head sweeps is ignored.
    float previous_head_quaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    double previous_head_ms = 0.0;
    bool have_previous_head = false;
    bool move_gesture = false;
    // W26: the Sense grab. `grab_state` is the gesture's memory (the hands'
    // previous poses), `grab_gesture` its start/end log edge, and the two
    // booleans the one-time VrTracker2 registration each Sense pad needs before
    // controller_pose() returns anything. UNVERIFIED on hardware.
    splat::GrabState grab_state;
    bool grab_gesture = false;
    // Each Sense pad's VrTracker2 registration: 0 not tried yet, 1 registered,
    // -1 refused (one attempt, one line - a failure must not retry every frame).
    int sense_left_tracker = 0;
    int sense_right_tracker = 0;
    // The pose log's per-frame state: the tracker result this frame rendered
    // from (the same values Hmd2 got in SetParam) and the event of this frame,
    // if any. `last_raw` starts untracked, so the rows written while the
    // headset is still being put on say so.
    splat::HeadPoses last_raw;
    last_raw.head.tracked = false;
    char log_event[64] = {};
    uint32_t log_actions = 0;
    auto log_pose = [&](const char* event) {
        // `yaw_deg` is the yaw the renderer used, turn included: view_yaw() is
        // scene yaw minus the right-stick turn, and `turn_deg` records the turn
        // itself (docs/console-runbook.md). `hold_progress` is the hold's 0..1,
        // 0 when nothing is held - never the HUD's negative "no element".
        pose_log.row((now_ms() - session_started) / 1000.0, counters.frames, last_raw.head,
                     headset.view_yaw() * kRadToDeg, headset.scene_pitch() * kRadToDeg,
                     headset.scene_roll() * kRadToDeg, dualsense.buttons(), sense_left.buttons(),
                     sense_right.buttons(), log_actions, headset.world_scale(),
                     headset.scene_offset(), headset.scene_turn() * kRadToDeg,
                     pad::logged_hold_progress(hold_progress), current_folder().c_str(), event);
    };

    // The one placement operation, used by the first placement and by the Cross
    // hold: the cached head pose becomes the scene's frame (the capture camera
    // at the eyes, looking forward, level and upright), the scene adjustments
    // are cleared and the HUD goes back to the configured scale. It returns the
    // yaw it removed, for the log. One path for both is the fix for the
    // 2026-10-27 headset run, where the first placement landed ~90 deg off and
    // the user had to recentre to bring the photo in front.
    auto place_scene_at_head = [&]() {
        const double removed_yaw = headset.view_yaw() * kRadToDeg;
        headset.recentre_here();
        headset.reset_scene_adjust();
        set_scale_text(headset.world_scale());
        overlay_dirty = true;
        return removed_yaw;
    };

    // The scene step the DualSense's L1/R1 and a Sense grip tap share (W26:
    // a held Sense grip is the grab, a tap steps). `key` names the press in
    // the log; `next` is the direction, and the event is the same `scene`.
    auto step_scene = [&](const char* key, bool next) {
        if (scenes.size() <= 1) return;
        const size_t target_index =
            next ? (scenes.index() + 1) % scenes.size()
                 : (scenes.index() + scenes.size() - 1) % scenes.size();
        line("viewer: vr %s -> scene %zu/%zu", key, target_index + 1, scenes.size());
        show_scene(target_index);
        std::snprintf(log_event, sizeof log_event, "%s", "scene");
    };

    for (;;) {
        const double now_loop = now_ms();
        log_event[0] = '\0';
        if (placed && (now_loop - placed_at) / 1000.0 >= static_cast<double>(options.cap_seconds)) {
            line("viewer: vr session cap reached (%.0f s of %d s)", (now_loop - placed_at) / 1000.0,
                 options.cap_seconds);
            log_pose("cap");
            break;
        }
        // Input: edge-triggered, once per frame, from every pad that is there.
        // A missing pad is retried here at kPadRetryMs, not every frame.
        dualsense.open(user, now_loop);
        sense_left.open(user, now_loop);
        sense_right.open(user, now_loop);
        // W26: the grab reads both hands every frame, so each Sense pad is
        // registered with VrTracker2 as soon as it is open - W25 registered the
        // right one only while the panel was up, which a grab behind a closed
        // panel never reaches. register_controller() is idempotent per handle,
        // so W25's own call is a no-op once this has run. UNVERIFIED on
        // hardware: the runbook's morning check is what proves the tracked grab.
        if (sense_left.handle() >= 0 && sense_left_tracker == 0) {
            sense_left_tracker = hmd2::register_controller(sense_left.handle(), false) ? 1 : -1;
            line("viewer: grab: left Sense pad %d %s (device 2)%s", sense_left.handle(),
                 sense_left_tracker > 0 ? "registered with VrTracker2"
                                        : "tracker registration failed",
                 sense_left_tracker > 0 ? "; the tracked grab is UNVERIFIED on hardware"
                                        : "; the grab stays off for that hand");
        }
        if (sense_right.handle() >= 0 && sense_right_tracker == 0) {
            sense_right_tracker = hmd2::register_controller(sense_right.handle(), true) ? 1 : -1;
            line("viewer: grab: right Sense pad %d %s (device 1)%s", sense_right.handle(),
                 sense_right_tracker > 0 ? "registered with VrTracker2"
                                         : "tracker registration failed",
                 sense_right_tracker > 0 ? "; the tracked grab is UNVERIFIED on hardware"
                                         : "; the grab stays off for that hand");
        }
        // A Sense L1/R1 is its grip (W26), so it is not an immediate scene
        // switch like the DualSense's: a held grip is the grab, and only a tap
        // (released inside the tap window with the hand still) steps the scene,
        // through grab_state.take_tap() below. Dropping the action bits here is
        // what keeps the press from both grabbing and stepping at once; the
        // panel's ViewScene is still there. Everything else a Sense pad sends
        // is unchanged.
        const uint32_t scene_keys = pad::kPrevScene | pad::kNextScene;
        const uint32_t pressed = dualsense.take_pressed() |
                                 (sense_left.take_pressed() & ~scene_keys) |
                                 (sense_right.take_pressed() & ~scene_keys);
        const uint32_t held_buttons = dualsense.held() | sense_left.held() | sense_right.held();
        log_actions = pressed;
        // W16: one frame's routing (ui_mode.h). Options opens the panel; while it
        // is up it takes the whole button word and the left stick through
        // pad::ui_actions_for (the one input map, pad_input.h) and the scene's own
        // controls are suspended - a stick held when it opened cannot keep walking
        // the viewer behind it.
        const float ui_x = pad::clamp_unit(dualsense.axes().left_x + sense_left.axes().left_x);
        const float ui_y = pad::clamp_unit(dualsense.axes().left_y + sense_left.axes().left_y);
        const viewer::UiRoute route = panel.route(held_buttons, ui_x, ui_y);
        if (route.opened) line("viewer: ui open");
        if (now_loop >= next_quit_check) {
            next_quit_check = now_loop + 1000.0;
            struct stat quit_status {};
            if (::stat(kQuitPath.c_str(), &quit_status) == 0 &&
                quit_status.st_mtime + 1 >= started_wall) {
                line("viewer: vr quit requested (%s); ending", kQuitPath.c_str());
                log_pose("quit");
                break;
            }
        }
        if (!placed && (now_loop - wait_started) / 1000.0 >= kPoseWaitLimitSeconds) {
            line("viewer: FAIL no usable pose after %.0f s (headset worn and a tracker pose are "
                 "both required); is the headset on and the play area set up?",
                 kPoseWaitLimitSeconds);
            status = 1;
            reason = "FAIL no pose";
            log_pose("fail");
            break;
        }

        // The input dt: seconds since the previous loop, with a hitch (a scene
        // load, a GPU stall) treated as a freeze rather than motion, and a
        // short slow frame capped at 20 ms so it cannot apply a double step.
        const double raw_input_seconds =
            std::max(0.0, (now_loop - last_input_ms) / 1000.0);
        const double input_seconds =
            raw_input_seconds > kInputHitchSeconds
                ? 0.0
                : std::min(kMaxInputSeconds, raw_input_seconds);
        last_input_ms = now_loop;

        // The previous rendered pose's angular speed, for the "no scaling while
        // the head sweeps" gate. last_raw is the pose the previous frame used.
        double head_speed_dps = 0.0;
        if (last_raw.head.tracked && have_previous_head) {
            double dot = 0.0;
            for (int i = 0; i < 4; ++i) {
                dot += static_cast<double>(last_raw.head.quaternion[i]) *
                       static_cast<double>(previous_head_quaternion[i]);
            }
            dot = std::max(-1.0, std::min(1.0, dot));
            const double angle = 2.0 * std::acos(std::fabs(dot)) * kRadToDeg;
            const double seconds = (now_loop - previous_head_ms) / 1000.0;
            if (seconds > 0.0) head_speed_dps = angle / seconds;
        }
        if (last_raw.head.tracked) {
            for (int i = 0; i < 4; ++i) previous_head_quaternion[i] = last_raw.head.quaternion[i];
            previous_head_ms = now_loop;
            have_previous_head = true;
        }

        // The Settings values are read every frame; the mode and rates apply to
        // the next press / stick frame without a restart.
        const splat::ScalePivotMode pivot_mode =
            pivot_mode_from_setting(settings->scale_around);
        headset.set_scale_pivot_mode(pivot_mode);
        renderer->set_low_pass_eps(settings->edge_softness);

        hold_progress = -1.0f;
        if (placed && route.scene_controls) {
            // ---- Sense grab (W26) ------------------------------------------
            // L1 (left Sense) and R1 (right Sense) are the grips (pad_input.h
            // kGripLeft/kGripRight, HLA-PSVR2 psvr2.c:530). One held: the
            // scene keeps its place in that hand's frame, so it follows the
            // hand's motion and turn. Both held: it scales about the point
            // between the hands. Only the scene transform changes - the head,
            // the anchor and the comfort radius are untouched, so the viewer
            // never moves ("sends us flying" was VR5's scale compensation).
            // A press starts that grab at once; released inside the tap window
            // with the hand still, it is a tap instead: the grab's own delta is
            // undone and the press steps the scene (GrabTap, the DualSense
            // L1/R1 mapping). The panel's route suspends this whole block, so a
            // grab cannot run behind the menu; the sticks below act on its
            // result in the same frame. UNVERIFIED on hardware.
            const bool grip_left = (sense_left.held() & pad::kGripLeft) != 0;
            const bool grip_right = (sense_right.held() & pad::kGripRight) != 0;
            // The head pose the previous frame was rendered with (the tracker
            // sample `last_raw` holds), which is the one the hand poses belong
            // to: frame() is the same call the renderer below uses.
            const bool gripping = grip_left || grip_right;
            splat::HandPose hands[2];
            splat::FrameView grab_view;
            if (gripping || grab_state.active()) grab_view = headset.frame();
            if (gripping && last_raw.head.tracked) {
                splat::GrabFrame frame;
                for (int axis = 0; axis < 3; ++axis) {
                    frame.head_position[axis] = last_raw.head.position[axis];
                    frame.scene_offset[axis] = headset.scene_offset()[axis];
                }
                for (int i = 0; i < 4; ++i) {
                    frame.head_quaternion[i] = last_raw.head.quaternion[i];
                }
                frame.head_scene = grab_view.midpoint;
                // The head's own yaw in the placement frame: the renderer's
                // camera yaw has the right-stick turn taken out of it, and the
                // turn is not part of where the head is.
                frame.head_scene.yaw = static_cast<float>(headset.scene_yaw());
                float position[3];
                float quaternion[4];
                if (grip_left && hmd2::controller_pose(sense_left.handle(), position, quaternion)) {
                    hands[0] = splat::grab_hand_pose(frame, position, quaternion, true);
                }
                if (grip_right &&
                    hmd2::controller_pose(sense_right.handle(), position, quaternion)) {
                    hands[1] = splat::grab_hand_pose(frame, position, quaternion, true);
                }
            }
            if (hands[0].grabbing || hands[1].grabbing || grab_state.active()) {
                // The viewer's own transform -> the grab's -> back. The grab
                // always works from what the viewer actually stores, so a stick
                // press composes instead of fighting it.
                float head_placement[3];
                splat::ViewerAdjust adjust;
                adjust.scale = headset.world_scale();
                adjust.turn = headset.scene_turn();
                for (int axis = 0; axis < 3; ++axis) {
                    head_placement[axis] =
                        grab_view.midpoint.position[axis] + headset.scene_offset()[axis];
                    adjust.offset[axis] = headset.scene_offset()[axis];
                }
                const splat::SceneTransform before =
                    splat::scene_transform(adjust, head_placement);
                const splat::SceneTransform after =
                    splat::update_grab(grab_state, hands[0], hands[1], before,
                                       now_loop / 1000.0);
                bool changed = after.scale != before.scale;
                for (int i = 0; i < 4; ++i) {
                    changed = changed || after.rotation[i] != before.rotation[i];
                }
                for (int axis = 0; axis < 3; ++axis) {
                    changed = changed || after.offset[axis] != before.offset[axis];
                }
                if (changed) {
                    const splat::ViewerAdjust wanted = splat::viewer_adjust(after, head_placement);
                    if (wanted.scale != adjust.scale && adjust.scale > 0.0f) {
                        headset.scale_scene(wanted.scale / adjust.scale);
                        set_scale_text(headset.world_scale());
                        scale_hint_until = now_loop + kScaleHintMs;
                    }
                    if (wanted.turn != adjust.turn) {
                        headset.turn_scene(wanted.turn - adjust.turn);
                    }
                    // scale_scene() keeps its own frozen cone pivot fixed, so
                    // it moves the offset too; read the offset back and move the
                    // rest of the way to the grab's. turn_scene() leaves it
                    // alone.
                    const float dx = wanted.offset[0] - headset.scene_offset()[0];
                    const float dy = wanted.offset[1] - headset.scene_offset()[1];
                    const float dz = wanted.offset[2] - headset.scene_offset()[2];
                    if (dx != 0.0f || dy != 0.0f || dz != 0.0f) {
                        // move_scene() moves the scene along the head's yaw
                        // frame (the frame the sticks use), the grab's offset
                        // is in the placement frame.
                        const float yaw = static_cast<float>(headset.scene_yaw());
                        const float c = std::cos(yaw);
                        const float s = std::sin(yaw);
                        headset.move_scene(c * dx - s * dz, s * dx + c * dz, -dy);
                    }
                }
                const bool grabbing_now = hands[0].grabbing || hands[1].grabbing;
                if (grabbing_now && !grab_gesture) {
                    grab_gesture = true;
                    std::snprintf(log_event, sizeof log_event, "%s", "grab start");
                    line("viewer: grab start: %s, scale=%.2fx (the scene follows the hand; the "
                         "viewer does not move)",
                         hands[0].grabbing && hands[1].grabbing
                             ? "both hands"
                             : (hands[0].grabbing ? "left hand" : "right hand"),
                         static_cast<double>(headset.world_scale()));
                } else if (!grabbing_now && grab_gesture) {
                    grab_gesture = false;
                    // The grab's own scaling went through scale_scene(), which
                    // begins a scale gesture; end it here, so the pivot state
                    // is clean for the next L2/R2 press and its "scale end"
                    // event. A trigger that is down at the same time keeps its
                    // own gesture (the block below ends that one).
                    if (!scale_gesture) headset.end_scale_gesture();
                    std::snprintf(log_event, sizeof log_event, "%s", "grab end");
                    line("viewer: grab end: scale=%.2fx, offset %.2f %.2f %.2f m kept (the viewer "
                         "moved 0.00 m)",
                         static_cast<double>(headset.world_scale()),
                         static_cast<double>(headset.scene_offset()[0]),
                         static_cast<double>(headset.scene_offset()[1]),
                         static_cast<double>(headset.scene_offset()[2]));
                }
                // A grip released inside the tap window with the hand still
                // was a tap, not a grab: the grab's own tiny delta has been
                // undone above, and the press steps the scene like the
                // DualSense's L1/R1 (left = previous, right = next).
                const splat::GrabTap tap = grab_state.take_tap();
                if (tap == splat::GrabTap::previous) {
                    log_actions |= pad::kPrevScene;
                    step_scene("L1 tap", false);
                } else if (tap == splat::GrabTap::next) {
                    log_actions |= pad::kNextScene;
                    step_scene("R1 tap", true);
                }
            }

            // Cross held for one second recentres. A short press and R3 do
            // nothing; releasing early cancels. The HUD bar follows progress.
            const pad::RecentreHold::Step hold =
                recentre_hold.update((held_buttons & pad::kCross) != 0,
                                     (now_loop - session_started) / 1000.0);
            if (hold.completed) {
                // One placement path: the same call the first placement uses.
                // The yaw removed is the yaw the renderer was using, turn
                // included: recentre clears the turn as well as re-anchoring.
                const double removed_yaw = place_scene_at_head();
                log_actions |= pad::kRecentre;
                std::snprintf(log_event, sizeof log_event, "%s", "recentre");
                line("viewer: vr recentred: Cross held %.1f s, capture camera placed at the head "
                     "(%.1f deg of yaw removed), scale reset to %.2fx, offset and turn cleared",
                     hold.held_seconds, removed_yaw,
                     static_cast<double>(headset.world_scale()));
            } else if (hold.cancelled) {
                line("viewer: vr recentre cancelled: Cross released after %.0f ms (a short press "
                     "does nothing)",
                     hold.held_seconds * 1000.0);
            }
            // While Cross is held the HUD bar follows the progress, and the
            // completion frame shows (and logs) a full 1.0. A negative value is
            // the HUD's "draw nothing" marker; the CSV clamps it to 0.
            if (recentre_hold.active()) {
                hold_progress = recentre_hold.progress();
            }

            // Sticks and triggers: L2/R2 scale the scene about a point fixed in
            // the frozen cone pivot, the left stick walks and the right stick
            // rises and turns (all three move the *viewer*, not the scene in
            // the opposite direction - headset test 4).
            const pad::ViewControl control =
                pad::view_control(dualsense.axes(), sense_left.axes(), sense_right.axes());

            if (control.scale != 0.0f && (!scale_gesture &&
                                          head_speed_dps > kScaleMaxHeadDegreesPerSecond)) {
                // The head is sweeping: do not begin a press now. The trigger
                // still works as soon as the turn settles.
            } else if (control.scale != 0.0f) {
                if (!scale_gesture) {
                    scale_gesture = true;
                    scale_gesture_started = now_loop;
                    headset.begin_scale_gesture();
                    const float* pivot = headset.scale_pivot_world();
                    std::snprintf(log_event, sizeof log_event, "%s", "scale start");
                    line("viewer: vr scale start: %s from %.2fx about the %s pivot %.2f m ahead "
                         "(world %.2f %.2f %.2f m, cone near %.2f m; the viewer does not move)",
                         control.scale > 0.0f ? "R2" : "L2",
                         static_cast<double>(headset.world_scale()),
                         pivot_mode_name(pivot_mode),
                         static_cast<double>(headset.scale_pivot_distance()),
                         static_cast<double>(pivot[0]), static_cast<double>(pivot[1]),
                         static_cast<double>(pivot[2]),
                         static_cast<double>(headset.scale_cone_near()));
                    if (pivot_mode == splat::ScalePivotMode::maquette) {
                        line("viewer: vr maquette: table %.2f %.2f %.2f m (%.0f cm ahead, %.0f cm "
                             "below the eyes), content centre %.2f %.2f %.2f m, robust extent "
                             "%.2f m, maquette scale %.3fx (the 5-95 box is %.2f m wide there)",
                             static_cast<double>(headset.maquette_table()[0]),
                             static_cast<double>(headset.maquette_table()[1]),
                             static_cast<double>(headset.maquette_table()[2]),
                             static_cast<double>(splat::kMaquetteTableForwardMetres) * 100.0,
                             static_cast<double>(splat::kMaquetteTableBelowEyesMetres) * 100.0,
                             static_cast<double>(headset.maquette_content_centre()[0]),
                             static_cast<double>(headset.maquette_content_centre()[1]),
                             static_cast<double>(headset.maquette_content_centre()[2]),
                             static_cast<double>(headset.maquette_extent()),
                             static_cast<double>(headset.maquette_scale()),
                             static_cast<double>(headset.maquette_extent()) *
                                 static_cast<double>(headset.maquette_scale()));
                    }
                }
                headset.scale_scene(pad::scale_factor(
                                        control.scale, input_seconds,
                                        (now_loop - scale_gesture_started) / 1000.0),
                                    input_seconds);
                scale_hint_until = now_loop + kScaleHintMs;
                scale_marker_until = now_loop + kScaleMarkerMs;
                set_scale_text(headset.world_scale());
            } else if (scale_gesture) {
                scale_gesture = false;
                headset.end_scale_gesture();
                scale_marker_until = now_loop + kScaleMarkerMs;
                std::snprintf(log_event, sizeof log_event, "%s", "scale end");
                line("viewer: vr scale end: %.2fx (range %.2f-%.2f, the %s pivot kept its place, "
                     "the viewer moved 0.00 m)",
                     static_cast<double>(headset.world_scale()),
                     static_cast<double>(splat::kMinWorldScale),
                     static_cast<double>(splat::kMaxWorldScale), pivot_mode_name(pivot_mode));
            }

            // Sticks: quadratic curve, Settings' speed at full deflection, and
            // a 150 ms velocity ease in/out. The velocities are integrated with
            // the measured frame dt, so a 40 deg/s turn is 0.67 deg per 60 Hz
            // frame and the same rate at any other frame time.
            const pad::LocomotionEase::Step motion = locomotion.update(
                pad::move_rate(control.move_right, settings->move_speed),
                pad::move_rate(control.move_forward, settings->move_speed),
                pad::move_rate(control.move_up, settings->move_speed),
                pad::turn_rate(control.turn, settings->turn_speed), input_seconds);
            const bool moving = locomotion.moving();
            if (moving) {
                // Viewer motions: forward walks into the scene, right steps
                // right, up rises and right turns the viewer to their right.
                headset.move_viewer(motion.right * static_cast<float>(input_seconds),
                                    motion.forward * static_cast<float>(input_seconds),
                                    motion.up * static_cast<float>(input_seconds));
                headset.turn_viewer(motion.turn * static_cast<float>(input_seconds));
                if (!move_gesture) {
                    move_gesture = true;
                    std::snprintf(log_event, sizeof log_event, "%s", "move start");
                    line("viewer: vr move start (left stick %.2f/%.2f, right stick %.2f/%.2f; "
                         "speed %.2f m/s, turn %.0f deg/s)",
                         static_cast<double>(control.move_right),
                         static_cast<double>(control.move_forward),
                         static_cast<double>(control.move_up), static_cast<double>(control.turn),
                         static_cast<double>(settings->move_speed),
                         static_cast<double>(settings->turn_speed));
                }
            } else if (move_gesture) {
                move_gesture = false;
                std::snprintf(log_event, sizeof log_event, "%s", "move end");
                line("viewer: vr move end: offset %.2f %.2f %.2f m, turn %.1f deg (the scene "
                     "moved so the viewer could)",
                     static_cast<double>(headset.scene_offset()[0]),
                     static_cast<double>(headset.scene_offset()[1]),
                     static_cast<double>(headset.scene_offset()[2]),
                     headset.scene_turn() * kRadToDeg);
            }

            if ((pressed & (pad::kPrevScene | pad::kNextScene)) != 0) {
                step_scene((pressed & pad::kNextScene) ? "R1" : "L1",
                           (pressed & pad::kNextScene) != 0);
            }
            service_preload();
        } else if (placed) {
            // The panel is up (W16): the scene's own controls are suspended, so a
            // gesture in progress is ended here instead of resuming behind the
            // panel when it closes. The preload keeps going: it is not an input.
            locomotion.reset();
            recentre_hold.update(false, (now_loop - session_started) / 1000.0);
            if (scale_gesture) {
                scale_gesture = false;
                headset.end_scale_gesture();
                line("viewer: vr scale end: %.2fx (the menu suspended the scene controls)",
                     static_cast<double>(headset.world_scale()));
            }
            if (move_gesture) {
                move_gesture = false;
                line("viewer: vr move end: offset %.2f %.2f %.2f m, turn %.1f deg (the menu "
                     "suspended the scene controls)",
                     static_cast<double>(headset.scene_offset()[0]),
                     static_cast<double>(headset.scene_offset()[1]),
                     static_cast<double>(headset.scene_offset()[2]),
                     headset.scene_turn() * kRadToDeg);
            }
            if (grab_gesture) {
                // The grip is still down, but the menu owns the input now: the
                // gesture ends here and keeps the scene where it is, instead of
                // resuming behind the panel when it closes. The grab's scaling
                // (if any) goes with it: no scale gesture outlives the grab.
                grab_gesture = false;
                grab_state.reset();
                headset.end_scale_gesture();
                line("viewer: grab end: scale=%.2fx (the menu suspended the scene controls)",
                     static_cast<double>(headset.world_scale()));
            }
            service_preload();
        } else {
            // Not placed yet: a Cross held through the wait is not a hold.
            locomotion.reset();
            recentre_hold.update(false, (now_loop - session_started) / 1000.0);
        }

        if (!hmd2::wait_slot()) {
            reason = "FAIL hmd2 stopped";
            status = 8;
            break;
        }
        if (!hmd2::begin_frame()) {
            if (++begin_failures > 120) {
                line("viewer: FAIL 120 frames without BeginFrame (0x%08x)",
                     static_cast<unsigned>(hmd2::error().rc));
                reason = "FAIL no BeginFrame";
                status = 8;
                break;
            }
            continue;
        }
        begin_failures = 0;
        const hmd2::EyeBuffer eye = hmd2::acquire();
        const hmd2::Poses poses = hmd2::predicted_poses();
        const splat::HeadPoses tracked = to_head_poses(poses);
        // The pose this frame is rendered from - and the one hmd2::submit()
        // hands Hmd2 as the render pose (same sample, same display time).
        last_raw = tracked;

        if (!placed) {
            // Still waiting for the headset. The eye buffer is untouched (zero)
            // and the flip keeps Hmd2's queue and the predicted pose moving.
            const double waited = (now_ms() - wait_started) / 1000.0;
            const bool worn = hmd2::worn();
            const splat::PlacementWait::Step settle = placement_wait.update(
                worn, poses.head.tracked, tracked.head, (now_ms() - session_started) / 1000.0);
            if (!settle.place) {
                if (!hmd2::submit() && ++wait_submit_errors > 5) {
                    line("viewer: FAIL %llu frames Hmd2 would not take while waiting",
                         static_cast<unsigned long long>(wait_submit_errors));
                    status = 8;
                    reason = "FAIL flips refused";
                    break;
                }
                if (settle.restarted) {
                    // A tracker re-anchor (a new local frame) or the headset
                    // moved in the hand: the pose the scene would have been
                    // anchored to is not the one the user is at any more.
                    line("viewer: vr first placement: the tracker's pose jumped; waiting another "
                         "%.1f s of a held pose",
                         splat::kPlaceStableSeconds);
                }
                counters.wait_frames++;
                if (waited >= next_wait_log) {
                    next_wait_log = waited + kPoseWaitLogSeconds;
                    line("viewer: vr waiting for the headset: worn=%d tracked=%d held=%.1f s "
                         "after %.0f s (up to %.0f s; %.1f s of a held pose places the scene)",
                         worn ? 1 : 0, poses.head.tracked ? 1 : 0, settle.stable_seconds, waited,
                         kPoseWaitLimitSeconds, splat::kPlaceStableSeconds);
                }
                continue;
            }
            // Worn, tracked and held for kPlaceStableSeconds: place the scene
            // at this pose, through the recentre path the Cross hold uses. The
            // 2026-10-27 runs placed on the first frame and the tracker's SLAM
            // reset moved the head ~90 deg in the new frame right after, which
            // left the photo to the user's right until they recentred.
            headset.update(tracked);
            const double removed_yaw = place_scene_at_head();
            placed = true;
            placed_at = now_ms();
            second_started = placed_at;
            // The mirror's rate counts from here: the waiting frames were
            // submitted black ones, and the first second's line must not
            // report them as the mirror's rate.
            mirror_shown = hmd2::flip_status().shown;
            line("viewer: vr headset worn and tracked after %.1f s (%llu waiting frames): the pose "
                 "held %.1f s, so the scene is placed at the head (capture camera at the eyes, "
                 "looking forward - the recentre path, %.1f deg of yaw removed, world scale %.2f), "
                 "ipd %.1f mm %s",
                 waited, static_cast<unsigned long long>(counters.wait_frames),
                 settle.stable_seconds, removed_yaw, static_cast<double>(headset.world_scale()),
                 headset.ipd() * 1000.0,
                 poses.left.tracked && poses.right.tracked ? "from the tracker's eye poses"
                                                           : "from the 63 mm default");
        }

        if (poses.head.tracked) {
            headset.update(tracked);
            if (run.tracking_lost) {
                run.tracking_lost = false;
                line("viewer: vr tracking regained");
            }
        } else if (!run.tracking_lost) {
            // Hold the last good pose: a lost tracker must not teleport the user
            // to the origin. Hmd2 reports the pose it could not track.
            run.tracking_lost = true;
            line("viewer: vr tracking lost; holding the last pose");
        }

        const splat::FrameView view = headset.frame();
        last_fade = view.comfort_fade;

        // ---- the panel (W16, Sense pointer W25) -----------------------------
        // The host draws its canvas into the renderer's RGBA overlay texture
        // (only when it changed), and its quads are appended to the overlay below.
        // The reference pose is latched on the frame the panel appears, so the
        // quad is world-locked where the user opened it (UiHost, Mode::Vr:
        // vr_panel_quad, yaw only, 1.6 m ahead).
        //
        // The right Sense controller is the panel's laser: R2 past
        // kPointerClickTrigger is a click on the pixel the ray hits. The trigger
        // edge is tracked every frame, so a press held while the panel opens is
        // not a click on its first frame; no controller pose (or no ray hit)
        // leaves pointer_valid false, and the d-pad/stick path is untouched.
        const bool pointer_trigger = sense_right.axes().r2 > kPointerClickTrigger;
        const bool pointer_click_edge = pointer_trigger && !pointer_r2_down;
        pointer_r2_down = pointer_trigger;
        if (ui_runtime != nullptr) {
            viewer::UiHost& ui_host = *ui_runtime->host;
            if (ui_host.visible() != route.panel_visible) ui_host.set_visible(route.panel_visible);
            // Advance the lazy follower once, before the pointer ray and the
            // drawn quads, so both use the same panel this frame.
            ui_host.update_panel(view.midpoint, input_seconds);
            splat::UiInput input;
            input.pressed = route.ui_pressed;
            input.held = route.ui_held;
            input.dt = input_seconds;

            if (ui_host.visible()) {
                // VrTracker2 registration is per pad handle (the same handle
                // sceVrTracker2GetResult takes). The right controller is device
                // id 1 (HLA-PSVR2 psvr2.c:469). UNVERIFIED on hardware: the
                // runbook's morning check is what proves the tracked ray.
                if (!pointer_registered && sense_right.handle() >= 0) {
                    pointer_registered =
                        hmd2::register_controller(sense_right.handle(), true);
                    if (pointer_registered) {
                        line("viewer: pointer: right Sense pad %d registered with VrTracker2 "
                             "(device 1); the tracked ray is UNVERIFIED on hardware",
                             sense_right.handle());
                    } else if (!pointer_register_failed_logged) {
                        pointer_register_failed_logged = true;
                        line("viewer: pointer: right Sense tracker registration failed (pad %d); "
                             "the ray stays off, d-pad and stick still work",
                             sense_right.handle());
                    }
                }

                std::optional<splat::PanelHit> hit;
                float controller_pos[3] = {0.0f, 0.0f, 0.0f};
                float controller_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (pointer_registered && last_raw.head.tracked &&
                    hmd2::controller_pose(sense_right.handle(), controller_pos, controller_quat)) {
                    const splat::PointerRay ray = splat::controller_ray(
                        last_raw.head.position, last_raw.head.quaternion, controller_pos,
                        controller_quat, view.midpoint);
                    const splat::OverlayQuad panel = ui_host.panel_quad(view.midpoint);
                    hit = splat::ray_panel_hit(ray.origin, ray.direction, panel,
                                               ui_host.canvas_width(), ui_host.canvas_height());
                }
                const std::optional<splat::PanelHit> filtered =
                    pointer_filter.update(hit, input_seconds);
                if (filtered) {
                    input.pointer_valid = true;
                    input.px = filtered->x;
                    input.py = filtered->y;
                }
                input.click = pointer_click_edge && input.pointer_valid;
                if (filtered && now_loop >= next_pointer_log) {
                    next_pointer_log = now_loop + kPointerLogMs;
                    line("viewer: pointer hit %.0f %.0f", static_cast<double>(filtered->x),
                         static_cast<double>(filtered->y));
                }
            } else {
                pointer_filter.reset();
            }
            ui_host.frame(input, view.midpoint);

            while (auto command = ui_runtime->app->take_command()) {
                switch (command->kind) {
                    case splat::UiCommand::ViewScene: {
                        line("viewer: ui command view_scene");
                        // W19: the model-load phase is the one window a scene
                        // load waits for.
                        if (scene_loads_paused()) {
                            ui_runtime->app->toast("Converting... scene switch in a moment");
                            line("viewer: vr jobs: ViewScene waits for the model load (album %s, "
                                 "index %d)",
                                 command->album.empty() ? "(loose)" : command->album.c_str(),
                                 command->index);
                            break;
                        }
                        // The album's scenes in listing order (the grid's focus
                        // index is the playlist index), through the same gallery
                        // the screen listed.
                        auto entries = splat::playlist_entries(
                            splat::list_scenes(ui_runtime->services.paths.scenes,
                                               command->album));
                        auto built = splat::ScenePlaylist::from_entries(std::move(entries));
                        if (!built.ok()) {
                            ui_runtime->app->toast("No scenes in this album");
                            line("viewer: vr view scene: album %s has no scenes",
                                 command->album.empty() ? "(loose)" : command->album.c_str());
                            break;
                        }
                        splat::ScenePlaylist album = built.take();
                        const int last = static_cast<int>(album.size()) - 1;
                        album.set_index(
                            static_cast<std::size_t>(std::clamp(command->index, 0, last)));
                        // The renderer's only load path (scene_switch.h): stage the
                        // album's scene into the one-deep slot and commit it, so
                        // Hmd2 keeps reprojecting the last submitted frame while
                        // the file is read.
                        VrSwitchSlot probe(*renderer, album);
                        probe.on_load = [&](const splat::LoadedScene& next) {
                            note_intrinsics(next);
                        };
                        // The old album's forward preload is not this scene: drop
                        // it (and its index) before the probe takes the slot.
                        switch_slot.reset();
                        const double started = now_ms();
                        const viewer::SwitchReport report =
                            viewer::switch_scene(probe, album.index());
                        if (!report.switched) {
                            ui_runtime->app->toast("Cannot load the scene");
                            line("viewer: FAIL view scene %d of album %s", command->index,
                                 command->album.empty() ? "(loose)" : command->album.c_str());
                            break;
                        }
                        const std::size_t index = album.index();
                        // L1/R1 now step this album, and the switch slot starts
                        // from scratch on it (its indices named the old playlist).
                        scenes = std::move(album);
                        switch_slot.reset();
                        apply_switch(index, probe, report, started);
                        ui_runtime->app->set_scene_loaded(true);
                        line("viewer: vr scene %zu/%zu %s (album %s)", index + 1, scenes.size(),
                             current_folder().c_str(),
                             command->album.empty() ? "(loose)" : command->album.c_str());
                        panel.close();
                        ui_host.set_visible(false);
                        line("viewer: ui close");
                        break;
                    }
                    case splat::UiCommand::ResumeScene:
                        // Options in the panel: back to the scene that is still
                        // loaded, with the menu where the user left it.
                        line("viewer: ui command resume_scene");
                        panel.close();
                        ui_host.set_visible(false);
                        line("viewer: ui close");
                        break;
                    case splat::UiCommand::Quit:
                        // The one quit route a title has: LoadExec after this
                        // loop's teardown, as SECONDS and the quit file do.
                        line("viewer: ui command quit");
                        log_pose("quit");
                        panel_quit = true;
                        break;
                }
                if (panel_quit) break;
            }
            if (panel_quit) break;
        }

        // The overlay: scene label moved with the scene's scale/offset, plus the
        // head-locked HUD (hold bar, scale readout) while it is up, plus the panel
        // when it is (update_overlay appends UiHost::quads()).
        // The scale press's peripheral vignette eases in and out with the press
        // (the dolly term is optic flow; a faint tunnel makes it easier to read).
        const float vignette_target =
            (scale_gesture || now_loop < scale_marker_until) ? kScaleVignetteStrength : 0.0f;
        const float vignette_alpha =
            1.0f - static_cast<float>(std::exp(-input_seconds / 0.12));
        scale_vignette += (vignette_target - scale_vignette) * vignette_alpha;
        renderer->set_scale_vignette(scale_vignette);
        update_overlay(view, now_loop);
        // Display > Blending chooses the attachment: sRGB (linear-light blend,
        // the default) or its UNORM counterpart (gamma-space blend like Spark).
        splat::RenderTarget& frame_target =
            (alt_target_ready && !settings->blend_linear) ? alt_target : target;
        const double frame_started = now_ms();
        const splat::Status rendered = renderer->render(view, frame_target, false);
        if (!rendered.ok()) {
            line("viewer: FAIL render: %s", rendered.message().c_str());
            run.gpu_ok = false;
            status = 7;
            reason = "FAIL render";
            break;
        }
        frame_target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const splat::PassTimings& timings = renderer->timings();
        project_second += timings.project_ms;
        sort_second += timings.sort_ms;
        draw_second += timings.draw_ms;
        counters.project_sum += timings.project_ms;
        counters.sort_sum += timings.sort_ms;
        counters.draw_sum += timings.draw_ms;

        // The GPU copy into the eye buffer Hmd2 will scan out, then the flip.
        // Bounded: a timeout means a wedged GPU and nothing may be freed.
        const double copy_started = now_ms();
        if (!copy_pass.copy(frame_target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            eye_buffers.buffer(eye.index), extent, kCopyTimeoutNs, error)) {
            line("viewer: FAIL eye buffer copy: %s", error.c_str());
            run.gpu_ok = false;
            status = 8;
            reason = "FAIL eye buffer copy";
            break;
        }
        const double copy_ms = now_ms() - copy_started;
        counters.copy_sum += copy_ms;
        counters.copy_second += copy_ms;
        if (!hmd2::submit()) counters.submit_errors++;
        const double work_ms = now_ms() - frame_started;
        counters.work_max = std::max(counters.work_max, work_ms);
        if (work_ms > kFramePeriodMs) counters.slow_frames++;
        counters.frames++;
        counters.frames_this_second++;
        // The row for this submitted frame, with the event that happened before
        // it (recentre, scene switch) when there was one.
        log_pose(log_event);
        if (counters.frames == 1) {
            line("viewer: vr first frame in %.1f ms from the session start (%u visible splats, "
                 "eye buffer %d, %.1f ms of work)",
                 now_ms() - session_started, renderer->last_visible_count(), eye.index, work_ms);
        }

        const double now = now_ms();
        if (now - second_started >= 1000.0) {
            const double seconds = (now - second_started) / 1000.0;
            const uint64_t frames = counters.frames_this_second;
            const hmd2::FlipInfo flip = hmd2::flip_status();
            const hmd2::PaceStats pace = hmd2::take_pace();
            // hmd2 counts the display slots it skipped: the miss count that
            // matters for "reprojected on time".
            const int missed_now = hmd2::missed_frames();
            // The mirror's rate: the images the display showed this second (each
            // is a fresh mirror frame too). Off and refused both print a word,
            // not a number, so a 0 can never be mistaken for a mirror.
            const double mirror_fps =
                mirror_ok ? static_cast<double>(flip.shown - mirror_shown) / seconds : 0.0;
            mirror_shown = flip.shown;
            char mirror_text[24];
            if (mirror_ok) {
                std::snprintf(mirror_text, sizeof mirror_text, "%.1ffps", mirror_fps);
            } else {
                std::snprintf(mirror_text, sizeof mirror_text, "%s",
                              options.mirror ? "failed" : "off");
            }
            line("viewer: vr t=%.0fs fps=%.1f project=%.2f sort=%.2f draw=%.2f (per eye %.2f) "
                 "copy=%.2f gpu=%.2f ms visible=%u fade=%.3f scale=%.2fx ipd=%.1f mm "
                 "slow=%llu/%llu missed=%d shown=%llu repeated=%d queue-full=%d err=%d worn=%d mirror=%s",
                 (now - placed_at) / 1000.0, frames / seconds,
                 frames ? project_second / frames : 0.0, frames ? sort_second / frames : 0.0,
                 frames ? draw_second / frames : 0.0, frames ? draw_second / frames / 2.0 : 0.0,
                 frames ? counters.copy_second / frames : 0.0,
                 frames ? (project_second + sort_second + draw_second) / frames : 0.0,
                 renderer->last_visible_count(), last_fade,
                 static_cast<double>(headset.world_scale()), headset.ipd() * 1000.0,
                 static_cast<unsigned long long>(counters.slow_frames),
                 static_cast<unsigned long long>(counters.frames), missed_now,
                 static_cast<unsigned long long>(flip.shown), hmd2::repeated_frames(),
                 hmd2::queue_full_waits(), hmd2::frame_errors(),
                 hmd2::device_state().worn ? 1 : 0, mirror_text);
            if (mirror_ok && !mirror_logged) {
                // Logged once, when there is a measurement to give: the mirror
                // costs the loop nothing itself, so this is the display's rate.
                mirror_logged = true;
                line("viewer: vr mirror: %.1f fps achieved over the first %.1f s (Hmd2's compositor, "
                     "next to the frames this loop already submits)",
                     mirror_fps, seconds);
            }
            if (pace.step_ms > 0.0f && missed_now > 0) {
                // One line of context, only when the display is skipping slots:
                // the pace numbers say whether the loop or the display is behind.
                line("viewer: vr pace nap=%.2f call=%.2f ahead=%.2f step=%.2f ms", pace.nap_ms,
                     pace.call_ms, pace.ahead_ms, pace.step_ms);
            }
            // The compact pose line for a recorded video's one-second grid, and
            // the CSV's flush: the raw Hmd2 position in cm (tracker axes: +x
            // right, +y up, forward -z) and the angles the camera used in the
            // scene's frame, in degrees - the same values as the CSV's row.
            line("viewer: vr pose pos %.1f %.1f %.1f cm yaw %.1f pitch %.1f roll %.1f deg "
                 "frame=%llu",
                 static_cast<double>(last_raw.head.position[0]) * 100.0,
                 static_cast<double>(last_raw.head.position[1]) * 100.0,
                 static_cast<double>(last_raw.head.position[2]) * 100.0,
                 headset.view_yaw() * kRadToDeg, headset.scene_pitch() * kRadToDeg,
                 headset.scene_roll() * kRadToDeg,
                 static_cast<unsigned long long>(counters.frames));
            pose_log.flush();
            counters.frames_this_second = 0;
            counters.copy_second = 0.0;
            project_second = 0.0;
            sort_second = 0.0;
            draw_second = 0.0;
            second_started = now;
        }
    }

    const double run_s = (now_ms() - placed_at) / 1000.0;
    const hmd2::FlipInfo flip = hmd2::flip_status();
    const int final_errors = hmd2::frame_errors();
    const int final_missed = hmd2::missed_frames();
    const int final_repeated = hmd2::repeated_frames();
    const int final_queue_full = hmd2::queue_full_waits();
    const int final_submitted = hmd2::frames_submitted();
    if (counters.frames > 0) {
        line("viewer: vr totals: %llu frames in %.1f s (%.1f fps), GPU %.2f ms/frame "
             "(project %.2f sort %.2f draw %.2f, per eye %.2f), copy %.2f ms/frame, "
             "slow %llu/%llu, submitted=%d shown=%llu missed=%d repeated=%d queue-full=%d err=%d",
             static_cast<unsigned long long>(counters.frames), run_s,
             counters.frames / (run_s > 0.0 ? run_s : 1.0),
             counters.draw_sum / counters.frames + counters.project_sum / counters.frames +
                 counters.sort_sum / counters.frames,
             counters.project_sum / counters.frames, counters.sort_sum / counters.frames,
             counters.draw_sum / counters.frames, counters.draw_sum / counters.frames / 2.0,
             counters.copy_sum / counters.frames,
             static_cast<unsigned long long>(counters.slow_frames),
             static_cast<unsigned long long>(counters.frames), final_submitted,
             static_cast<unsigned long long>(flip.shown), final_missed, final_repeated,
             final_queue_full, final_errors);
    } else {
        line("viewer: vr totals: no frame was submitted");
    }

    // W19: a conversion still running at exit owns the GPU; the panel runtime
    // (and with it the JobService destructor, which joins the worker) is left to
    // the process exit instead of being destroyed under it.
    if (ui_runtime != nullptr && ui_runtime->jobs != nullptr &&
        (viewer::jobs_active(*ui_runtime->services.jobs) || ui_runtime->services.jobs->busy())) {
        line("viewer: vr jobs: a conversion is still running at exit; leaving its worker and the "
             "model to the process exit");
        (void)ui_runtime.release();
    }

    teardown();

    // PASS is the app's own verdict: it rendered, Hmd2 took every frame, and no
    // display slot was skipped for long enough to matter. The picture itself is
    // the human check, as in vr_hello.
    const bool pass = status == 0 && run.gpu_ok && counters.frames > 120 &&
                      final_errors == 0 && counters.submit_errors == 0;
    if (!pass) {
        if (status == 0) {
            status = 1;
            reason = counters.frames <= 120 ? "too few frames (under 2 s of VR)"
                                            : "Hmd2 reported frame errors";
        }
        line("viewer: vr FAIL %llu frames, %.1f fps, gpu_ok=%d err=%d submit-errors=%llu %s",
             static_cast<unsigned long long>(counters.frames), counters.frames / (run_s > 0.0 ? run_s : 1.0),
             run.gpu_ok ? 1 : 0, final_errors,
             static_cast<unsigned long long>(counters.submit_errors), reason);
        return status;
    }
    line("viewer: vr PASS %llu frames, %.1f fps, %d submitted, shown %llu, missed %d, worst "
         "frame %.1f ms",
         static_cast<unsigned long long>(counters.frames),
         counters.frames / (run_s > 0.0 ? run_s : 1.0), final_submitted,
         static_cast<unsigned long long>(flip.shown), final_missed, counters.work_max);
    return 0;
}

}  // namespace viewer
