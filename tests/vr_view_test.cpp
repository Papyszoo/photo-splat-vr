// VR2-fix: the viewer's real VR view code, exercised on this Mac.
//
// The headset run of 2026-10-26 placed the scene at the head and then showed
// almost nothing: 4k-15k visible splats and 180-245 ms of draw instead of
// 1.18 M splats and 2.6 ms, with the picture only appearing when the head was
// turned far from where it started. This test feeds HmdView - the same code
// vr_mode.cpp uses on the console - synthetic poses in Hmd2's tracker
// convention and renders the real scene (runs/sharp_e2e/immichvr-1) at the
// PSVR2 eye size, through core/render, so the view can be checked on the
// desktop:
//
//   (a) at the placement pose the frame is the desktop capture pose (identity
//       camera) in both eyes, PSNR >= 20 dB per eye, and at least 95 % of the
//       warm-up frame's visible splats;
//   (b) a head yaw of +30 deg puts the scene content left in both eyes, and a
//       head rise of 10 cm puts it down, measured by cross-correlation;
//   (c) draw time is in the normal range for this scene.
//
// PNGs go to $SPLAT_DATA_DIR/runs/vr2_fix/. The scene is external data, so the
// test skips when it is absent (AGENTS.md).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <stb/stb_image_write.h>

#include "common/image.h"
#include "data_dir.h"
#include "render/hmd_view.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/scene_folder.h"

#if defined(SPLAT_HAVE_VULKAN)
#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif
#endif

namespace {

// PSVR2's eye image and the tangents hmd2 reported (the headset run's log).
constexpr uint32_t kEyeWidth = 1984;
constexpr uint32_t kEyeHeight = 2048;
constexpr splat::HeadsetFov kPsvr2Fov{1.842f, 0.947f, 1.329f, 1.329f};

// The scene of the failing run: SHARP, 1.18 M splats, capture intrinsics in
// meta.json.
std::filesystem::path scene_folder() {
    return splat_test::data_path("runs/sharp_e2e/immichvr-1");
}

std::filesystem::path output_folder() {
    return splat_test::data_path("runs/vr2_fix");
}

// Head pose with the given camera yaw (right positive), pitch (down positive)
// and roll (the camera's right side going down, i.e. right ear down) in the
// scene convention, expressed the way Hmd2 reports it: metres, quaternion
// (x, y, z, w), +y up, forward -z. The head frame's basis in scene axes is the
// level camera's right/down/forward turned by `roll` about the optical axis, as
// the renderer does it (core/render/splat_renderer.cpp); the tracker columns are
// that basis through the scene mapping (x, -y, -z) - right, up (-down) and
// backward (-forward).
splat::TrackedPose make_head_pose(float x, float y, float z, double yaw, double pitch,
                                  double roll = 0.0) {
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cr = std::cos(roll), sr = std::sin(roll);
    const double f[3] = {sy * cp, sp, cy * cp};
    const double s0[3] = {cy, 0.0, -sy};
    const double d0[3] = {f[1] * s0[2] - f[2] * s0[1], f[2] * s0[0] - f[0] * s0[2],
                          f[0] * s0[1] - f[1] * s0[0]};
    double right[3], down[3], forward[3];
    for (int i = 0; i < 3; ++i) {
        right[i] = cr * s0[i] + sr * d0[i];
        down[i] = cr * d0[i] - sr * s0[i];
        forward[i] = f[i];
    }
    // Scene -> tracker is (x, -y, -z), for the basis vectors too.
    const auto to_tracker = [](const double v[3], double out[3]) {
        out[0] = v[0];
        out[1] = -v[1];
        out[2] = -v[2];
    };
    double tracker_right[3], tracker_up[3], tracker_backward[3];
    const double up[3] = {-down[0], -down[1], -down[2]};
    const double backward[3] = {-forward[0], -forward[1], -forward[2]};
    to_tracker(right, tracker_right);
    to_tracker(up, tracker_up);
    to_tracker(backward, tracker_backward);
    const double m[3][3] = {
        {tracker_right[0], tracker_up[0], tracker_backward[0]},
        {tracker_right[1], tracker_up[1], tracker_backward[1]},
        {tracker_right[2], tracker_up[2], tracker_backward[2]},
    };
    double w = 1.0, qx = 0.0, qy = 0.0, qz = 0.0;
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s;
        qx = (m[2][1] - m[1][2]) / s;
        qy = (m[0][2] - m[2][0]) / s;
        qz = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        w = (m[2][1] - m[1][2]) / s;
        qx = 0.25 * s;
        qy = (m[0][1] + m[1][0]) / s;
        qz = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        w = (m[0][2] - m[2][0]) / s;
        qx = (m[0][1] + m[1][0]) / s;
        qy = 0.25 * s;
        qz = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        w = (m[1][0] - m[0][1]) / s;
        qx = (m[0][2] + m[2][0]) / s;
        qy = (m[1][2] + m[2][1]) / s;
        qz = 0.25 * s;
    }
    splat::TrackedPose pose;
    pose.position[0] = x;
    pose.position[1] = y;
    pose.position[2] = z;
    pose.quaternion[0] = static_cast<float>(qx);
    pose.quaternion[1] = static_cast<float>(qy);
    pose.quaternion[2] = static_cast<float>(qz);
    pose.quaternion[3] = static_cast<float>(w);
    pose.tracked = true;
    return pose;
}

// The two eye poses of a head pose: +-IPD/2 along the head's right axis.
splat::HeadPoses head_with_eyes(const splat::TrackedPose& head, float ipd) {
    splat::HeadPoses poses;
    poses.head = head;
    // Right axis of the head pose, rotated from its quaternion: the first
    // column of the rotation matrix.
    const float x = head.quaternion[0], y = head.quaternion[1], z = head.quaternion[2],
                w = head.quaternion[3];
    const float right[3] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z),
                            2.0f * (x * z - w * y)};
    for (int side = 0; side < 2; ++side) {
        const float sign = side == 0 ? -0.5f : 0.5f;
        splat::TrackedPose eye = head;
        for (int axis = 0; axis < 3; ++axis) {
            eye.position[axis] = head.position[axis] + sign * ipd * right[axis];
        }
        (side == 0 ? poses.left : poses.right) = eye;
    }
    return poses;
}

// The head's own axes (right ear, down, forward) in scene axes, from the tracker
// quaternion: the rotation's columns are the head's x, y (up) and z (backward)
// axes in tracker space, mapped to the scene's y-down axes by (x, -y, -z).
// Written from the quaternion directly, so it is an independent check of
// hmd_basis and of anything built on it.
void head_axes(const splat::TrackedPose& pose, double right[3], double down[3],
               double forward[3]) {
    const double x = pose.quaternion[0], y = pose.quaternion[1], z = pose.quaternion[2],
                 w = pose.quaternion[3];
    const double tracker_right[3] = {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z),
                                     2.0 * (x * z - w * y)};
    const double tracker_up[3] = {2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z),
                                  2.0 * (y * z + w * x)};
    const double tracker_backward[3] = {2.0 * (x * z + w * y), 2.0 * (y * z - w * x),
                                        1.0 - 2.0 * (x * x + y * y)};
    for (int i = 0; i < 3; ++i) {
        const int sign = i == 0 ? 1 : -1;
        right[i] = sign * tracker_right[i];
        down[i] = -sign * tracker_up[i];
        forward[i] = -sign * tracker_backward[i];
    }
}

// The camera axes the renderer builds from a FrameView's yaw/pitch/roll, in scene
// axes (core/render/splat_renderer.cpp): yaw about scene y, pitch about the
// yawed x, then roll about the optical axis.
void camera_axes(const splat::CameraPose& pose, double right[3], double down[3],
                 double forward[3]) {
    const double cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
    const double cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const double cr = std::cos(pose.roll), sr = std::sin(pose.roll);
    forward[0] = sy * cp;
    forward[1] = sp;
    forward[2] = cy * cp;
    const double s0[3] = {cy, 0.0, -sy};
    const double d0[3] = {forward[1] * s0[2] - forward[2] * s0[1],
                          forward[2] * s0[0] - forward[0] * s0[2],
                          forward[0] * s0[1] - forward[1] * s0[0]};
    for (int i = 0; i < 3; ++i) {
        right[i] = cr * s0[i] + sr * d0[i];
        down[i] = cr * d0[i] - sr * s0[i];
    }
}

double dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// doctest 2.4's Approx has no absolute margin; these comparisons include zero.
bool near(double value, double expected, double tolerance = 1e-6) {
    return std::abs(value - expected) <= tolerance;
}

bool write_png(const std::filesystem::path& path, const splat::Image& image) {
    if (image.empty()) return false;
    return stbi_write_png(path.string().c_str(), image.width, image.height, 3,
                          image.rgb.data(), image.width * 3) != 0;
}

splat::Image eye_crop(const splat::Image& side_by_side, uint32_t eye) {
    splat::Image out;
    out.resize(static_cast<int>(kEyeWidth), static_cast<int>(kEyeHeight));
    const int x0 = static_cast<int>(eye * kEyeWidth);
    for (uint32_t row = 0; row < kEyeHeight; ++row) {
        const uint8_t* source = side_by_side.pixel(x0, static_cast<int>(row));
        std::copy(source, source + kEyeWidth * 3, out.pixel(0, static_cast<int>(row)));
    }
    return out;
}

// Peak signal-to-noise ratio over the RGB channels; 1e9 for identical images.
double psnr(const splat::Image& a, const splat::Image& b) {
    if (a.width != b.width || a.height != b.height) return -1.0;
    double squared = 0.0;
    const double count = static_cast<double>(a.pixel_count()) * 3.0;
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const double difference = static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]);
        squared += difference * difference;
    }
    if (squared <= 0.0) return 1.0e9;
    const double mse = squared / count;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

// PSNR over the pixels where `reference` shows the photo rather than the
// neutral backdrop (the capture-frustum fade leaves most of a wide headset eye
// as backdrop, so an overall PSNR can be high even when the photo is wrong).
// Returns -1 and `fraction` 0 when there is no such pixel.
double psnr_over_photo(const splat::Image& a, const splat::Image& b,
                       const splat::Image& reference, double& fraction) {
    const double backdrop = 255.0 * (1.055 * std::pow(0.18, 1.0 / 2.4) - 0.055);
    double squared = 0.0;
    size_t masked = 0;
    for (size_t i = 0; i < reference.pixel_count(); ++i) {
        const uint8_t* p = reference.rgb.data() + i * 3;
        const double luminance = (p[0] + p[1] + p[2]) / 3.0;
        if (std::abs(luminance - backdrop) <= 8.0) continue;
        ++masked;
        for (int channel = 0; channel < 3; ++channel) {
            const double difference = static_cast<double>(a.rgb[i * 3 + channel]) -
                                      static_cast<double>(b.rgb[i * 3 + channel]);
            squared += difference * difference;
        }
    }
    fraction = static_cast<double>(masked) / static_cast<double>(reference.pixel_count());
    if (masked == 0) return -1.0;
    if (squared <= 0.0) return 1.0e9;
    const double mse = squared / (static_cast<double>(masked) * 3.0);
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

// Mean RGB luminance of one image, for a "not a flat backdrop" sanity check.
double mean_luminance(const splat::Image& image) {
    double sum = 0.0;
    for (size_t i = 0; i < image.pixel_count(); ++i) {
        const uint8_t* p = image.rgb.data() + i * 3;
        sum += (p[0] + p[1] + p[2]) / 3.0;
    }
    return sum / static_cast<double>(std::max<size_t>(image.pixel_count(), 1));
}

// Where the renderer must put one world point in one eye of a side-by-side
// frame: an independent implementation of the shaders' camera (the yaw, pitch,
// roll view matrix of splat_renderer.cpp and splat.vert's per-eye x shift and
// principal point), derived from the projection's own definition. If the
// renderer's view matrix drops a rotation, uses a different roll sign or moves
// the eye the wrong way, the rendered splat and this prediction disagree.
void project_point(const splat::FrameView& view, const splat::HeadsetProjection& projection,
                   int eye, const float world[3], double& px, double& py) {
    const double yaw = view.midpoint.yaw;
    const double pitch = view.midpoint.pitch;
    const double roll = view.midpoint.roll;
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cr = std::cos(roll), sr = std::sin(roll);
    const double f[3] = {sy * cp, sp, cy * cp};
    const double s0[3] = {cy, 0.0, -sy};
    const double d0[3] = {f[1] * s0[2] - f[2] * s0[1], f[2] * s0[0] - f[0] * s0[2],
                          f[0] * s0[1] - f[1] * s0[0]};
    double right[3], down[3];
    for (int i = 0; i < 3; ++i) {
        right[i] = cr * s0[i] + sr * d0[i];
        down[i] = cr * d0[i] - sr * s0[i];
    }
    const double q[3] = {world[0] - view.midpoint.position[0],
                         world[1] - view.midpoint.position[1],
                         world[2] - view.midpoint.position[2]};
    const auto dot = [](const double a[3], const double b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    // splat.vert: a splat at view position p is drawn at p.x - eye_offset, with
    // the left eye at -ipd/2 along the camera's x axis.
    const double eye_offset = eye == 0 ? -0.5 * view.ipd : 0.5 * view.ipd;
    const double x = dot(q, right) - eye_offset;
    const double y = dot(q, down);
    const double z = dot(q, f);
    const double cx = projection.intrinsics.cx + projection.eye_shift[eye == 0 ? 0 : 1];
    px = cx + projection.intrinsics.fx * x / z;
    py = projection.intrinsics.cy + projection.intrinsics.fy * y / z;
}

// Intensity-weighted centroid of the content in an image (backdrop excluded):
// where a single splat landed, to a fraction of a pixel.
bool image_centroid(const splat::Image& image, double& cx, double& cy) {
    const double backdrop = 255.0 * (1.055 * std::pow(0.18, 1.0 / 2.4) - 0.055);
    double weight = 0.0, sx = 0.0, sy = 0.0;
    for (int row = 0; row < image.height; ++row) {
        for (int column = 0; column < image.width; ++column) {
            const uint8_t* p = image.pixel(column, row);
            const double luminance = (p[0] + p[1] + p[2]) / 3.0;
            const double w = luminance - backdrop - 2.0;
            if (w <= 0.0) continue;
            weight += w;
            sx += w * (column + 0.5);
            sy += w * (row + 0.5);
        }
    }
    if (!(weight > 0.0)) return false;
    cx = sx / weight;
    cy = sy / weight;
    return true;
}

struct Shift {
    int dx = 0;  // full-resolution pixels: test(x + dx) matches reference(x)
    int dy = 0;
    double score = -2.0;
};

// Box-downsamples both images by `factor`, turns them into zero-mean
// luminance, and finds the offset with the best normalised correlation.
Shift find_shift(const splat::Image& reference, const splat::Image& test, int factor,
                 int max_dx, int max_dy) {
    const int width = reference.width / factor;
    const int height = reference.height / factor;
    if (width != test.width / factor || height != test.height / factor || width <= 0 || height <= 0) {
        return Shift{};
    }
    std::vector<float> a(static_cast<size_t>(width) * height, 0.0f);
    std::vector<float> b(a.size(), 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum_a = 0.0f, sum_b = 0.0f;
            for (int oy = 0; oy < factor; ++oy) {
                for (int ox = 0; ox < factor; ++ox) {
                    const uint8_t* pa = reference.pixel(x * factor + ox, y * factor + oy);
                    const uint8_t* pb = test.pixel(x * factor + ox, y * factor + oy);
                    sum_a += (pa[0] + pa[1] + pa[2]) / 3.0f;
                    sum_b += (pb[0] + pb[1] + pb[2]) / 3.0f;
                }
            }
            a[static_cast<size_t>(y) * width + x] = sum_a;
            b[static_cast<size_t>(y) * width + x] = sum_b;
        }
    }
    double mean_a = 0.0, mean_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        mean_a += a[i];
        mean_b += b[i];
    }
    mean_a /= static_cast<double>(a.size());
    mean_b /= static_cast<double>(b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] -= static_cast<float>(mean_a);
        b[i] -= static_cast<float>(mean_b);
    }

    Shift best;
    for (int dy = -max_dy; dy <= max_dy; ++dy) {
        for (int dx = -max_dx; dx <= max_dx; ++dx) {
            double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
            for (int y = 0; y < height; ++y) {
                const int ty = y + dy;
                if (ty < 0 || ty >= height) continue;
                for (int x = 0; x < width; ++x) {
                    const int tx = x + dx;
                    if (tx < 0 || tx >= width) continue;
                    const double va = a[static_cast<size_t>(y) * width + x];
                    const double vb = b[static_cast<size_t>(ty) * width + tx];
                    dot += va * vb;
                    norm_a += va * va;
                    norm_b += vb * vb;
                }
            }
            if (!(norm_a > 0.0) || !(norm_b > 0.0)) continue;
            const double score = dot / std::sqrt(norm_a * norm_b);
            if (score > best.score) {
                best.score = score;
                best.dx = dx * factor;
                best.dy = dy * factor;
            }
        }
    }
    return best;
}

#if defined(SPLAT_HAVE_VULKAN)

// One renderer for one mono frame, for the renderer-robustness checks.
bool render_single(splat::VulkanContext& context, const splat::SplatScene& scene,
                   const splat::RendererOptions& options, const splat::FrameView& view,
                   splat::Image& out, uint32_t& visible, std::string& error) {
    auto renderer = splat::SplatRenderer::create(context, options, scene);
    if (!renderer.ok()) {
        error = renderer.message();
        return false;
    }
    auto image = splat::create_vulkan_color_image(
        context, options.target_format, renderer.value()->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image.ok()) {
        error = image.message();
        return false;
    }
    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = renderer.value()->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const splat::Status status = renderer.value()->render(view, target, true);
    if (!status.ok()) {
        splat::destroy_vulkan_image(context, image.value());
        error = status.message();
        return false;
    }
    auto pixels = renderer.value()->read_image();
    visible = renderer.value()->last_visible_count();
    splat::destroy_vulkan_image(context, image.value());
    if (!pixels.ok()) {
        error = pixels.message();
        return false;
    }
    out = std::move(pixels.value());
    return true;
}

// A white Gaussian at (x, y, z) in metres, 1 cm across.
splat::SplatScene one_splat(float z, float sigma, float x = 0.0f, float y = 0.0f) {
    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {x, y, z};
    const float log_sigma = std::log(sigma);
    scene.log_scale = {log_sigma, log_sigma, log_sigma};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    const float white_dc = 0.9f / 0.28209479177387814f;
    scene.sh_dc = {white_dc, white_dc, white_dc};
    scene.opacity_logit = {12.0f};
    return scene;
}

// Fraction of pixels that are not the neutral backdrop (encoded 0.18 linear).
double photo_coverage(const splat::Image& image) {
    const double backdrop = 255.0 * (1.055 * std::pow(0.18, 1.0 / 2.4) - 0.055);
    size_t content = 0;
    for (size_t i = 0; i < image.pixel_count(); ++i) {
        const uint8_t* p = image.rgb.data() + i * 3;
        const double luminance = (p[0] + p[1] + p[2]) / 3.0;
        if (std::abs(luminance - backdrop) > 4.0) ++content;
    }
    return static_cast<double>(content) / static_cast<double>(std::max<size_t>(image.pixel_count(), 1));
}

struct Rig {
    std::unique_ptr<splat::VulkanContext> context;
    std::unique_ptr<splat::SplatRenderer> renderer;
    splat::VulkanImage image;
    splat::RenderTarget target;

    ~Rig() {
        renderer.reset();
        if (context) splat::destroy_vulkan_image(*context, image);
    }

    bool render(const splat::FrameView& view, splat::Image& out, std::string& error) {
        const splat::Status status = renderer->render(view, target, true);
        if (!status.ok()) {
            error = status.message();
            return false;
        }
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer->read_image();
        if (!pixels.ok()) {
            error = pixels.message();
            return false;
        }
        out = std::move(pixels.value());
        return true;
    }
};

// The renderer options the viewer builds from the headset's FOV: stereo at the
// PSVR2 eye size, the tangents' pinhole, and each eye's principal-point offset.
splat::RendererOptions psvr2_options(splat::HeadsetProjection& projection) {
    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeWidth;
    options.eye_height = kEyeHeight;
    options.stereo = true;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    projection = splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);
    options.intrinsics = projection.intrinsics;
    options.eye_center_shift[0] = projection.eye_shift[0];
    options.eye_center_shift[1] = projection.eye_shift[1];
    return options;
}

#endif  // SPLAT_HAVE_VULKAN

}  // namespace

// The convention itself, without a GPU: an Hmd2 pose looking forward must land
// on the renderer's capture pose, yaw +30 must turn the camera right and a
// tracker +y rise must lower the camera in the scene's y-down axes.
TEST_CASE("hmd pose convention maps to the scene camera") {
    const splat::TrackedPose level = make_head_pose(0.4f, 1.6f, 1.0f, 0.0, 0.0);
    float forward[3] = {0.0f, 0.0f, 0.0f};
    splat::hmd_forward(level.quaternion, forward);
    CHECK(forward[0] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(forward[1] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(forward[2] == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(splat::hmd_yaw(level) == doctest::Approx(0.0).epsilon(1e-5));
    CHECK(splat::hmd_pitch(level) == doctest::Approx(0.0).epsilon(1e-5));

    const double yaw = 30.0 * 3.14159265358979323846 / 180.0;
    const splat::TrackedPose right = make_head_pose(0.4f, 1.6f, 1.0f, yaw, 0.0);
    CHECK(splat::hmd_yaw(right) == doctest::Approx(yaw).epsilon(1e-5));
    splat::hmd_forward(right.quaternion, forward);
    CHECK(forward[0] == doctest::Approx(std::sin(yaw)).epsilon(1e-5));
    CHECK(forward[2] == doctest::Approx(std::cos(yaw)).epsilon(1e-5));

    const double pitch = 10.0 * 3.14159265358979323846 / 180.0;
    const splat::TrackedPose down = make_head_pose(0.4f, 1.6f, 1.0f, 0.0, pitch);
    CHECK(splat::hmd_pitch(down) == doctest::Approx(pitch).epsilon(1e-5));

    float scene[3] = {0.0f, 0.0f, 0.0f};
    const float tracker[3] = {0.1f, 1.7f, -2.0f};
    splat::hmd_scene_position(tracker, scene);
    CHECK(scene[0] == doctest::Approx(0.1f));
    CHECK(scene[1] == doctest::Approx(-1.7f));
    CHECK(scene[2] == doctest::Approx(2.0f));

    // Placement: a head at an arbitrary spot and orientation gets the capture
    // camera (scene origin, yaw 0, pitch 0) and the head's IPD.
    splat::HmdView view;
    view.set_comfort_radius(2.0f);  // the VR default, see vr_mode
    const splat::HeadPoses placed =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.5, 0.12), 0.066f);
    REQUIRE(view.place(placed));
    const splat::FrameView frame = view.frame();
    CHECK(frame.midpoint.position[0] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(frame.midpoint.position[1] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(frame.midpoint.position[2] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(frame.midpoint.yaw == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(frame.midpoint.pitch == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(frame.ipd == doctest::Approx(0.066f).epsilon(1e-4));
    CHECK(frame.comfort_fade == 1.0f);

    // Relative head motion after a level placement: yaw +30 adds +30 to the
    // scene yaw, a 10 cm tracker rise moves the scene camera 10 cm up (scene
    // y = -0.1). A placement that is itself pitched or rolled turns the scene
    // frame with the head, so those angles only add up exactly from level; the
    // rigid-transform test below covers the general case.
    splat::HmdView level_view;
    level_view.set_comfort_radius(2.0f);
    REQUIRE(level_view.place(
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.5, 0.0), 0.066f)));
    const splat::HeadPoses yawed =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.5 + yaw, 0.0), 0.066f);
    REQUIRE(level_view.update(yawed));
    CHECK(level_view.frame().midpoint.yaw == doctest::Approx(yaw).epsilon(1e-5));
    const splat::HeadPoses raised =
        head_with_eyes(make_head_pose(0.35f, 1.72f, 0.8f, 0.5 + yaw, 0.0), 0.066f);
    REQUIRE(level_view.update(raised));
    CHECK(level_view.frame().midpoint.position[1] == doctest::Approx(-0.1f).epsilon(1e-4));
}

// All six degrees of freedom come from one transform. At the placement pose the
// camera is the scene origin looking forward; from then on it is the head's
// position and orientation expressed in the placement frame, R_placement^T *
// pose. Anything less - the roll dropped, the step rotated by the yaw alone -
// leaves the scene partly following the head, which is what the first headset
// session saw.
TEST_CASE("the placement pose is one rigid transform, position and orientation") {
    // Placed with the head turned, pitched and rolled, as a real first tracked
    // pose is.
    const splat::TrackedPose placement =
        make_head_pose(0.35f, 1.62f, 0.8f, 0.5, 0.12, -0.07);
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    REQUIRE(view.place(head_with_eyes(placement, 0.066f)));
    const splat::FrameView placed = view.frame();
    CHECK(placed.midpoint.yaw == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(placed.midpoint.pitch == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(placed.midpoint.roll == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(placed.midpoint.position[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(placed.midpoint.position[1] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(placed.midpoint.position[2] == doctest::Approx(0.0f).epsilon(1e-6));

    double placement_right[3], placement_down[3], placement_forward[3];
    head_axes(placement, placement_right, placement_down, placement_forward);

    const splat::TrackedPose moved =
        make_head_pose(0.41f, 1.57f, 0.86f, 0.68, 0.02, 0.11);
    REQUIRE(view.update(head_with_eyes(moved, 0.066f)));
    const splat::FrameView frame = view.frame();
    double moved_right[3], moved_down[3], moved_forward[3];
    head_axes(moved, moved_right, moved_down, moved_forward);
    double camera_right[3], camera_down[3], camera_forward[3];
    camera_axes(frame.midpoint, camera_right, camera_down, camera_forward);
    // Every camera axis is the head's axis resolved on the placement axes: no
    // rotation is dropped and none is applied twice.
    CHECK(near(camera_right[0], dot3(placement_right, moved_right)));
    CHECK(near(camera_right[1], dot3(placement_down, moved_right)));
    CHECK(near(camera_right[2], dot3(placement_forward, moved_right)));
    CHECK(near(camera_down[0], dot3(placement_right, moved_down)));
    CHECK(near(camera_down[1], dot3(placement_down, moved_down)));
    CHECK(near(camera_down[2], dot3(placement_forward, moved_down)));
    CHECK(near(camera_forward[0], dot3(placement_right, moved_forward)));
    CHECK(near(camera_forward[1], dot3(placement_down, moved_forward)));
    CHECK(near(camera_forward[2], dot3(placement_forward, moved_forward)));

    // The position is the same transform: the tracker step (0.06, -0.05, -0.06)
    // in scene axes, resolved on the placement axes.
    const double step[3] = {0.41 - 0.35, -(1.57 - 1.62), -(0.86 - 0.80)};
    CHECK(near(frame.midpoint.position[0], dot3(placement_right, step)));
    CHECK(near(frame.midpoint.position[1], dot3(placement_down, step)));
    CHECK(near(frame.midpoint.position[2], dot3(placement_forward, step)));

    // The parallax direction is the camera's own x axis: a 5 cm step along the
    // head's x (the axis the eyes sit on) moves the camera 5 cm along its own x,
    // whatever the head's orientation or the placement pose. This is what "the
    // eye offsets and the head step are along the head's x axis" means; the old
    // mapping rotated the step by the yaw alone and put the rest of the step
    // sideways.
    splat::TrackedPose stepped = moved;
    const double head_right_tracker[3] = {moved_right[0], -moved_right[1], -moved_right[2]};
    for (int axis = 0; axis < 3; ++axis) {
        stepped.position[axis] =
            static_cast<float>(moved.position[axis] + 0.05 * head_right_tracker[axis]);
    }
    REQUIRE(view.update(head_with_eyes(stepped, 0.066f)));
    const splat::FrameView stepped_frame = view.frame();
    const double step_delta[3] = {stepped_frame.midpoint.position[0] - frame.midpoint.position[0],
                                    stepped_frame.midpoint.position[1] - frame.midpoint.position[1],
                                    stepped_frame.midpoint.position[2] -
                                        frame.midpoint.position[2]};
    MESSAGE("5 cm along the head's x, in the camera: " << step_delta[0] << ", " << step_delta[1]
                                                       << ", " << step_delta[2]);
    for (int i = 0; i < 3; ++i) CHECK(near(step_delta[i], 0.05 * camera_right[i]));
    CHECK(near(dot3(step_delta, camera_down), 0.0));
    CHECK(near(dot3(step_delta, camera_forward), 0.0));
}

// VR7: the first placement is the recentre path. The 2026-10-27 runs placed the
// scene on the first frame with worn=1/tracked=1, the tracker's SLAM reset
// moved the head ~90 deg in its new local frame right after, and the photo
// stayed where the old frame had put it - to the user's right until they held
// Cross ("when I first start the app the scene is to the right"). Both paths
// now end in recentre_here() + reset_scene_adjust(), so a first frame cannot be
// placed differently from a recentre.
TEST_CASE("the first placement is the recentre path") {
    const splat::TrackedPose head = make_head_pose(0.42f, 1.71f, 0.60f, 0.83, 0.11, -0.06);
    const splat::HeadPoses poses = head_with_eyes(head, 0.064f);

    // What vr_mode's first placement does: the settled pose is cached, then the
    // shared placement call.
    splat::HmdView first;
    first.set_comfort_radius(2.0f);
    REQUIRE(first.update(poses));
    first.recentre_here();
    first.reset_scene_adjust();

    // What the Cross hold does: the same two calls (the pose is already cached
    // from this frame's tracker sample).
    splat::HmdView recentred;
    recentred.set_comfort_radius(2.0f);
    REQUIRE(recentred.update(poses));
    const double removed_yaw = recentred.view_yaw() * 180.0 / 3.14159265358979323846;
    recentred.recentre_here();
    recentred.reset_scene_adjust();

    // HmdView::place() is that same path for a fresh pose (update + recentre).
    splat::HmdView placed;
    placed.set_comfort_radius(2.0f);
    REQUIRE(placed.place(poses));
    const splat::FrameView a = first.frame();
    const splat::FrameView b = recentred.frame();
    const splat::FrameView c = placed.frame();
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(first.anchor()[axis] == doctest::Approx(recentred.anchor()[axis]));
        CHECK(first.anchor()[axis] == doctest::Approx(placed.anchor()[axis]));
        CHECK(first.head_position()[axis] == doctest::Approx(recentred.head_position()[axis]));
        CHECK(a.midpoint.position[axis] == doctest::Approx(b.midpoint.position[axis]));
        CHECK(a.midpoint.position[axis] == doctest::Approx(c.midpoint.position[axis]));
        CHECK(first.scene_offset()[axis] == doctest::Approx(0.0f));
    }
    CHECK(a.midpoint.yaw == doctest::Approx(b.midpoint.yaw));
    CHECK(a.midpoint.pitch == doctest::Approx(b.midpoint.pitch));
    CHECK(a.midpoint.roll == doctest::Approx(b.midpoint.roll));
    CHECK(a.scene_scale == doctest::Approx(b.scene_scale));
    CHECK(a.scene_scale == doctest::Approx(1.0f));
    CHECK(b.ipd == doctest::Approx(0.064f).epsilon(1e-4));
    CHECK(first.scene_turn() == doctest::Approx(0.0f));

    // Placed at the settled pose the frame is the capture pose: the camera is
    // the scene's origin looking down its +z, so the photo is in front.
    CHECK(b.midpoint.position[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(b.midpoint.position[1] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(b.midpoint.position[2] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(b.midpoint.yaw == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(b.midpoint.pitch == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(b.midpoint.roll == doctest::Approx(0.0f).epsilon(1e-6));
    // The log's "N deg of yaw removed" is the head's own yaw at the placement.
    CHECK(removed_yaw == doctest::Approx(splat::hmd_yaw(head) * 180.0 / 3.14159265358979323846)
                             .epsilon(1e-4));
}

// VR7: the first placement waits for a pose the tracker is not redefining. The
// two 2026-10-27 sessions show exactly what this rejects: a 13-97 deg rotation
// and a 22-141 mm translation in a single 16.7 ms sample (the SLAM reset), while
// the fastest real head turn in the same logs changes the orientation 4.1 deg.
TEST_CASE("the first placement waits for a held pose") {
    const splat::HeadPoses held =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.80f, 0.30, 0.05), 0.066f);

    // Not worn or not tracked: no window.
    splat::PlacementWait wait;
    CHECK_FALSE(wait.update(false, true, held.head, 0.0).place);
    CHECK_FALSE(wait.waiting());
    CHECK_FALSE(wait.update(true, false, held.head, 0.1).place);
    CHECK_FALSE(wait.waiting());

    // Worn and tracked: place exactly at kPlaceStableSeconds, once.
    CHECK(splat::kPlaceStableSeconds == doctest::Approx(0.5));
    CHECK_FALSE(wait.update(true, true, held.head, 1.0).place);
    CHECK(wait.waiting());
    CHECK_FALSE(wait.update(true, true, held.head, 1.4).place);
    const splat::PlacementWait::Step done = wait.update(true, true, held.head, 1.5);
    CHECK(done.place);
    CHECK(wait.placed());
    CHECK(done.stable_seconds == doctest::Approx(0.5).epsilon(1e-9));
    CHECK_FALSE(wait.update(true, true, held.head, 1.6).place);
    CHECK_FALSE(wait.update(true, true, held.head, 9.0).place);

    // Taken off (or a lost pose) restarts the window.
    splat::PlacementWait off;
    CHECK_FALSE(off.update(true, true, held.head, 10.0).place);
    CHECK_FALSE(off.update(true, true, held.head, 10.3).place);
    CHECK_FALSE(off.update(true, false, held.head, 10.4).place);
    CHECK_FALSE(off.update(true, true, held.head, 10.5).place);
    CHECK_FALSE(off.update(true, true, held.head, 10.8).place);  // 0.3 s in
    CHECK(off.update(true, true, held.head, 11.0).place);        // 0.5 s in

    // A real head turn (4 deg per frame in those logs) is not a jump.
    splat::PlacementWait turning;
    CHECK_FALSE(turning.update(true, true, held.head, 20.0).place);
    for (int frame = 1; frame <= 40; ++frame) {
        const splat::HeadPoses turned = head_with_eyes(
            make_head_pose(0.35f, 1.62f, 0.80f, 0.30 + 0.07 * frame, 0.05), 0.066f);
        const splat::PlacementWait::Step step =
            turning.update(true, true, turned.head, 20.0 + 0.0167 * frame);
        CHECK_FALSE(step.restarted);
    }
    CHECK(turning.placed());

    // The sessions' re-anchor: 94 deg and 10 cm in one sample restarts the
    // window instead of anchoring the scene to the frame that is about to be
    // replaced.
    const splat::TrackedPose before = make_head_pose(0.0f, 1.62f, 0.0f, 0.0, 0.0);
    const splat::TrackedPose after =
        make_head_pose(0.10f, 1.62f, 0.02f, 94.0 * 3.14159265358979323846 / 180.0, 0.03);
    CHECK(splat::pose_jumped(before, after));
    CHECK_FALSE(splat::pose_jumped(before, before));
    CHECK_FALSE(splat::pose_jumped(
        before, make_head_pose(0.01f, 1.62f, 0.0f, 4.0 * 3.14159265358979323846 / 180.0, 0.0)));
    splat::PlacementWait jumping;
    CHECK_FALSE(jumping.update(true, true, before, 30.0).place);
    const splat::PlacementWait::Step restart = jumping.update(true, true, after, 30.2);
    CHECK(restart.restarted);
    CHECK_FALSE(restart.place);
    CHECK_FALSE(jumping.update(true, true, after, 30.6).place);   // 0.4 s since the jump
    CHECK(jumping.update(true, true, after, 30.7).place);         // 0.5 s since the jump

    // What the gate is for: anchored at the pre-reset pose and then given the
    // post-reset pose, the photo sits 94 deg to the side - the reported bug.
    const splat::HeadPoses before_poses = head_with_eyes(before, 0.063f);
    const splat::HeadPoses after_poses = head_with_eyes(after, 0.063f);
    splat::HmdView early;
    early.set_comfort_radius(2.0f);
    REQUIRE(early.place(before_poses));
    REQUIRE(early.update(after_poses));
    MESSAGE("placing at the pre-reset pose leaves the photo " << early.frame().midpoint.yaw * 180.0 /
                                                                    3.14159265358979323846
                                                             << " deg off");
    CHECK(std::abs(early.frame().midpoint.yaw) > 1.0);
    // Anchored at the settled pose, the photo is in front.
    splat::HmdView settled;
    settled.set_comfort_radius(2.0f);
    REQUIRE(settled.place(after_poses));
    CHECK(settled.frame().midpoint.yaw == doctest::Approx(0.0f).epsilon(1e-6));
}

// A scene switch must not move the user: the new scene's capture camera goes to
// the same world anchor with the same scale and offset (2026-10-26 feedback:
// "in VR we should be free to explore a scene, not have it adjust to our
// moves"). The only things a switch changes are the new scene's two depth
// measures - the centre for the log and the near content that clamps the next
// press's pivot - so setting them is exactly the switch, from the view's point
// of view.
TEST_CASE("a scene switch keeps the world anchor, scale and offset") {
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    const splat::HeadPoses placement =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.32, 0.08), 0.066f);
    REQUIRE(view.place(placement));
    const float anchor_before[3] = {view.anchor()[0], view.anchor()[1], view.anchor()[2]};
    // The anchor is the eye midpoint in the scene's OpenCV axes (x right, y
    // down, z forward), so a tracker height of +1.62 m is -1.62 here.
    CHECK(anchor_before[0] == doctest::Approx(0.35f));
    CHECK(anchor_before[1] == doctest::Approx(-1.62f));
    CHECK(anchor_before[2] == doctest::Approx(-0.8f));

    // The user adjusts the scene, then walks and looks elsewhere.
    view.set_median_depth(1.9f);
    view.set_near_depth(2.2f);
    view.begin_scale_gesture();
    view.scale_scene(1.6f);
    view.end_scale_gesture();
    view.move_scene(0.4f, -0.25f, 0.15f);
    const double turn = 25.0 * 3.14159265358979323846 / 180.0;
    view.turn_scene(turn);
    REQUIRE(view.update(head_with_eyes(make_head_pose(0.6f, 1.7f, 0.5f, 0.9, -0.05), 0.066f)));
    const splat::FrameView before = view.frame();
    const float offset_before[3] = {view.scene_offset()[0], view.scene_offset()[1],
                                    view.scene_offset()[2]};
    const float scale_before = view.world_scale();

    // Switch to a scene with different depths: nothing about the view may
    // change.
    view.set_median_depth(0.7f);
    view.set_near_depth(0.9f);
    const splat::FrameView after = view.frame();
    CHECK(view.anchor()[0] == doctest::Approx(anchor_before[0]));
    CHECK(view.anchor()[1] == doctest::Approx(anchor_before[1]));
    CHECK(view.anchor()[2] == doctest::Approx(anchor_before[2]));
    CHECK(view.world_scale() == doctest::Approx(scale_before));
    CHECK(view.scene_offset()[0] == doctest::Approx(offset_before[0]));
    CHECK(view.scene_offset()[1] == doctest::Approx(offset_before[1]));
    CHECK(view.scene_offset()[2] == doctest::Approx(offset_before[2]));
    CHECK(view.scene_turn() == doctest::Approx(turn).epsilon(1e-5));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(after.midpoint.position[axis] ==
              doctest::Approx(before.midpoint.position[axis]).epsilon(1e-6));
    }
    CHECK(after.midpoint.yaw == doctest::Approx(before.midpoint.yaw).epsilon(1e-6));
    CHECK(after.midpoint.pitch == doctest::Approx(before.midpoint.pitch).epsilon(1e-6));
    CHECK(after.midpoint.roll == doctest::Approx(before.midpoint.roll).epsilon(1e-6));
    CHECK(after.scene_scale == doctest::Approx(before.scene_scale));

    // The next tracker sample follows the head from the same anchor: the
    // switch did not re-place the scene at the head.
    REQUIRE(view.update(head_with_eyes(make_head_pose(0.6f, 1.7f, 0.5f, 0.9, -0.05), 0.066f)));
    const splat::FrameView resampled = view.frame();
    CHECK(resampled.midpoint.yaw == doctest::Approx(before.midpoint.yaw).epsilon(1e-5));
    CHECK(resampled.midpoint.position[0] ==
          doctest::Approx(before.midpoint.position[0]).epsilon(1e-5));
}

// VR7: a scale press is visible and the viewer does not move. The scene is
// grown about a point fixed in the *world* 1.5 m ahead of the eyes (or the
// scene's near content when that is closer), so that point keeps its place in
// the picture while everything else expands around it. VR6 scaled about the
// capture point between the eyes - a fixed point of the scene, so the image was
// the same at any scale ("scale does absolutely nothing", headset test 4) -
// and VR5 compensated a pivot 25-50 m away, which translated the camera by up
// to 139 m in one press (runs/vr5/REPORT.md, runs/vr6/REPORT.md).
TEST_CASE("a scale press grows the scene about a point fixed ahead of the eyes") {
    const float kNearScene = 2.4f;   // the photo's near edge, scene metres
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    const splat::HeadPoses capture =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.32, 0.08), 0.066f);
    REQUIRE(view.place(capture));
    view.set_median_depth(1.7f);
    view.set_near_depth(kNearScene);

    const splat::HeadsetProjection projection =
        splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);
    // The picture's centre on the midpoint ray (project_point shifts a point by
    // this eye's principal offset even at ipd 0).
    const double centre_x = projection.intrinsics.cx + projection.eye_shift[0];
    const double centre_y = projection.intrinsics.cy;

    // The renderer draws a scene point at scale * x (project.comp); the scene's
    // offset is the camera's, so a world point is scale * x + offset and the
    // point handed to the projection is world - offset.
    auto world_of = [](const splat::FrameView& frame, const float offset[3],
                       const float scene_point[3], float out[3]) {
        for (int axis = 0; axis < 3; ++axis) {
            out[axis] = scene_point[axis] * frame.scene_scale + offset[axis];
        }
    };
    auto project_world = [&](const splat::FrameView& frame, const float offset[3],
                             const float world[3], double& px, double& py) {
        splat::FrameView midpoint = frame;
        midpoint.ipd = 0.0f;
        const float render_point[3] = {world[0] - offset[0], world[1] - offset[1],
                                       world[2] - offset[2]};
        project_point(midpoint, projection, 0, render_point, px, py);
    };
    // Depth of a *world* point from the eyes: the renderer's camera position is
    // the eyes' world position minus the offset (world = scale * x + offset is
    // the scene's transform), so the eyes are at camera + offset.
    auto depth_of = [](const splat::FrameView& frame, const float offset[3], const float world[3]) {
        double axes_right[3], axes_down[3], axes_forward[3];
        camera_axes(frame.midpoint, axes_right, axes_down, axes_forward);
        const double relative[3] = {world[0] - (frame.midpoint.position[0] + offset[0]),
                                    world[1] - (frame.midpoint.position[1] + offset[1]),
                                    world[2] - (frame.midpoint.position[2] + offset[2])};
        return dot3(relative, axes_forward);
    };

    // The press's pivot: 1.5 m ahead on the horizontal gaze, at eye level. The
    // scene's near content is 2.4 m away, so it does not clamp.
    view.begin_scale_gesture();
    CHECK(view.scale_gesture());
    CHECK(view.scale_pivot_distance() == doctest::Approx(splat::kScalePivotMetres));
    const float pivot[3] = {view.scale_pivot_world()[0], view.scale_pivot_world()[1],
                            view.scale_pivot_world()[2]};
    CHECK(pivot[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(pivot[1] == doctest::Approx(0.0f).epsilon(1e-6));  // eye level, gaze horizontal
    CHECK(pivot[2] == doctest::Approx(1.5f).epsilon(1e-6));

    const splat::FrameView before = view.frame();
    const float offset_before[3] = {view.scene_offset()[0], view.scene_offset()[1],
                                    view.scene_offset()[2]};
    const float head_before[3] = {view.head_position()[0], view.head_position()[1],
                                  view.head_position()[2]};
    double pivot_x0 = 0.0, pivot_y0 = 0.0;
    project_world(before, offset_before, pivot, pivot_x0, pivot_y0);
    CHECK(std::abs(pivot_x0 - centre_x) < 0.5);
    CHECK(std::abs(pivot_y0 - centre_y) < 0.5);
    CHECK(depth_of(before, offset_before, pivot) == doctest::Approx(1.5).epsilon(1e-4));

    // A 2x press, as 3 s of a full R2 does.
    view.scale_scene(2.0f);
    const splat::FrameView after = view.frame();
    CHECK(after.scene_scale == doctest::Approx(2.0f));
    CHECK(view.world_scale() == doctest::Approx(2.0f));
    // The viewer did not move: the head is bit-identical, and the pivot - a
    // point of the world - is still exactly where it was relative to the eyes.
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(view.head_position()[axis] == head_before[axis]);
    }
    double pivot_x1 = 0.0, pivot_y1 = 0.0;
    project_world(after, view.scene_offset(), pivot, pivot_x1, pivot_y1);
    MESSAGE("pivot pixel " << pivot_x0 << "," << pivot_y0 << " -> " << pivot_x1 << "," << pivot_y1);
    CHECK(std::abs(pivot_x1 - pivot_x0) < 0.5);
    CHECK(std::abs(pivot_y1 - pivot_y0) < 0.5);
    CHECK(std::abs(pivot_x1 - centre_x) < 0.5);

    // ... and the scale is visible: the photo's near edge (2.4 m ahead) recedes
    // to 1.5 + 2 * (2.4 - 1.5) = 3.3 m and its angular size grows by
    // 2 * 2.4 / 3.3 = 1.45x. VR6's capture-point scale left both at 2.4 * s,
    // i.e. exactly the same image (the old behaviour would print depth 4.8 and
    // an angular ratio of 1.00).
    const float near_point[3] = {0.0f, 0.0f, kNearScene};
    float near_before[3];
    float near_after[3];
    world_of(before, offset_before, near_point, near_before);
    world_of(after, view.scene_offset(), near_point, near_after);
    CHECK(near_before[2] == doctest::Approx(2.4f));
    CHECK(near_after[2] == doctest::Approx(3.3f).epsilon(1e-5));
    const double near_depth_before = depth_of(before, offset_before, near_before);
    const double near_depth_after = depth_of(after, view.scene_offset(), near_after);
    const double angular_gain = (2.0 * near_depth_before) / near_depth_after;
    MESSAGE("near content " << near_depth_before << " m -> " << near_depth_after
                            << " m, angular gain " << angular_gain);
    CHECK(angular_gain > 1.4);

    // The whole transform is the one a scale about the world pivot means:
    // world' = pivot + f * (world - pivot), for every scene point.
    const float samples[3][3] = {{0.0f, 0.0f, 1.7f}, {0.6f, -0.3f, 3.0f}, {-0.4f, 0.5f, 0.8f}};
    for (const auto& sample : samples) {
        float world_0[3];
        float world_1[3];
        world_of(before, offset_before, sample, world_0);
        world_of(after, view.scene_offset(), sample, world_1);
        for (int axis = 0; axis < 3; ++axis) {
            const double expected = pivot[axis] + 2.0 * (world_0[axis] - pivot[axis]);
            CHECK(near(world_1[axis], expected, 1e-4));
        }
    }

    // Shrinking back within the press restores the transform exactly, so a
    // grow-then-shrink round trip cannot drift the scene.
    view.scale_scene(0.5f);
    CHECK(view.world_scale() == doctest::Approx(1.0f).epsilon(1e-6));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(view.scene_offset()[axis] == doctest::Approx(offset_before[axis]).epsilon(1e-6));
    }
    view.end_scale_gesture();
    CHECK_FALSE(view.scale_gesture());

    // The range clamps at 4x and a press at the limit leaves the scene alone
    // instead of sliding it towards the pivot at the rate the trigger asked
    // for (the offset must not drift while the scale cannot follow).
    view.begin_scale_gesture();
    view.scale_scene(100.0f);
    CHECK(view.world_scale() == doctest::Approx(splat::kMaxWorldScale));
    const float at_limit[3] = {view.scene_offset()[0], view.scene_offset()[1],
                               view.scene_offset()[2]};
    view.scale_scene(2.0f);
    CHECK(view.world_scale() == doctest::Approx(splat::kMaxWorldScale));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(view.scene_offset()[axis] == doctest::Approx(at_limit[axis]));
    }
    view.end_scale_gesture();

    // A scene whose near content is closer than 1.5 m gets its pivot there: a
    // miniature is scaled about its own photo, not about a point behind it.
    splat::HmdView miniature;
    miniature.set_comfort_radius(2.0f);
    REQUIRE(miniature.place(capture));
    miniature.set_median_depth(1.7f);
    miniature.set_near_depth(0.8f);
    miniature.begin_scale_gesture();
    CHECK(miniature.scale_pivot_distance() == doctest::Approx(0.8f));
    CHECK(miniature.scale_pivot_world()[2] == doctest::Approx(0.8f).epsilon(1e-6));
}

// VR7: a press after the user has walked, looked up, risen and turned still
// scales about a point fixed in the world in front of the eyes, and composes
// with the lift/move/turn exactly - "lift and scale must not interfere". The
// pivot is built from the head in the placement frame, not from the camera,
// which carries the scene offset (that was VR5's per-frame drift).
TEST_CASE("a scale press after a move and a turn keeps the viewer and the pivot") {
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    REQUIRE(view.place(head_with_eyes(make_head_pose(0.30f, 1.60f, 0.80f, 0.0, 0.0), 0.066f)));
    view.set_median_depth(1.9f);
    view.set_near_depth(2.5f);
    // Step aside, look up and left, walk forward a little, rise and turn.
    const splat::HeadPoses posed =
        head_with_eyes(make_head_pose(0.72f, 1.66f, 0.55f, 0.28, -0.12), 0.066f);
    REQUIRE(view.update(posed));
    view.move_viewer(0.55f, 0.35f, 0.20f);
    view.turn_viewer(0.22f);
    CHECK(std::abs(view.scene_offset()[0]) > 0.2f);
    CHECK(std::abs(view.scene_offset()[1]) > 0.1f);

    // The eyes' position in the placement frame - the frame the pivot, the
    // offset and the scale all live in: the renderer's camera plus the offset.
    const float offset_before[3] = {view.scene_offset()[0], view.scene_offset()[1],
                                    view.scene_offset()[2]};
    const splat::FrameView before = view.frame();
    const float eyes[3] = {before.midpoint.position[0] + offset_before[0],
                           before.midpoint.position[1] + offset_before[1],
                           before.midpoint.position[2] + offset_before[2]};
    const float head_before[3] = {view.head_position()[0], view.head_position()[1],
                                  view.head_position()[2]};

    view.begin_scale_gesture();
    CHECK(view.scale_gesture());
    const float pivot[3] = {view.scale_pivot_world()[0], view.scale_pivot_world()[1],
                            view.scale_pivot_world()[2]};
    CHECK(view.scale_pivot_distance() == doctest::Approx(1.5f));
    // 1.5 m from the eyes horizontally: the eyes' own height whatever the
    // pitch, and the head's placement-frame y whatever the scene's lift.
    const double horizontal = std::sqrt((pivot[0] - eyes[0]) * (pivot[0] - eyes[0]) +
                                        (pivot[2] - eyes[2]) * (pivot[2] - eyes[2]));
    CHECK(horizontal == doctest::Approx(1.5).epsilon(1e-5));
    CHECK(pivot[1] == doctest::Approx(eyes[1]).epsilon(1e-6));
    // It follows the gaze the renderer uses (the turn included), so it is on the
    // picture's centre axis.
    const double gaze = std::atan2(pivot[0] - eyes[0], pivot[2] - eyes[2]);
    CHECK(gaze == doctest::Approx(view.view_yaw()).epsilon(1e-5));

    const splat::HeadsetProjection projection =
        splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);
    // Where a world point lands: the renderer draws scale * x and puts the scene
    // offset on the camera, so the point handed to the projection is
    // world - offset. The relative vector is therefore world - eyes, which is
    // what a camera that has not moved must keep constant.
    auto project_world = [&](const splat::FrameView& frame, const float offset[3],
                             const float world[3], double& px, double& py) {
        splat::FrameView midpoint = frame;
        midpoint.ipd = 0.0f;
        const float render_point[3] = {world[0] - offset[0], world[1] - offset[1],
                                       world[2] - offset[2]};
        project_point(midpoint, projection, 0, render_point, px, py);
    };
    auto depth_of = [](const splat::FrameView& frame, const float offset[3],
                       const float world[3]) {
        double axes_right[3], axes_down[3], axes_forward[3];
        camera_axes(frame.midpoint, axes_right, axes_down, axes_forward);
        const double relative[3] = {world[0] - (frame.midpoint.position[0] + offset[0]),
                                    world[1] - (frame.midpoint.position[1] + offset[1]),
                                    world[2] - (frame.midpoint.position[2] + offset[2])};
        return dot3(relative, axes_forward);
    };
    // The eyes to the pivot, straight line: the pivot is horizontal, so with the
    // head pitched it is below the picture's centre and its depth along the
    // optical axis is 1.5 * cos(pitch) - the distance is what stays 1.5 m.
    auto distance_of = [&eyes](const float world[3]) {
        const double dx = world[0] - eyes[0];
        const double dy = world[1] - eyes[1];
        const double dz = world[2] - eyes[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    project_world(before, offset_before, pivot, x0, y0);
    CHECK(distance_of(pivot) == doctest::Approx(1.5).epsilon(1e-5));
    CHECK(depth_of(before, offset_before, pivot) ==
          doctest::Approx(1.5 * std::cos(before.midpoint.pitch)).epsilon(1e-5));

    // The press: the pivot keeps its pixel, the eyes stay where they are, and
    // the scene grows around the pivot.
    view.scale_scene(1.6f);
    const splat::FrameView after = view.frame();
    CHECK(view.world_scale() == doctest::Approx(1.6f));
    const float* offset_after = view.scene_offset();
    project_world(after, offset_after, pivot, x1, y1);
    MESSAGE("pivot pixel before " << x0 << "," << y0 << ", after a 1.6x press " << x1 << "," << y1);
    CHECK(std::abs(x1 - x0) < 0.5);
    CHECK(std::abs(y1 - y0) < 0.5);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(after.midpoint.position[axis] + offset_after[axis] ==
              doctest::Approx(before.midpoint.position[axis] + offset_before[axis])
                  .epsilon(1e-6));
        CHECK(view.head_position()[axis] == doctest::Approx(head_before[axis]).epsilon(1e-6));
    }
    CHECK(after.midpoint.yaw == doctest::Approx(before.midpoint.yaw).epsilon(1e-7));
    CHECK(after.midpoint.pitch == doctest::Approx(before.midpoint.pitch).epsilon(1e-7));
    CHECK(distance_of(pivot) == doctest::Approx(1.5).epsilon(1e-5));

    // The composition is exact for every scene point: the lift, the walk and
    // the turn are part of world = scale * x + offset, and the scale is applied
    // about the pivot - so the rise the user made scales with the scene instead
    // of pulling them down.
    const float samples[3][3] = {{0.0f, 0.0f, 1.9f}, {0.4f, 0.2f, 0.3f}, {-0.5f, -0.4f, 4.0f}};
    for (const auto& sample : samples) {
        float world_0[3];
        float world_1[3];
        for (int axis = 0; axis < 3; ++axis) {
            world_0[axis] = sample[axis] + offset_before[axis];
            world_1[axis] = sample[axis] * view.world_scale() + offset_after[axis];
        }
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(near(world_1[axis], pivot[axis] + 1.6 * (world_0[axis] - pivot[axis]), 1e-4));
        }
    }

    // The head turns mid-press: the frozen pivot is a point of the world, so it
    // neither follows the gaze nor moves with the scale, and the press still
    // composes about it.
    const splat::HeadPoses turned =
        head_with_eyes(make_head_pose(0.72f, 1.66f, 0.55f, 0.62, 0.05), 0.066f);
    REQUIRE(view.update(turned));
    CHECK(view.scale_pivot_world()[0] == pivot[0]);
    CHECK(view.scale_pivot_world()[1] == pivot[1]);
    CHECK(view.scale_pivot_world()[2] == pivot[2]);
    view.scale_scene(1.25f);
    CHECK(view.world_scale() == doctest::Approx(2.0f).epsilon(1e-6));
    for (const auto& sample : samples) {
        const float world_0[3] = {sample[0] + offset_before[0], sample[1] + offset_before[1],
                                  sample[2] + offset_before[2]};
        const float world_1[3] = {sample[0] * view.world_scale() + view.scene_offset()[0],
                                  sample[1] * view.world_scale() + view.scene_offset()[1],
                                  sample[2] * view.world_scale() + view.scene_offset()[2]};
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(near(world_1[axis], pivot[axis] + 2.0 * (world_0[axis] - pivot[axis]), 1e-4));
        }
    }

    // Shrink back inside the press: exactly the transform before it (1.6 x 1.25
    // = 2x undone by 0.5x).
    view.scale_scene(0.5f);
    CHECK(view.world_scale() == doctest::Approx(1.0f).epsilon(1e-5));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(view.scene_offset()[axis] == doctest::Approx(offset_before[axis]).epsilon(1e-5));
    }
    view.end_scale_gesture();
}

// VR7: the sticks move the *viewer* (locomotion). Left stick forward walks into
// the scene - the scene comes towards you - left stick right steps right, right
// stick up rises and right turns the viewer to their right. VR5/VR6 moved the
// scene instead, which is the opposite of every stick push ("left and right
// stick movement is the other way around - everything is the opposite of what
// it should be", headset test 4). The viewer's forward is the head's horizontal
// gaze in the placement frame: the pitch never leaks into the walk.
TEST_CASE("the sticks walk, rise and turn the viewer") {
    const double deg = 3.14159265358979323846 / 180.0;
    const splat::HeadsetProjection projection =
        splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);

    // Placed level, then the head turned 90 degrees to the right: the viewer's
    // forward is the placement frame's +x.
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    REQUIRE(view.place(head_with_eyes(make_head_pose(0.3f, 1.6f, 0.9f, 0.0, 0.0), 0.066f)));
    REQUIRE(view.update(head_with_eyes(make_head_pose(0.3f, 1.6f, 0.9f, 90.0 * deg, 0.0), 0.066f)));
    view.move_viewer(0.0f, 0.5f, 0.0f);
    // Left stick forward: the viewer walks half a metre along +x. The scene's
    // offset is the same motion the other way, and the camera - the viewer - is
    // at +0.5 in the placement frame.
    CHECK(view.scene_offset()[0] == doctest::Approx(-0.5f).epsilon(1e-5));
    CHECK(view.scene_offset()[1] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(view.scene_offset()[2] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(view.frame().midpoint.position[0] == doctest::Approx(0.5f).epsilon(1e-5));

    // ... and the scene comes towards the viewer: a point 1 m along the walk is
    // 0.5 m away (it was 1 m when the viewer stood at the anchor).
    auto depth_of = [](const splat::FrameView& frame, const float world[3]) {
        double axes_right[3], axes_down[3], axes_forward[3];
        camera_axes(frame.midpoint, axes_right, axes_down, axes_forward);
        const double relative[3] = {world[0] - frame.midpoint.position[0],
                                    world[1] - frame.midpoint.position[1],
                                    world[2] - frame.midpoint.position[2]};
        return dot3(relative, axes_forward);
    };
    const float ahead[3] = {1.0f, 0.0f, 0.0f};
    CHECK(depth_of(view.frame(), ahead) == doctest::Approx(0.5).epsilon(1e-5));

    // Pitch must not leak into the walk: look 40 degrees down and push forward
    // again - still along the head's yaw, no vertical component.
    REQUIRE(view.update(head_with_eyes(
        make_head_pose(0.3f, 1.6f, 0.9f, 90.0 * deg, 40.0 * deg), 0.066f)));
    view.move_viewer(0.0f, 0.5f, 0.0f);
    CHECK(view.scene_offset()[0] == doctest::Approx(-1.0f).epsilon(1e-5));
    CHECK(view.scene_offset()[1] == doctest::Approx(0.0f).epsilon(1e-5));

    // Left stick right: the viewer steps to the right, the scene goes left.
    splat::HmdView walking;
    walking.set_comfort_radius(2.0f);
    REQUIRE(walking.place(head_with_eyes(make_head_pose(0.0f, 1.6f, 0.0f, 0.0, 0.0), 0.063f)));
    walking.move_viewer(0.25f, 0.0f, 0.0f);
    CHECK(walking.frame().midpoint.position[0] == doctest::Approx(0.25f).epsilon(1e-5));
    CHECK(walking.frame().midpoint.position[2] == doctest::Approx(0.0f).epsilon(1e-5));

    // Right stick up: the viewer rises 0.2 m (scene y is down, so the camera's
    // y is -0.2), and a point straight ahead moves down in the picture.
    double before_x = 0.0, before_y = 0.0, after_x = 0.0, after_y = 0.0;
    const float point[3] = {0.0f, 0.0f, 3.0f};
    project_point(walking.frame(), projection, 0, point, before_x, before_y);
    walking.move_viewer(0.0f, 0.0f, 0.2f);
    CHECK(walking.frame().midpoint.position[1] == doctest::Approx(-0.2f).epsilon(1e-5));
    project_point(walking.frame(), projection, 0, point, after_x, after_y);
    MESSAGE("rise 0.2 m: a point ahead moves from y " << before_y << " to " << after_y);
    CHECK(after_y > before_y + 5.0);

    // Right stick right: the viewer turns to their right. The renderer's camera
    // yaw rises (view_yaw = scene yaw - turn), the content moves left, and the
    // CSV's turn_deg records the scene's own turn, the opposite sign.
    splat::HmdView turning;
    turning.set_comfort_radius(2.0f);
    REQUIRE(turning.place(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 0.0f, 0.0f), 0.063f)));
    project_point(turning.frame(), projection, 0, point, before_x, before_y);
    turning.turn_viewer(10.0 * deg);
    project_point(turning.frame(), projection, 0, point, after_x, after_y);
    MESSAGE("turn right 10 deg: content x " << before_x << " -> " << after_x);
    CHECK(after_x < before_x - 5.0);
    CHECK(turning.frame().midpoint.yaw == doctest::Approx(10.0 * deg).epsilon(1e-5));
    CHECK(turning.view_yaw() == doctest::Approx(10.0 * deg).epsilon(1e-5));
    CHECK(turning.scene_turn() == doctest::Approx(-10.0 * deg).epsilon(1e-5));
    CHECK(turning.scene_yaw() == doctest::Approx(0.0).epsilon(1e-5));

    // With the head yawed 30 degrees and the viewer turned 10 right, the logged
    // yaw is 40 degrees (scene yaw 30 minus the -10 turn) and turn_deg is -10:
    // the columns still describe the renderer's camera and the turn.
    REQUIRE(turning.update(head_with_eyes(
        make_head_pose(0.0f, 0.0f, 0.0f, 30.0 * deg, 0.0), 0.063f)));
    CHECK(turning.scene_yaw() == doctest::Approx(30.0 * deg).epsilon(1e-5));
    CHECK(turning.view_yaw() == doctest::Approx(40.0 * deg).epsilon(1e-5));
    CHECK(turning.frame().midpoint.yaw == doctest::Approx(turning.view_yaw()).epsilon(1e-5));

    // Recentre's reset: back to the configured scale, no offset, no turn.
    turning.set_world_scale(0.75f);
    turning.begin_scale_gesture();
    turning.scale_scene(2.0f);
    turning.end_scale_gesture();
    turning.move_viewer(0.3f, 0.2f, -0.1f);
    turning.turn_viewer(0.4f);
    turning.reset_scene_adjust();
    CHECK(turning.world_scale() == doctest::Approx(0.75f));
    CHECK(turning.scene_offset()[0] == doctest::Approx(0.0f));
    CHECK(turning.scene_offset()[1] == doctest::Approx(0.0f));
    CHECK(turning.scene_offset()[2] == doctest::Approx(0.0f));
    CHECK(turning.scene_turn() == doctest::Approx(0.0f));
    CHECK_FALSE(turning.scale_gesture());
}

TEST_CASE("the scene's median depth locates its centre") {
    // Median of five forward depths is the middle one.
    const float positions[15] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,
                                 3.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 9.0f};
    CHECK(splat::median_scene_depth(positions, 5) == doctest::Approx(3.0f));
    // Gaussians behind the capture plane are ignored, not counted as depth 0.
    const float behind[15] = {0.0f, 0.0f, -1.5f, 0.0f, 0.0f, -0.2f, 0.0f, 0.0f,
                              1.0f, 0.0f, 0.0f, 2.0f,  0.0f, 0.0f, 3.0f};
    CHECK(splat::median_scene_depth(behind, 5) == doctest::Approx(2.0f));
    // An empty scene falls back to the label's reading distance.
    CHECK(splat::median_scene_depth(nullptr, 0) == doctest::Approx(1.6f));
    const float all_behind[3] = {0.0f, 0.0f, -2.0f};
    CHECK(splat::median_scene_depth(all_behind, 1) == doctest::Approx(1.6f));
    // It sets the scene's centre point (0, 0, depth) and nothing else: no
    // gesture state, no offset, and the scale pivot is a point of the scene.
    splat::HmdView view;
    REQUIRE(view.place(head_with_eyes(make_head_pose(0.0f, 1.6f, 0.0f, 0.0f, 0.0f), 0.063f)));
    view.set_median_depth(2.4f);
    CHECK(view.scale_pivot()[2] == doctest::Approx(2.4f));
    CHECK(view.scale_pivot()[0] == doctest::Approx(0.0f));
    CHECK(view.scale_pivot()[1] == doctest::Approx(0.0f));
    CHECK(view.scene_offset()[0] == doctest::Approx(0.0f));
    CHECK(view.world_scale() == doctest::Approx(1.0f));
}

// The degree of freedom yaw and pitch cannot carry. A head pose that is only
// rolled must come back as a roll and nothing else, the placement must take the
// roll out (the photo is upright when it appears), and after that the roll must
// be the head's own, so rolling left shows the world turned to the right.
TEST_CASE("a rolled head is carried as roll, not dropped") {
    const double deg = 3.14159265358979323846 / 180.0;
    for (double roll_degrees : {-10.0, -35.0, 0.0, 12.0, 60.0}) {
        const splat::TrackedPose pose =
            make_head_pose(0.3f, 1.6f, 1.0f, 0.4, -0.15, roll_degrees * deg);
        CHECK(splat::hmd_roll(pose) == doctest::Approx(roll_degrees * deg).epsilon(1e-4));
        // A roll about the optical axis cannot move the view direction: the
        // forward vector and the yaw/pitch stay put, so the old pair is exactly
        // the part of the pose that was being used.
        float forward[3];
        splat::hmd_forward(pose.quaternion, forward);
        const double yaw = std::atan2(forward[0], forward[2]);
        const double pitch = std::asin(std::clamp(forward[1], -1.0f, 1.0f));
        CHECK(splat::hmd_yaw(pose) == doctest::Approx(yaw).epsilon(1e-4));
        CHECK(splat::hmd_pitch(pose) == doctest::Approx(pitch).epsilon(1e-4));
        // The full basis is the level one turned about forward by the roll:
        // the head's up axis must tilt, which is what a yaw/pitch-only view
        // threw away.
        float right[3], down[3], down_forward[3];
        splat::hmd_basis(pose.quaternion, right, down, down_forward);
        if (std::abs(roll_degrees) > 1.0) {
            CHECK(std::abs(right[1]) > 0.05f);  // the right axis gained a down/up part
        }
    }

    // Placement takes the head's roll out, like its pitch: the capture camera
    // is level, so the photo appears upright.
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    REQUIRE(view.place(head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.5, 0.12, -0.12), 0.066f)));
    CHECK(view.frame().midpoint.roll == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(view.head_roll() == doctest::Approx(-0.12).epsilon(1e-5));

    // Afterwards the roll is relative to the placed pose and fully carried: the
    // head rolling left (left ear down, negative) leaves the scene camera rolled
    // by the same negative angle, so the image content turns the other way.
    REQUIRE(view.update(head_with_eyes(
        make_head_pose(0.35f, 1.62f, 0.8f, 0.5, 0.12, -0.12 - 10.0 * deg), 0.066f)));
    CHECK(view.frame().midpoint.roll == doctest::Approx(-10.0 * deg).epsilon(1e-4));
    CHECK(view.scene_roll() == doctest::Approx(-10.0 * deg).epsilon(1e-4));
    // A head rolled the other way arrives with the opposite sign.
    REQUIRE(view.update(head_with_eyes(
        make_head_pose(0.35f, 1.62f, 0.8f, 0.5, 0.12, -0.12 + 10.0 * deg), 0.066f)));
    CHECK(view.frame().midpoint.roll == doctest::Approx(10.0 * deg).epsilon(1e-4));
}

// The headset run of 2026-10-26: the scene followed the head on roll and moved
// a little too on yaw and pitch. These three checks are the synthetic picture of
// what the user reported, measured against the projection rather than by eye:
// a head rotation of 10 degrees must move a world point by exactly the pixels
// the pinhole predicts (not a fraction, not the wrong way), and a head move must
// move near points more than far ones.
TEST_CASE("VR view: yaw, pitch and roll move the image as the projection predicts (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    splat::VulkanContext& context = *context_result.value();

    splat::HeadsetProjection projection;
    const splat::RendererOptions options = psvr2_options(projection);

    // One splat right and up of the optical axis, one metre in front: every
    // rotation of the head moves it in both image axes.
    const float world[3] = {0.35f, -0.20f, 1.0f};
    const splat::SplatScene scene = one_splat(world[2], 0.01f, world[0], world[1]);

    const double deg = 3.14159265358979323846 / 180.0;
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    REQUIRE(view.place(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 0.0, 0.0), 0.063f)));

    struct Shot {
        double x = 0.0;
        double y = 0.0;
        double predicted_x = 0.0;
        double predicted_y = 0.0;
    };
    // Renders the frame for `head` and returns the splat's centroid next to the
    // projection's own prediction for the same pose.
    std::string error;
    auto shoot = [&](const splat::HeadPoses& head, Shot& shot, const char* name) {
        REQUIRE(view.update(head));
        const splat::FrameView frame = view.frame();
        project_point(frame, projection, 0, world, shot.predicted_x, shot.predicted_y);
        splat::Image image;
        uint32_t visible = 0;
        REQUIRE_MESSAGE(render_single(context, scene, options, frame, image, visible, error), error);
        REQUIRE(visible == 1);
        // The left eye's half: the prediction is in one eye's pixels.
        REQUIRE(image_centroid(eye_crop(image, 0), shot.x, shot.y));
        MESSAGE(name << ": predicted (" << shot.predicted_x << ", " << shot.predicted_y
                     << "), rendered (" << shot.x << ", " << shot.y << ")");
        // No under- or over-rotation: the head's angle landed on the image
        // whole, not a part of it, and in the right direction.
        CHECK(std::abs(shot.x - shot.predicted_x) < 1.5);
        CHECK(std::abs(shot.y - shot.predicted_y) < 1.5);
    };

    Shot level, yawed, pitched, rolled;
    shoot(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 0.0, 0.0), 0.063f), level, "level");
    // Yaw right by 10 degrees: the content moves left by fx * tan(10 deg) plus
    // the eye's parallax term. A scaled or sign-flipped yaw fails here.
    shoot(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 10.0 * deg, 0.0), 0.063f), yawed,
          "yaw +10");
    CHECK(yawed.x < level.x - 100.0);
    // Pitch down by 10 degrees: the content moves up by fy * tan(10 deg).
    shoot(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 0.0, 10.0 * deg), 0.063f), pitched,
          "pitch +10");
    CHECK(pitched.y < level.y - 100.0);
    // Roll the head left by 10 degrees (the left ear going down): the image
    // content turns CLOCKWISE, by the roll angle - as the user sees it when the
    // world stays put. The old yaw/pitch-only view left it moving with the head.
    shoot(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 0.0, 0.0, -10.0 * deg), 0.063f), rolled,
          "roll left 10");
    const double cx_eye = projection.intrinsics.cx + projection.eye_shift[0];
    const double cy_eye = projection.intrinsics.cy;
    const auto turn = [&](const Shot& a, const Shot& b) {
        return (std::atan2(b.y - cy_eye, b.x - cx_eye) -
                std::atan2(a.y - cy_eye, a.x - cx_eye)) /
               deg;
    };
    const double turned = turn(level, rolled);
    const double predicted_turn = turn(Shot{level.predicted_x, level.predicted_y, 0.0, 0.0},
                                       Shot{rolled.predicted_x, rolled.predicted_y, 0.0, 0.0});
    MESSAGE("roll left 10: content turned " << turned << " deg (positive is clockwise), "
                                            << "the projection predicts " << predicted_turn);
    // Clockwise on screen is an increasing angle in (x right, y down) pixels.
    CHECK(turned > 0.0);
    CHECK(predicted_turn > 8.5);
    CHECK(predicted_turn < 11.5);
    CHECK(std::abs(turned - predicted_turn) < 1.5);
    // The roll is not a yaw or a pitch wearing its name: the point stays the
    // same distance from the eye's principal point.
    const double radius_level = std::hypot(level.x - cx_eye, level.y - cy_eye);
    const double radius_rolled = std::hypot(rolled.x - cx_eye, rolled.y - cy_eye);
    MESSAGE("roll left 10: radius " << radius_level << " -> " << radius_rolled << " px");
    CHECK(std::abs(radius_rolled - radius_level) < 2.0);
#endif
}

// The parallax the first headset session did not show: the scene must be a world
// the eyes move through, so a head step of 5 cm shifts a point 0.6 m away by
// fx * 0.05 / 0.6 = 59 px and a point 6 m away by 5.9 px, both to the left for a
// step to the right. A scene that follows the head shifts neither.
TEST_CASE("VR view: a 5 cm head step shifts near points left more than far (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    splat::VulkanContext& context = *context_result.value();

    splat::HeadsetProjection projection;
    const splat::RendererOptions options = psvr2_options(projection);

    const float near_world[3] = {0.0f, 0.0f, 0.6f};
    const float far_world[3] = {0.0f, 0.0f, 6.0f};
    const splat::SplatScene near_scene = one_splat(near_world[2], 0.01f);
    const splat::SplatScene far_scene = one_splat(far_world[2], 0.03f);

    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    REQUIRE(view.place(head_with_eyes(make_head_pose(0.0f, 0.0f, 0.0f, 0.0, 0.0), 0.063f)));

    std::string error;
    auto centroid_now = [&](const splat::SplatScene& scene, double& x, double& y) {
        splat::Image image;
        uint32_t visible = 0;
        REQUIRE_MESSAGE(render_single(context, scene, options, view.frame(), image, visible, error),
                        error);
        REQUIRE(visible == 1);
        REQUIRE(image_centroid(eye_crop(image, 0), x, y));
    };

    const splat::FrameView placed = view.frame();
    double near_x0, near_y0, far_x0, far_y0;
    centroid_now(near_scene, near_x0, near_y0);
    centroid_now(far_scene, far_x0, far_y0);

    // 5 cm along the head's own x axis (scene +x with no yaw): the eye midpoint
    // moves right, so the content moves left.
    REQUIRE(view.update(head_with_eyes(make_head_pose(0.05f, 0.0f, 0.0f, 0.0, 0.0), 0.063f)));
    const splat::FrameView stepped = view.frame();
    CHECK(stepped.midpoint.position[0] == doctest::Approx(0.05f).epsilon(1e-5));
    double near_x1, near_y1, far_x1, far_y1;
    centroid_now(near_scene, near_x1, near_y1);
    centroid_now(far_scene, far_x1, far_y1);

    double near_px, near_py, far_px, far_py;
    project_point(placed, projection, 0, near_world, near_px, near_py);
    const double near_predicted_start = near_px;
    project_point(stepped, projection, 0, near_world, near_px, near_py);
    const double near_predicted = near_px - near_predicted_start;
    project_point(placed, projection, 0, far_world, far_px, far_py);
    const double far_predicted_start = far_px;
    project_point(stepped, projection, 0, far_world, far_px, far_py);
    const double far_predicted = far_px - far_predicted_start;

    const double near_shift = near_x1 - near_x0;
    const double far_shift = far_x1 - far_x0;
    MESSAGE("5 cm right: near (0.6 m) " << near_shift << " px, projection says " << near_predicted
                                        << "; far (6 m) " << far_shift << " px, projection says "
                                        << far_predicted);
    // Leftward for a step right, and the projection's own amount (59 px near).
    CHECK(near_shift < -20.0);
    CHECK(far_shift < 0.0);
    CHECK(std::abs(near_shift - near_predicted) < 1.5);
    CHECK(std::abs(far_shift - far_predicted) < 1.5);
    // The sign AND the size of the parallax: the near point moves ten times as
    // far as the far one, which is what a 3D scene looks like from a moving head.
    CHECK(std::abs(near_shift) > 5.0 * std::abs(far_shift));
    CHECK(std::abs(near_y1 - near_y0) < 1.5);
    CHECK(std::abs(far_y1 - far_y0) < 1.5);
#endif
}

TEST_CASE("VR view places the scene at the head and follows it (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    const std::filesystem::path folder = scene_folder();
    if (!std::filesystem::exists(folder / "scene.psplat")) {
        MESSAGE("skipping: no scene at " << (folder / "scene.psplat"));
        return;
    }
    auto loaded = splat::read_scene_path(splat::Path((folder / "scene.psplat").string()));
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    REQUIRE(loaded.value().scene.count() > 100000);

    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    Rig rig;
    rig.context = context_result.take();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeWidth;
    options.eye_height = kEyeHeight;
    options.stereo = true;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    const splat::HeadsetProjection projection = splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);
    options.intrinsics = projection.intrinsics;
    options.eye_center_shift[0] = projection.eye_shift[0];
    options.eye_center_shift[1] = projection.eye_shift[1];
    const splat::Intrinsics& capture = loaded.value().meta.intrinsics;
    if (capture.fx > 0.0f && capture.width > 0) {
        options.capture_intrinsics = {capture.fx, capture.fy, capture.cx, capture.cy};
        options.capture_width = capture.width;
        options.capture_height = capture.height;
    }

    auto renderer_result = splat::SplatRenderer::create(*rig.context, options, loaded.value().scene);
    REQUIRE_MESSAGE(renderer_result.ok(), renderer_result.message());
    rig.renderer = renderer_result.take();

    auto image_result = splat::create_vulkan_color_image(
        *rig.context, options.target_format, rig.renderer->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image_result.ok(), image_result.message());
    rig.image = image_result.take();
    rig.target.image = rig.image.image;
    rig.target.view = rig.image.view;
    rig.target.extent = rig.renderer->target_extent();
    rig.target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    rig.target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    std::error_code filesystem_error;
    const std::filesystem::path out = output_folder();
    std::filesystem::create_directories(out, filesystem_error);
    REQUIRE_MESSAGE(!filesystem_error, "cannot create " << out << ": " << filesystem_error.message());
    const double ipd = 0.066f;

    // Warm-up: the title's first frame, the capture pose at the default IPD.
    // 1,179,631 splats were visible there on the console.
    splat::Image warm_image;
    std::string error;
    REQUIRE_MESSAGE(rig.render(splat::FrameView{}, warm_image, error), error);
    const uint32_t warm_visible = rig.renderer->last_visible_count();
    const splat::PassTimings warm_timings = rig.renderer->timings();
    MESSAGE("warm-up: " << warm_visible << " visible, project " << warm_timings.project_ms
                        << " sort " << warm_timings.sort_ms << " draw " << warm_timings.draw_ms
                        << " ms");
    CHECK(warm_visible > 1000000);

    // Placement at a real standing pose, head turned and slightly pitched, as
    // the first tracked pose is in practice.
    splat::HmdView view;
    view.set_comfort_radius(2.0f);
    const splat::HeadPoses placement =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.32, 0.08), ipd);
    REQUIRE(view.place(placement));
    const splat::FrameView placed = view.frame();
    MESSAGE("placement: position " << placed.midpoint.position[0] << " "
                                   << placed.midpoint.position[1] << " "
                                   << placed.midpoint.position[2] << ", yaw " << placed.midpoint.yaw
                                   << ", pitch " << placed.midpoint.pitch << ", ipd "
                                   << placed.ipd << ", fade " << placed.comfort_fade);

    splat::Image placement_image;
    REQUIRE_MESSAGE(rig.render(placed, placement_image, error), error);
    const uint32_t placement_visible = rig.renderer->last_visible_count();
    const splat::PassTimings placement_timings = rig.renderer->timings();
    MESSAGE("placement: " << placement_visible << " visible, project "
                          << placement_timings.project_ms << " sort " << placement_timings.sort_ms
                          << " draw " << placement_timings.draw_ms << " ms");
    CHECK_MESSAGE(placement_visible >= warm_visible * 95 / 100,
                  placement_visible << " visible at the placement pose, warm-up had "
                                    << warm_visible);

    // The desktop capture pose: the same renderer and eye offsets with the
    // identity camera. The placement frame must be that view (the expected IPD
    // shift is already in the eye offsets, so no further shift is applied).
    splat::FrameView desktop;
    desktop.midpoint = {};
    desktop.ipd = static_cast<float>(ipd);
    desktop.comfort_fade = 1.0f;
    splat::Image desktop_image;
    REQUIRE_MESSAGE(rig.render(desktop, desktop_image, error), error);

    const std::vector<splat::Image> placed_eyes = {
        eye_crop(placement_image, 0), eye_crop(placement_image, 1)};
    const std::vector<splat::Image> desktop_eyes = {
        eye_crop(desktop_image, 0), eye_crop(desktop_image, 1)};
    for (int eye = 0; eye < 2; ++eye) {
        const double quality = psnr(placed_eyes[eye], desktop_eyes[eye]);
        double photo_fraction = 0.0;
        const double photo_quality =
            psnr_over_photo(placed_eyes[eye], desktop_eyes[eye], desktop_eyes[eye],
                            photo_fraction);
        MESSAGE("eye " << eye << " vs desktop capture pose: PSNR " << quality << " dB (photo "
                       << photo_quality << " dB over " << (photo_fraction * 100.0)
                       << " % of the eye), mean " << mean_luminance(placed_eyes[eye]));
        CHECK(quality >= 20.0);
        CHECK(photo_quality >= 20.0);
        CHECK(photo_fraction > 0.05);
        // Not the backdrop or a black/white field: a real photo has mid-tones.
        CHECK(mean_luminance(placed_eyes[eye]) > 5.0);
        write_png(out / ("placement_eye" + std::to_string(eye) + ".png"), placed_eyes[eye]);
        write_png(out / ("desktop_eye" + std::to_string(eye) + ".png"), desktop_eyes[eye]);
    }

    // (b) Head yaw +30 deg right: the scene content moves left in both eyes.
    const double yaw = 30.0 * 3.14159265358979323846 / 180.0;
    const splat::HeadPoses yawed =
        head_with_eyes(make_head_pose(0.35f, 1.62f, 0.8f, 0.32 + yaw, 0.08), ipd);
    REQUIRE(view.update(yawed));
    // 30 degrees on the image: the placement pose's own pitch (0.08) tilts the
    // scene frame, so the exact composition lands at 30.5 degrees, not 30.
    CHECK(view.frame().midpoint.yaw == doctest::Approx(yaw).epsilon(0.02));
    splat::Image yawed_image;
    REQUIRE_MESSAGE(rig.render(view.frame(), yawed_image, error), error);
    for (int eye = 0; eye < 2; ++eye) {
        const splat::Image seen = eye_crop(yawed_image, eye);
        const Shift shift = find_shift(placed_eyes[eye], seen, 8, 80, 24);
        MESSAGE("yaw +30, eye " << eye << ": shift " << shift.dx << ", " << shift.dy
                                << " px, score " << shift.score);
        CHECK(shift.dx < -20);
        write_png(out / ("yaw30_eye" + std::to_string(eye) + ".png"), seen);
    }

    // Head up 10 cm: the scene content moves down.
    const splat::HeadPoses raised =
        head_with_eyes(make_head_pose(0.35f, 1.72f, 0.8f, 0.32 + yaw, 0.08), ipd);
    REQUIRE(view.update(raised));
    // 10 cm up, along the placement frame's y: cos(0.08) of it, so 9.97 cm, and
    // a 1.2 mm component onto the placement's z.
    CHECK(view.frame().midpoint.position[1] == doctest::Approx(-0.1f).epsilon(1e-2));
    splat::Image raised_image;
    REQUIRE_MESSAGE(rig.render(view.frame(), raised_image, error), error);
    for (int eye = 0; eye < 2; ++eye) {
        const splat::Image seen = eye_crop(raised_image, eye);
        const Shift shift = find_shift(eye_crop(yawed_image, eye), seen, 4, 24, 32);
        MESSAGE("up 10 cm, eye " << eye << ": shift " << shift.dx << ", " << shift.dy
                                 << " px, score " << shift.score);
        CHECK(shift.dy > 0);
        write_png(out / ("up10_eye" + std::to_string(eye) + ".png"), seen);
    }

    // (c) Draw time in the normal range: the failing run was 180-245 ms for
    // 4k-15k visible splats; a placed frame with the whole scene is a few
    // milliseconds per eye on the console and tens of milliseconds here.
    const splat::PassTimings& timings = rig.renderer->timings();
    MESSAGE("draw " << timings.draw_ms << " ms both eyes, project " << timings.project_ms
                    << " sort " << timings.sort_ms << ", " << rig.renderer->last_visible_count()
                    << " visible");
    CHECK(placement_timings.draw_ms < 150.0);
#endif
}

// Renderer robustness shared with the desktop: a splat a few centimetres from
// the eye must not paint the whole frame, and one inside the near distance must
// not paint at all. Without both, a camera inside the scene overdraws the eye
// with a handful of giant quads (the VR2 run's 180-245 ms draw).
TEST_CASE("near splats cannot fill the eye (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    splat::VulkanContext& context = *context_result.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeWidth;
    options.eye_height = kEyeHeight;
    options.stereo = false;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    // PSVR2's per-eye focal length, so the cap's pixel radius is exercised at
    // the size the headset actually renders.
    options.intrinsics = {711.0f, 770.0f, static_cast<float>(kEyeWidth) * 0.5f,
                          static_cast<float>(kEyeHeight) * 0.5f};

    std::string error;
    splat::Image image;
    uint32_t visible = 0;

    // 5 cm from the eye with a 5 cm Gaussian: 3 sigma is 3555 px wide at the
    // PSVR2 focal length, far past the 512 px cap. It must cover a bounded part
    // of the frame, not all of it.
    REQUIRE_MESSAGE(render_single(context, one_splat(0.06f, 0.05f), options,
                                  splat::FrameView{}, image, visible, error),
                    error);
    CHECK(visible == 1);
    const double capped = photo_coverage(image);
    MESSAGE("splat at 6 cm: coverage " << (capped * 100.0) << " %");
    CHECK(capped > 0.03);
    CHECK(capped < 0.30);

    // 3 cm is inside the 5 cm near distance: culled, backdrop only.
    REQUIRE_MESSAGE(render_single(context, one_splat(0.03f, 0.05f), options,
                                  splat::FrameView{}, image, visible, error),
                    error);
    CHECK(visible == 0);
    CHECK(photo_coverage(image) == 0.0);

    // A normal splat is untouched by the cap: 1 cm at 1 m is 8 px sigma, a
    // 24 px 3-sigma radius, so only a tiny disc is covered.
    REQUIRE_MESSAGE(render_single(context, one_splat(1.0f, 0.01f), options,
                                  splat::FrameView{}, image, visible, error),
                    error);
    CHECK(visible == 1);
    const double normal = photo_coverage(image);
    MESSAGE("splat at 1 m: coverage " << (normal * 100.0) << " %");
    CHECK(normal > 0.0);
    CHECK(normal < 0.01);
#endif
}
