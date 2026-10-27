// vr_hello - PSVR2 test pattern through Hmd2's VideoOut buffers (C7, Spike B).
//
// What it proves: a plain payload can load the PS VR2 system libraries, open
// VideoOut's VR port (bus 32), register side-by-side eye buffers as Hmd2
// layers, submit frames through Hmd2's reprojection at 120 Hz output, read the
// predicted head pose, notice the headset being taken off, and shut everything
// down cleanly. No RADV, no AGC: the eye buffers are CPU-filled direct memory
// (see platform/ps5/hmd2/).
//
// The pattern: a different background colour per eye, a 64-pixel grid, a
// crosshair and corner squares, plus a yellow bar in each eye that moves with
// head yaw - so a wrong pose pipeline, a wrong eye, or a wrong buffer is
// visible at a glance.
//
// Every line goes to stdout (elfldr streams it back) and to the kernel log
// (klogsrv / etaHEN forward it). One status line per second: HMD state, refresh
// rate, eye-buffer size, pose. Runs 30 s, or stops early when the headset comes
// off, then shuts Hmd2 down.
//
// Build: make -C platform/ps5/vr_hello
// Run:   platform/ps5/scripts/run.sh platform/ps5/vr_hello/vr_hello.elf 40
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/sysctl.h>

#include <ps5/klog.h>

#include "../hmd2/hmd2.h"

extern "C" uint64_t sceKernelGetProcessTime(void);

namespace {

constexpr uint64_t kRunSeconds = 30;
constexpr uint32_t kGridStep = 64;
constexpr float kYawFullScaleDeg = 45.0f;   // head yaw that moves the marker to the eye's edge

void
line(const char *fmt, ...) {
    char    msg[320];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
}

void
hmd2_line(const char *msg) {
    line("%s", msg);
}

// The static part of the picture, in the eye buffer's tiled layout: draw it
// once into a scratch image, then copy it into each frame's buffer.
struct Canvas {
    hmd2::EyeBuffer eye = {0, nullptr, 0, 0};
    size_t           bytes = 0;
    uint32_t         eye_w = 0;
};

uint32_t
eye_colour(int eye, uint8_t r, uint8_t g, uint8_t b) {
    // The two eyes get clearly different colours, so a swapped eye or a
    // single-eye picture is obvious.
    return eye == 0 ? hmd2::pixel(r, g, b) : hmd2::pixel(static_cast<uint8_t>(r * 0.6f),
                                                        static_cast<uint8_t>(b * 0.9f),
                                                        static_cast<uint8_t>(g * 0.8f));
}

bool
canvas_init(Canvas &canvas) {
    const uint32_t width = static_cast<uint32_t>(hmd2::eye_size()) * 2;
    const uint32_t height = static_cast<uint32_t>(hmd2::eye_height());
    canvas.bytes = hmd2::image_bytes();
    canvas.eye_w = width / 2;
    void *pixels = std::malloc(canvas.bytes);
    if (pixels == nullptr)
        return false;
    std::memset(pixels, 0, canvas.bytes);
    canvas.eye = {0, static_cast<uint8_t *>(pixels), width, height};

    // Background: every pixel slot gets the same value, so a 32-bit fill is a
    // valid tiled fill.
    uint32_t *words = reinterpret_cast<uint32_t *>(canvas.eye.pixels);
    for (size_t i = 0; i < canvas.bytes / 4; i++)
        words[i] = eye_colour(0, 0, 40, 16);

    for (int e = 0; e < 2; e++) {
        const uint32_t x0 = static_cast<uint32_t>(e) * canvas.eye_w;
        const uint32_t bg = eye_colour(e, 0, 40, 16);
        const uint32_t grid = eye_colour(e, 0, 105, 45);
        const uint32_t edge = eye_colour(e, 0, 190, 90);
        const uint32_t white = hmd2::pixel(230, 230, 230);

        hmd2::fill_rect(canvas.eye, x0, 0, canvas.eye_w, height, bg);
        for (uint32_t x = 0; x < canvas.eye_w; x += kGridStep)      // vertical grid
            hmd2::fill_rect(canvas.eye, x0 + x, 0, 2, height, grid);
        for (uint32_t y = 0; y < height; y += kGridStep)            // horizontal grid
            hmd2::fill_rect(canvas.eye, x0, y, canvas.eye_w, 2, grid);
        // Crosshair at the eye's centre, so each eye's principal point is visible.
        hmd2::fill_rect(canvas.eye, x0 + canvas.eye_w / 2 - 1, height / 2 - 40, 3, 80, white);
        hmd2::fill_rect(canvas.eye, x0 + canvas.eye_w / 2 - 40, height / 2 - 1, 80, 3, white);
        // Corner squares: orientation and overscan.
        const uint32_t s = 24, m = 16;
        hmd2::fill_rect(canvas.eye, x0 + m, m, s, s, white);
        hmd2::fill_rect(canvas.eye, x0 + canvas.eye_w - m - s, m, s, s, white);
        hmd2::fill_rect(canvas.eye, x0 + m, height - m - s, s, s, white);
        hmd2::fill_rect(canvas.eye, x0 + canvas.eye_w - m - s, height - m - s, s, s, white);
        // A band along the top edge names the eye's colour at a glance.
        hmd2::fill_rect(canvas.eye, x0, 0, canvas.eye_w, 4, edge);
    }
    return true;
}

// The marker that follows head yaw, drawn into this frame's buffer.
void
draw_marker(const hmd2::EyeBuffer &eye, uint32_t eye_w, float yaw_deg) {
    float fraction = 0.5f + yaw_deg / (2.0f * kYawFullScaleDeg);
    if (fraction < 0.0f)
        fraction = 0.0f;
    if (fraction > 1.0f)
        fraction = 1.0f;
    const uint32_t local = static_cast<uint32_t>(fraction * static_cast<float>(eye_w - 1));
    const uint32_t yellow = hmd2::pixel(255, 220, 0);
    for (int e = 0; e < 2; e++) {
        const uint32_t x0 = static_cast<uint32_t>(e) * eye_w;
        hmd2::fill_rect(eye, x0 + local, 0, 10, eye.height, yellow);
        hmd2::fill_rect(eye, x0 + local, 48, 48, 48, yellow);
    }
}

// Head yaw and pitch in degrees from the pose's rotation (forward = R*(0,0,-1)).
void
head_direction(const hmd2::Pose &pose, float *yaw_deg, float *pitch_deg) {
    const float x = pose.quat[0], y = pose.quat[1], z = pose.quat[2], w = pose.quat[3];
    const float fx = -2.0f * (x * z + y * w);
    const float fy = -2.0f * (y * z - x * w);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    *yaw_deg = std::atan2(fx, -fz) * 57.29578f;
    const float up = fy < -1.0f ? -1.0f : fy > 1.0f ? 1.0f : fy;
    *pitch_deg = std::asin(up) * 57.29578f;
}

void
log_firmware() {
    uint32_t sdk = 0;
    size_t   len = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &len, nullptr, 0) == 0 && sdk)
        line("vrhello: firmware %x.%02x (kern.sdk_version 0x%08x)", sdk >> 24,
             (sdk >> 16) & 0xff, sdk);
    else
        line("vrhello: firmware unknown (kern.sdk_version unavailable)");
}

}  // namespace

int
main() {
    line("vrhello: start (vr_hello.elf, ps5-splat-vr, spike B step 2)");
    log_firmware();

    hmd2::set_logger(hmd2_line);
    if (!hmd2::init({})) {
        const hmd2::Error error = hmd2::error();
        line("vrhello: init failed at %s (rc 0x%08x) - see the hmd2 lines above",
             error.step != nullptr && error.step[0] != '\0' ? error.step : "unknown",
             static_cast<unsigned>(error.rc));
        hmd2::shutdown();
        return 1;
    }

    const hmd2::Fov fov = hmd2::field_of_view();
    line("vrhello: eye buffer %dx%d x%d, B8G8R8A8 tiled 64 KiB R_X, %zu bytes each, fov "
         "out %.3f in %.3f top %.3f bottom %.3f",
         hmd2::eye_size() * 2, hmd2::eye_height(), hmd2::buffer_count(), hmd2::image_bytes(),
         static_cast<double>(fov.out), static_cast<double>(fov.in), static_cast<double>(fov.top),
         static_cast<double>(fov.bottom));

    Canvas canvas;
    if (!canvas_init(canvas)) {
        line("vrhello: no memory for the test pattern");
        hmd2::shutdown();
        return 1;
    }

    const uint64_t start = sceKernelGetProcessTime();
    uint64_t       next_log = start + 1000000;
    uint64_t       frames = 0, window_frames = 0, window_start = start;
    bool           removed = false;
    // Only a worn -> not-worn transition stops the run, so starting the
    // payload with the headset still on the table does not end it at once.
    bool           was_worn = hmd2::worn();
    int            failures = 0;

    for (;;) {
        const uint64_t now = sceKernelGetProcessTime();
        if (now - start >= kRunSeconds * 1000000)
            break;
        if (!hmd2::wait_slot())
            break;
        if (!hmd2::begin_frame()) {
            if (++failures > 120) {
                line("vrhello: 120 frames without a BeginFrame, giving up");
                break;
            }
            continue;
        }
        hmd2::EyeBuffer eye = hmd2::acquire();
        if (eye.pixels != nullptr) {
            std::memcpy(eye.pixels, canvas.eye.pixels, canvas.bytes);
            float yaw = 0.0f, pitch = 0.0f;
            // The explicit-time lookup: the pose Hmd2 predicts for the display
            // time of the frame begin_frame() opened. predicted_pose() alone
            // returns the same value cached by begin_frame().
            head_direction(hmd2::predicted_pose(hmd2::display_time_us()), &yaw, &pitch);
            draw_marker(eye, canvas.eye_w, yaw);
        }
        if (!hmd2::submit())
            failures++;

        frames++;
        window_frames++;
        const bool worn = hmd2::worn();
        if (was_worn && !worn) {
            removed = true;
            line("vrhello: headset taken off, stopping after %llu frames",
                 static_cast<unsigned long long>(frames));
            break;
        }
        was_worn = worn;

        if (now >= next_log) {
            const double seconds = static_cast<double>(now - window_start) / 1e6;
            const double fps = seconds > 0.0 ? static_cast<double>(window_frames) / seconds : 0.0;
            const hmd2::Pose pose = hmd2::predicted_pose();
            const hmd2::DeviceState device = hmd2::device_state();
            float yaw = 0.0f, pitch = 0.0f;
            head_direction(pose, &yaw, &pitch);
            line("vrhello: t=%llus frames=%llu %.1f fps state=%u worn=%d lens=%u mm eye=%dx%d "
                 "bufs=%d out=%dHz pose=(%.3f,%.3f,%.3f) yaw=%+.1f pitch=%+.1f tracked=%d "
                 "err=%d fw=%d",
                 static_cast<unsigned long long>((now - start) / 1000000),
                 static_cast<unsigned long long>(frames), fps, device.status, device.worn ? 1 : 0,
                 device.lens_mm, hmd2::eye_size() * 2, hmd2::eye_height(), hmd2::buffer_count(),
                 hmd2::refresh_hz(), static_cast<double>(pose.pos[0]),
                 static_cast<double>(pose.pos[1]), static_cast<double>(pose.pos[2]),
                 static_cast<double>(yaw), static_cast<double>(pitch), pose.tracked ? 1 : 0,
                 hmd2::frame_errors() + failures, hmd2::firmware_table());
            next_log += 1000000;
            if (next_log < now)
                next_log = now + 1000000;
            window_frames = 0;
            window_start = now;
        }
    }

    std::free(canvas.eye.pixels);
    hmd2::shutdown();
    line("vrhello: done, %llu frames in %llu s, %s", static_cast<unsigned long long>(frames),
         static_cast<unsigned long long>((sceKernelGetProcessTime() - start) / 1000000),
         removed ? "headset removed" : "30 s elapsed");
    return 0;
}
