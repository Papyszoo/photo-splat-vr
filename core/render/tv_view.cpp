// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/tv_view.h"

#include <algorithm>
#include <cmath>

namespace splat {

TvCrop tv_crop(float cx, float cy, uint32_t eye_w, uint32_t eye_h, uint32_t out_w,
               uint32_t out_h) {
    TvCrop crop;
    if (eye_w == 0 || eye_h == 0 || out_w == 0 || out_h == 0) return crop;
    const double width = static_cast<double>(eye_w);
    const double height = static_cast<double>(eye_h);
    // An axis outside the image (a bad intrinsic) is pulled to its edge, which
    // leaves a zero half extent: fall back to the image centre then.
    double axis_x = std::isfinite(cx) ? std::clamp(static_cast<double>(cx), 0.0, width)
                                      : 0.5 * width;
    double axis_y = std::isfinite(cy) ? std::clamp(static_cast<double>(cy), 0.0, height)
                                      : 0.5 * height;
    double half_w = std::min(axis_x, width - axis_x);
    double half_h = std::min(axis_y, height - axis_y);
    if (!(half_w >= 1.0) || !(half_h >= 1.0)) {
        axis_x = 0.5 * width;
        axis_y = 0.5 * height;
        half_w = 0.5 * width;
        half_h = 0.5 * height;
    }
    const double aspect = static_cast<double>(out_w) / static_cast<double>(out_h);
    if (half_w / half_h > aspect) {
        half_w = half_h * aspect;
    } else {
        half_h = half_w / aspect;
    }
    const auto clamp_to = [](double value, double limit) {
        return static_cast<int32_t>(std::clamp(std::llround(value), 0ll,
                                               static_cast<long long>(limit)));
    };
    crop.x0 = clamp_to(axis_x - half_w, width);
    crop.x1 = clamp_to(axis_x + half_w, width);
    crop.y0 = clamp_to(axis_y - half_h, height);
    crop.y1 = clamp_to(axis_y + half_h, height);
    return crop;
}

#if defined(SPLAT_HAVE_VULKAN)

OverlayCamera tv_overlay_camera(uint32_t width, uint32_t height) {
    OverlayCamera camera;
    for (int i = 0; i < 16; ++i) camera.view[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // identity
    const float fx = 0.5f * static_cast<float>(width) /
                     std::tan(0.5f * 60.0f * 3.14159265358979323846f / 180.0f);
    camera.intrinsics = {fx, fx, 0.5f * static_cast<float>(width),
                         0.5f * static_cast<float>(height)};
    camera.width = width;
    camera.height = height;
    return camera;
}

OverlayQuad tv_screen_quad(const OverlayCamera& camera, float x, float y, float w, float h) {
    // The overlay shader maps a camera-space point (X, Y, Z) to pixels
    //   px = fx * X / Z + cx,  py = fy * Y / Z + cy   (eye offset and shift 0)
    // so a quad at Z = 1 with centre ((x + w/2 - cx) / fx, (y + h/2 - cy) / fy)
    // and size (w / fx, h / fy) covers exactly that rectangle. Camera +y is
    // down; the quad's `up` is therefore -y, the same as tv_panel_quad.
    constexpr float kDepth = 1.0f;
    const CameraIntrinsics& k = camera.intrinsics;
    OverlayQuad quad;
    quad.center[0] = (x + 0.5f * w - k.cx) * kDepth / k.fx;
    quad.center[1] = (y + 0.5f * h - k.cy) * kDepth / k.fy;
    quad.center[2] = kDepth;
    quad.right[0] = 1.0f;
    quad.right[1] = 0.0f;
    quad.right[2] = 0.0f;
    quad.up[0] = 0.0f;
    quad.up[1] = -1.0f;
    quad.up[2] = 0.0f;
    quad.width = w * kDepth / k.fx;
    quad.height = h * kDepth / k.fy;
    return quad;
}

#endif  // SPLAT_HAVE_VULKAN

}  // namespace splat
