// UiHost: the only thing the TV and VR viewer paths talk to for the panel UI
// (docs/ui-v1-plan.md section 3). It owns the UiCanvas, uploads the drawn
// canvas through SplatRenderer's overlay textures when IPanelContent::draw()
// reports a change, and returns the overlay quads the caller merges into
// SplatRenderer::set_overlay_quads.
//
// The panel is ONE RGBA8 overlay texture: the canvas bytes, straight alpha,
// uploaded whole on a change (render/overlay.h, overlay_rgba.frag). A photo
// thumbnail or a UI photo preview therefore keeps every colour it was drawn
// with; there is no palette and no per-colour mask. An opaque 1x1 RGBA
// backdrop quad sits behind it so the panel reads over the scene.
//
// In VR the quad is the lazy follower of render/hmd_view.h (PanelFollower): it
// stays parallel to the view plane (yaw and pitch, never roll) and re-centres
// with a ~0.3 s ease when the head turns more than ~17 deg away, so it never
// keystones the way the old yaw-only world-locked plane did.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/camera.h"
#include "render/hmd_view.h"
#include "render/overlay_quad.h"
#include "ui/canvas.h"
#include "ui/ui_input.h"

namespace splat {
class IPanelContent;
class SplatRenderer;
}  // namespace splat

namespace viewer {

class UiHost {
public:
    enum class Mode { Tv, Vr };

    static constexpr uint32_t kDefaultCanvasWidth = 1280;
    static constexpr uint32_t kDefaultCanvasHeight = 720;

    UiHost(splat::SplatRenderer& renderer, splat::IPanelContent& content, Mode mode);
    ~UiHost();
    UiHost(const UiHost&) = delete;
    UiHost& operator=(const UiHost&) = delete;

    // Creates the canvas, the 1x1 backdrop texture and the canvas-sized panel
    // texture. `eye_intrinsics` is the renderer's RendererOptions::intrinsics
    // (TV: the panel fills exactly that view); an invalid value means the
    // renderer's 60 degree fallback, which tv_panel_quad() reproduces.
    bool init(uint32_t width = kDefaultCanvasWidth, uint32_t height = kDefaultCanvasHeight,
              const splat::CameraIntrinsics& eye_intrinsics = {});

    // One frame: update the content, upload when it changed, rebuild the quads.
    void frame(const splat::UiInput& input, const splat::CameraPose& camera);

    // VR only: advances the lazy panel follower with this frame's head pose and
    // dt. Call once per frame before panel_quad()/frame(), so the pointer ray,
    // the drawn quad and the follower state all agree. TV mode ignores it.
    void update_panel(const splat::CameraPose& camera, double dt);

    bool visible() const { return visible_; }
    // Showing the panel again (a menu reopening in VR) re-faces the reference.
    void set_visible(bool visible);

    // The panel's quad this frame, without the backdrop: the same geometry
    // quads() returns, for the pointer ray to test against before frame()
    // rebuilds the list. In VR it latches the world reference exactly as
    // frame() would (the first visible frame included), so the ray and the
    // drawn panel share one reference pose. Empty (width 0) when hidden or
    // before init().
    splat::OverlayQuad panel_quad(const splat::CameraPose& camera);

    uint32_t canvas_width() const { return canvas_.width(); }
    uint32_t canvas_height() const { return canvas_.height(); }

    // Backdrop first (behind), then the panel. Empty when hidden or before
    // init().
    std::vector<splat::OverlayQuad> quads() const { return quads_; }

    const std::string& last_error() const { return last_error_; }
    // Number of canvas texture uploads since init(); a test or a log line can
    // show that an idle panel really costs none.
    uint64_t uploads() const { return uploads_; }

private:
    bool upload_canvas();
    void build_quads(const splat::CameraPose& camera);
    // The Sense laser cursor: a 12 px ring drawn on the canvas after the
    // screens' draw(), saved and restored so a stationary cursor costs no
    // upload and a moving one does not smear. Returns true when it changed the
    // canvas (and the frame must upload).
    bool draw_cursor(const splat::UiInput& input, bool content_redrew);
    void restore_cursor();

    splat::SplatRenderer& renderer_;
    splat::IPanelContent& content_;
    Mode mode_;
    splat::UiCanvas canvas_;
    splat::CameraIntrinsics intrinsics_;
    uint32_t backdrop_texture_ = 0;
    uint32_t panel_texture_ = 0;
    bool backdrop_ready_ = false;
    bool initialised_ = false;
    bool visible_ = true;
    // VR: the panel's lazy follower (hmd_view.h). It starts ahead of the pose
    // that raised the panel and re-centres when the head turns away.
    bool panel_reset_ = true;
    splat::PanelFollower panel_follower_;
    std::vector<splat::OverlayQuad> quads_;
    // The cursor's saved pixels and position; `cursor_saved_` says the patch is
    // the canvas under a ring that is currently drawn.
    std::vector<uint8_t> cursor_patch_;
    int cursor_patch_x_ = 0;
    int cursor_patch_y_ = 0;
    int cursor_patch_w_ = 0;
    int cursor_patch_h_ = 0;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    bool cursor_saved_ = false;
    std::string last_error_;
    uint64_t uploads_ = 0;
};

}  // namespace viewer
