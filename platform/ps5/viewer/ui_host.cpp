// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_host.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "render/splat_renderer.h"
#include "render/ui_panel.h"

#include "title_common.h"

namespace viewer {
namespace {

// The panel's plate colour (sRGB), opaque: a transparent canvas pixel shows the
// plate, not the 3D scene behind the panel.
constexpr uint8_t kBackdropSrgb[3] = {16, 18, 22};

// The Sense laser cursor: a 12 px white ring on the canvas, drawn after the
// screens' draw() so no screen has to know about the pointer.
constexpr int kCursorDiameter = 12;
constexpr int kCursorRadius = kCursorDiameter / 2;
constexpr int kCursorThickness = 2;
constexpr splat::Rgba kCursorColor = {255, 255, 255, 255};

// The renderer's world -> camera matrix for a CameraPose (splat_renderer.cpp:
// yaw about world y, then pitch, then roll; column-major OpenCV axes).
void view_from_pose(const splat::CameraPose& pose, float view[16]) {
    const float cy = std::cos(pose.yaw);
    const float sy = std::sin(pose.yaw);
    const float cp = std::cos(pose.pitch);
    const float sp = std::sin(pose.pitch);
    const float f[3] = {sy * cp, sp, cy * cp};
    float s[3] = {cy, 0.0f, -sy};
    float d[3] = {f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2],
                  f[0] * s[1] - f[1] * s[0]};
    const float cr = std::cos(pose.roll);
    const float sr = std::sin(pose.roll);
    const float level_s[3] = {s[0], s[1], s[2]};
    const float level_d[3] = {d[0], d[1], d[2]};
    for (int axis = 0; axis < 3; ++axis) {
        s[axis] = cr * level_s[axis] + sr * level_d[axis];
        d[axis] = cr * level_d[axis] - sr * level_s[axis];
    }
    const float* p = pose.position;
    view[0] = s[0];
    view[1] = d[0];
    view[2] = f[0];
    view[3] = 0.0f;
    view[4] = s[1];
    view[5] = d[1];
    view[6] = f[1];
    view[7] = 0.0f;
    view[8] = s[2];
    view[9] = d[2];
    view[10] = f[2];
    view[11] = 0.0f;
    view[12] = -(s[0] * p[0] + s[1] * p[1] + s[2] * p[2]);
    view[13] = -(d[0] * p[0] + d[1] * p[1] + d[2] * p[2]);
    view[14] = -(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
    view[15] = 1.0f;
}

// The TV overlay camera: the renderer's view matrix for this pose, its eye
// intrinsics and its mono eye size. Mode::Tv renders mono (main.cpp sets
// stereo = false), so the eye offset and the per-eye principal-point shift are
// zero, exactly as SplatRenderer builds its OverlayCamera.
splat::OverlayCamera tv_camera(const splat::CameraIntrinsics& intrinsics, uint32_t width,
                               uint32_t height, const splat::CameraPose& camera) {
    splat::OverlayCamera overlay;
    view_from_pose(camera, overlay.view);
    overlay.intrinsics = intrinsics;
    overlay.width = width;
    overlay.height = height;
    overlay.eye_offset = 0.0f;
    overlay.cx_shift = 0.0f;
    return overlay;
}

}  // namespace

UiHost::UiHost(splat::SplatRenderer& renderer, splat::IPanelContent& content, Mode mode)
    : renderer_(renderer),
      content_(content),
      mode_(mode),
      canvas_(kDefaultCanvasWidth, kDefaultCanvasHeight) {}

UiHost::~UiHost() = default;

bool UiHost::init(uint32_t width, uint32_t height,
                  const splat::CameraIntrinsics& eye_intrinsics) {
    if (initialised_) return true;
    if (width == 0 || height == 0) {
        last_error_ = "the panel canvas has no size";
        return false;
    }
    canvas_ = splat::UiCanvas(width, height);
    intrinsics_ = eye_intrinsics;

    // The backdrop: the panel's plate behind a transparent canvas pixel.
    const uint8_t backdrop[4] = {kBackdropSrgb[0], kBackdropSrgb[1], kBackdropSrgb[2], 255};
    auto backdrop_texture = renderer_.add_overlay_rgba_texture(1, 1, backdrop);
    if (!backdrop_texture.ok()) {
        last_error_ = backdrop_texture.message();
        return false;
    }
    backdrop_texture_ = backdrop_texture.value();
    backdrop_ready_ = true;

    // The panel: the canvas itself, uploaded whole again only when it changes.
    auto panel_texture = renderer_.add_overlay_rgba_texture(
        canvas_.width(), canvas_.height(), canvas_.pixels().data());
    if (!panel_texture.ok()) {
        last_error_ = panel_texture.message();
        return false;
    }
    panel_texture_ = panel_texture.value();

    initialised_ = true;
    line("viewer: ui host: %ux%u canvas, %s mode, one RGBA8 overlay texture",
         canvas_.width(), canvas_.height(), mode_ == Mode::Tv ? "tv" : "vr");
    return true;
}

void UiHost::set_visible(bool visible) {
    if (visible && !visible_) panel_reset_ = true;
    visible_ = visible;
    if (!visible_) quads_.clear();
}

bool UiHost::upload_canvas() {
    const size_t pixels =
        static_cast<size_t>(canvas_.width()) * static_cast<size_t>(canvas_.height());
    const uint8_t* source = canvas_.pixels().data();
    if (pixels == 0 || source == nullptr) return false;
    const splat::Status status = renderer_.update_overlay_rgba_texture(
        panel_texture_, canvas_.width(), canvas_.height(), source);
    if (!status.ok()) {
        last_error_ = status.message();
        return false;
    }
    ++uploads_;
    return true;
}

void UiHost::build_quads(const splat::CameraPose& camera) {
    quads_.clear();
    if (!initialised_ || !visible_ || !backdrop_ready_) return;

    const splat::OverlayQuad panel = panel_quad(camera);
    // A zero-size panel has no viewport to fill (and its geometry is
    // meaningless); drawing it would only cost a draw call.
    if (!(panel.width > 0.0f) || !(panel.height > 0.0f)) return;

    // Both quads share the panel geometry and a white, opaque tint: the
    // textures carry their own colour and alpha.
    splat::OverlayQuad backdrop = panel;
    backdrop.texture = backdrop_texture_;
    quads_.push_back(backdrop);

    splat::OverlayQuad quad = panel;
    quad.texture = panel_texture_;
    quads_.push_back(quad);
}

splat::OverlayQuad UiHost::panel_quad(const splat::CameraPose& camera) {
    if (!initialised_ || !visible_ || !backdrop_ready_) return splat::OverlayQuad{};

    splat::OverlayQuad panel;
    if (mode_ == Mode::Tv) {
        panel = splat::tv_panel_quad(
            tv_camera(intrinsics_, renderer_.eye_width(), renderer_.eye_height(), camera),
            canvas_.width(), canvas_.height());
    } else {
        if (panel_reset_ || !panel_follower_.initialised()) {
            panel_follower_.reset(camera.yaw, camera.pitch);
            panel_reset_ = false;
        }
        panel = panel_follower_.quad(camera.position, camera.yaw, camera.pitch);
    }
    if (!(panel.width > 0.0f) || !(panel.height > 0.0f)) return splat::OverlayQuad{};
    return panel;
}

void UiHost::update_panel(const splat::CameraPose& camera, double dt) {
    if (mode_ != Mode::Vr || !visible_) return;
    if (panel_reset_ || !panel_follower_.initialised()) {
        panel_follower_.reset(camera.yaw, camera.pitch);
        panel_reset_ = false;
        return;
    }
    panel_follower_.update(camera.yaw, camera.pitch, dt);
}

void UiHost::restore_cursor() {
    if (!cursor_saved_) return;
    const size_t row_bytes = static_cast<size_t>(canvas_.width()) * 4u;
    std::vector<uint8_t>& pixels = canvas_.pixels();
    for (int row = 0; row < cursor_patch_h_; ++row) {
        const size_t offset = (static_cast<size_t>(cursor_patch_y_ + row) * row_bytes) +
                              static_cast<size_t>(cursor_patch_x_) * 4u;
        std::memcpy(pixels.data() + offset,
                    cursor_patch_.data() + static_cast<size_t>(row) *
                                              static_cast<size_t>(cursor_patch_w_) * 4u,
                    static_cast<size_t>(cursor_patch_w_) * 4u);
    }
    cursor_saved_ = false;
}

bool UiHost::draw_cursor(const splat::UiInput& input, bool content_redrew) {
    const int width = static_cast<int>(canvas_.width());
    const int height = static_cast<int>(canvas_.height());
    const int cx = static_cast<int>(std::lround(input.px));
    const int cy = static_cast<int>(std::lround(input.py));
    const bool inside = input.pointer_valid && cx >= 0 && cy >= 0 && cx < width && cy < height;

    // `was_at` has to be read before the old ring is removed: with an unchanged
    // canvas and an unchanged position, restore + redraw is the same pixels and
    // costs no upload.
    const bool was_at = cursor_saved_ && inside && cx == cursor_x_ && cy == cursor_y_;
    const bool had_ring = cursor_saved_;
    bool changed = false;
    if (content_redrew) {
        // The screens cleared the canvas (UiApp::draw clears), so the saved
        // patch describes pixels that no longer exist.
        cursor_saved_ = false;
    } else if (cursor_saved_) {
        restore_cursor();
        // Restoring the same position the ring is about to be drawn at leaves
        // the canvas byte-identical: no upload.
        changed = !was_at;
    }
    if (!inside) return changed;

    // Save the pixels under the ring's bounding box, clipped to the canvas.
    const int x0 = std::max(0, cx - kCursorRadius);
    const int y0 = std::max(0, cy - kCursorRadius);
    const int x1 = std::min(width, cx + kCursorRadius + 1);
    const int y1 = std::min(height, cy + kCursorRadius + 1);
    cursor_patch_x_ = x0;
    cursor_patch_y_ = y0;
    cursor_patch_w_ = x1 - x0;
    cursor_patch_h_ = y1 - y0;
    const size_t row_bytes = static_cast<size_t>(canvas_.width()) * 4u;
    const std::vector<uint8_t>& pixels = canvas_.pixels();
    cursor_patch_.resize(static_cast<size_t>(cursor_patch_w_) *
                         static_cast<size_t>(cursor_patch_h_) * 4u);
    for (int row = 0; row < cursor_patch_h_; ++row) {
        const size_t offset = (static_cast<size_t>(y0 + row) * row_bytes) +
                              static_cast<size_t>(x0) * 4u;
        std::memcpy(cursor_patch_.data() + static_cast<size_t>(row) *
                                               static_cast<size_t>(cursor_patch_w_) * 4u,
                    pixels.data() + offset, static_cast<size_t>(cursor_patch_w_) * 4u);
    }

    // A 12 px circle outline: pixels whose squared distance is between the
    // inner and outer radius.
    const int inner = kCursorRadius - kCursorThickness;
    for (int dy = -kCursorRadius; dy <= kCursorRadius; ++dy) {
        for (int dx = -kCursorRadius; dx <= kCursorRadius; ++dx) {
            const int distance = dx * dx + dy * dy;
            if (distance <= kCursorRadius * kCursorRadius && distance > inner * inner)
                canvas_.fill_rect(cx + dx, cy + dy, 1, 1, kCursorColor);
        }
    }
    cursor_saved_ = true;
    cursor_x_ = cx;
    cursor_y_ = cy;
    return changed || !had_ring;
}

void UiHost::frame(const splat::UiInput& input, const splat::CameraPose& camera) {
    if (!initialised_) return;
    content_.update(input);
    const bool content_changed = content_.draw(canvas_);
    // The cursor is drawn after the screens, never by them, and its saved patch
    // keeps a stationary pointer from costing an upload.
    const bool cursor_changed = draw_cursor(input, content_changed);
    if (content_changed || cursor_changed) {
        if (!upload_canvas() && !last_error_.empty()) {
            line("viewer: ui upload: %s", last_error_.c_str());
            last_error_.clear();
        }
    }
    build_quads(camera);
}

}  // namespace viewer
