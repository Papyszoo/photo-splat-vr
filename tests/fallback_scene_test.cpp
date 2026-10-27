// C4 fallback end to end: photo -> SplatScene sanity, plus the metric-scale
// knob. Needs a converted DA2 GGUF and a photo; without them the cases skip.
//
//   export SPLAT_DA2_GGUF=$SPLAT_DATA_DIR/weights/dav2s-f16.gguf
//   export SPLAT_FALLBACK_PHOTO=$SPLAT_DATA_DIR/golden/immichvr/4.jpg
//   build-*/tests/splat_tests -tc="fallback*"
//
// The backend runs on the CPU here on purpose: the assertions are about the
// scene contract (count, finite values, bounds, scale), not device numerics,
// and CI has no GPU.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/exif.h"
#include "jobs/image_io.h"
#include "recon/fallback/fallback_reconstructor.h"
#include "recon/reconstructor.h"
#include "render/project_math.h"

namespace {

namespace fs = std::filesystem;

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

struct FallbackFixture {
    std::unique_ptr<splat::FallbackReconstructor> backend;
    splat::Image image;
    float focal_px = 0.0f;
};

// Loads the photo and the model; returns false (with a MESSAGE) when the data
// is not available on this machine.
bool make_fixture(FallbackFixture* fixture, float assumed_depth) {
    const fs::path gguf = env_or_empty("SPLAT_DA2_GGUF");
    const fs::path photo = env_or_empty("SPLAT_FALLBACK_PHOTO");
    if (gguf.empty() || photo.empty()) {
        MESSAGE("skipping: set SPLAT_DA2_GGUF and SPLAT_FALLBACK_PHOTO");
        return false;
    }
    if (!fs::is_regular_file(gguf) || !fs::is_regular_file(photo)) {
        MESSAGE("skipping: " << gguf << " or " << photo << " is missing");
        return false;
    }
    splat::Result<splat::Image> decoded = splat::decode_image(photo);
    REQUIRE_MESSAGE(decoded.ok(), decoded.message());
    fixture->image = decoded.take();

    splat::Result<float> focal = splat::focal_px_from_jpeg(photo, fixture->image.width,
                                                           fixture->image.height);
    fixture->focal_px = focal.ok() ? focal.value() : 0.0f;

    splat::FallbackConfig config;
    config.assumed_scene_depth_m = assumed_depth;
    config.prefer_vulkan = false;
    fixture->backend.reset(new splat::FallbackReconstructor(config));
    const splat::Status loaded = fixture->backend->load(gguf);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    return true;
}

double median(std::vector<float> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// Alpha the fragment shader gives one splat at a pixel offset `(dx, dy)` from
// its centre, using the projected conic axes.
float gaussian_alpha(const splat::CovarianceAxes& axes, float dx, float dy, float opacity) {
    const float u = dx * axes.axis0[0] + dy * axes.axis0[1];
    const float v = dx * axes.axis1[0] + dy * axes.axis1[1];
    float exponent = 0.0f;
    if (axes.lambda0 > 1e-12f) exponent += (u * u) / (axes.lambda0 * axes.lambda0);
    if (axes.lambda1 > 1e-12f) exponent += (v * v) / (axes.lambda1 * axes.lambda1);
    return opacity * std::exp(-0.5f * exponent);
}

float linear_from_sh(float dc) {
    constexpr float kC0 = 0.28209479177387814f;
    const float encoded = std::min(std::max(0.5f + kC0 * dc, 0.0f), 1.0f);
    return encoded <= 0.04045f ? encoded / 12.92f
                              : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

TEST_CASE("fallback footprint covers the seam between two cells") {
    // Two horizontally adjacent cells at the capture pose. Their centres are one
    // cell apart, so the worst point between them is half a cell from each.
    constexpr float kCellPx = 2.5f;
    const float sigma_px = splat::fallback_footprint_sigma_px(kCellPx);
    CHECK(sigma_px > 0.7f * kCellPx);
    CHECK(sigma_px < 1.0f * kCellPx);

    // base_opacity 0.99, shader alpha exp(-0.5 (d/sigma)^2).
    const float distance = 0.5f * kCellPx;
    const float alpha = 0.99f * std::exp(-0.5f * (distance / sigma_px) * (distance / sigma_px));
    const float coverage = 1.0f - (1.0f - alpha) * (1.0f - alpha);
    CHECK(coverage > 0.95f);

    // The old sigma (half a cell) left a visible gap: that is the screen-door
    // grid this parameterisation replaces.
    const float old_sigma = 0.5f * kCellPx;
    const float old_alpha =
        0.99f * std::exp(-0.5f * (distance / old_sigma) * (distance / old_sigma));
    CHECK(1.0f - (1.0f - old_alpha) * (1.0f - old_alpha) < 0.9f);
}

TEST_CASE("fallback SH DC is the inverse of the renderer's colour mapping") {
    // The shader draws 0.5 + C0 * sh_dc, clamps, then decodes sRGB. Storing the
    // encoded photo colour must therefore round-trip exactly.
    constexpr float kC0 = 0.28209479177387814f;
    for (int value : {0, 1, 32, 64, 128, 200, 254, 255}) {
        const float dc =
            splat::fallback_sh_dc_from_srgb_value(static_cast<uint8_t>(value));
        const float encoded = 0.5f + kC0 * dc;
        CHECK(std::fabs(encoded - static_cast<float>(value) / 255.0f) < 1e-6f);
    }
    // And the renderer's sRGB decode lands on the linear-light colour that gets
    // blended: mid grey 128 is 0.2159 linear.
    const float encoded =
        0.5f + kC0 * splat::fallback_sh_dc_from_srgb_value(128);
    const float linear = encoded <= 0.04045f
                             ? encoded / 12.92f
                             : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    CHECK(std::fabs(linear - 0.2158605f) < 1e-5f);
}

TEST_CASE("fallback disocclusion band is the head-motion parallax") {
    // 1 m foreground, 4 m background, 1000 px focal: 10 cm of head travel
    // moves the two surfaces 100 px and 25 px, so 75 px of background is
    // revealed. The band is per side of the edge at that depth.
    CHECK(std::fabs(splat::fallback_disocclusion_band_px(1000.0f, 1.0f, 4.0f, 0.10f) - 75.0f) <
          1e-3f);
    // Half the motion (the +-5 cm poses) needs half the band.
    CHECK(std::fabs(splat::fallback_disocclusion_band_px(1000.0f, 1.0f, 4.0f, 0.05f) - 37.5f) <
          1e-3f);
    // A nearer background reveals less; no background at all reveals nothing.
    CHECK(splat::fallback_disocclusion_band_px(1000.0f, 1.0f, 2.0f, 0.10f) <
          splat::fallback_disocclusion_band_px(1000.0f, 1.0f, 4.0f, 0.10f));
    CHECK(splat::fallback_disocclusion_band_px(1000.0f, 2.0f, 2.0f, 0.10f) == 0.0f);
    CHECK(splat::fallback_disocclusion_band_px(1000.0f, 2.0f, 1.0f, 0.10f) == 0.0f);
}

TEST_CASE("fallback single-colour patch keeps its sub-pixel centre") {
    // A single bright patch on a flat background, at a deliberately
    // non-integer continuous-pixel centre. The grid saturates at one Gaussian
    // per pixel for 320x240, so any half-pixel convention error shows up
    // directly in the reconstructed patch's centroid.
    const fs::path gguf = env_or_empty("SPLAT_DA2_GGUF");
    if (gguf.empty() || !fs::is_regular_file(gguf)) {
        MESSAGE("skipping: set SPLAT_DA2_GGUF");
        return;
    }
    splat::FallbackConfig config;
    config.prefer_vulkan = false;
    config.assumed_scene_depth_m = 3.0f;
    splat::FallbackReconstructor backend(config);
    const splat::Status loaded = backend.load(gguf);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());

    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr double kPatchLeft = 152.25;
    constexpr double kPatchTop = 112.75;
    constexpr double kPatchSize = 16.0;
    constexpr uint8_t kBackground = 90;
    constexpr uint8_t kForeground = 255;
    splat::Image image;
    image.resize(kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const double width =
                std::min(x + 1.0, kPatchLeft + kPatchSize) - std::max<double>(x, kPatchLeft);
            const double height =
                std::min(y + 1.0, kPatchTop + kPatchSize) - std::max<double>(y, kPatchTop);
            const double coverage = std::max(0.0, width) * std::max(0.0, height);
            const uint8_t value = static_cast<uint8_t>(
                std::lround(kBackground + coverage * (kForeground - kBackground)));
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = pixel[1] = pixel[2] = value;
        }
    }

    splat::Progress progress;
    splat::ReconstructionInput input;
    input.rgb = image;  // no EXIF: the 30 mm default focal is used
    splat::Result<splat::SplatScene> result = backend.run(input, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SplatScene& scene = result.value();
    REQUIRE(scene.count() > 1000);

    splat::Result<float> focal_result =
        splat::focal_px_from_exif(splat::ExifFocal{}, kWidth, kHeight);
    REQUIRE(focal_result.ok());
    const float focal = focal_result.value();
    const float cx = 0.5f * static_cast<float>(kWidth);
    const float cy = 0.5f * static_cast<float>(kHeight);
    const float background_linear = [&] {
        const float encoded = static_cast<float>(kBackground) / 255.0f;
        return encoded <= 0.04045f ? encoded / 12.92f
                                   : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }();

    double sum = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (size_t index = 0; index < scene.count(); ++index) {
        const float z = scene.position[index * 3 + 2];
        if (!(z > 0.0f)) continue;
        const float encoded = std::min(
            std::max(0.5f + 0.28209479177387814f * scene.sh_dc[index * 3], 0.0f), 1.0f);
        const float linear = encoded <= 0.04045f
                                 ? encoded / 12.92f
                                 : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        const double weight = std::max(0.0, static_cast<double>(linear - background_linear));
        if (weight <= 0.02) continue;
        const double px = focal * scene.position[index * 3] / z + cx;
        const double py = focal * scene.position[index * 3 + 1] / z + cy;
        sum += weight;
        sum_x += weight * px;
        sum_y += weight * py;
    }
    REQUIRE(sum > 0.0);
    const double centre_x = sum_x / sum;
    const double centre_y = sum_y / sum;
    MESSAGE("patch centre " << centre_x << ", " << centre_y << " (expected "
            << kPatchLeft + 0.5 * kPatchSize << ", " << kPatchTop + 0.5 * kPatchSize << ")");
    CHECK(std::fabs(centre_x - (kPatchLeft + 0.5 * kPatchSize)) <= 0.25);
    CHECK(std::fabs(centre_y - (kPatchTop + 0.5 * kPatchSize)) <= 0.25);
}

TEST_CASE("fallback fills the background behind depth edges") {
    FallbackFixture fixture;
    if (!make_fixture(&fixture, 3.0f)) return;

    splat::Progress progress;
    splat::ReconstructionInput input;
    input.rgb = fixture.image;
    input.focal_px = fixture.focal_px > 0.0f ? std::optional<float>(fixture.focal_px) : std::nullopt;
    splat::Result<splat::SplatScene> result = fixture.backend->run(input, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SplatScene& scene = result.value();

    const float focal = fixture.focal_px > 0.0f
                            ? fixture.focal_px
                            : splat::focal_px_from_exif(splat::ExifFocal{}, fixture.image.width,
                                                        fixture.image.height).value();
    REQUIRE(focal > 0.0f);
    const float cx = 0.5f * static_cast<float>(fixture.image.width);
    const float cy = 0.5f * static_cast<float>(fixture.image.height);

    // Hidden fill shares the pixel position of the foreground surface in front
    // of it, at a depth at least 1.5x farther. Project everything and keep the
    // nearest depth per quarter-pixel cell; a splat whose position already
    // holds a much nearer one is a fill sample.
    // One-pixel bins: a fill sample sits on the same ray as the near surface
    // splat but at the cell's edge pixel, so its projection can differ from the
    // splat centroid by up to half a cell.
    auto key_of = [](float px, float py) {
        return (static_cast<uint64_t>(static_cast<int64_t>(std::lround(px)) + 100000)
                << 32) ^
               static_cast<uint32_t>(static_cast<int64_t>(std::lround(py)) + 100000);
    };
    std::unordered_map<uint64_t, float> nearest;
    nearest.reserve(scene.count() * 2);
    std::vector<uint64_t> keys(scene.count());
    for (size_t index = 0; index < scene.count(); ++index) {
        const float z = scene.position[index * 3 + 2];
        if (!(z > 0.0f)) continue;
        const float px = focal * scene.position[index * 3] / z + cx;
        const float py = focal * scene.position[index * 3 + 1] / z + cy;
        keys[index] = key_of(px, py);
        auto found = nearest.find(keys[index]);
        if (found == nearest.end() || z < found->second) nearest[keys[index]] = z;
    }
    size_t fill_count = 0;
    for (size_t index = 0; index < scene.count(); ++index) {
        const float z = scene.position[index * 3 + 2];
        const auto found = nearest.find(keys[index]);
        if (found != nearest.end() && z > 1.25f * found->second) ++fill_count;
    }
    const splat::FallbackFillStats& stats = fixture.backend->last_fill_stats();
    MESSAGE("hidden background samples: " << fill_count << " of " << scene.count()
            << " (producer fill samples " << stats.fill_samples << ")");
    CHECK(stats.fill_samples > 500);
    // The projection check is deliberately a subset: a fill sample is only
    // counted when its 1 px bin also holds a much nearer surface splat, so a
    // fill behind a moderate depth step is not counted twice.
    CHECK(fill_count > 100);
    CHECK(fill_count < scene.count() / 6);
    CHECK(scene.count() <= 500000);
}

TEST_CASE("fallback grid targets about half a million splats") {
    int columns = 0;
    int rows = 0;
    splat::fallback_grid(1280, 720, 500000, &columns, &rows);
    const double count = static_cast<double>(columns) * rows;
    CHECK(count > 400000.0);
    CHECK(count < 620000.0);
    // Grid aspect follows the photo.
    CHECK(std::fabs(static_cast<double>(columns) / rows - 1280.0 / 720.0) < 0.1);

    // A portrait 518x686 frame only has 355k pixels, so the grid saturates at
    // one splat per pixel instead of reaching 500k.
    splat::fallback_grid(518, 686, 500000, &columns, &rows);
    CHECK(columns == 518);
    CHECK(rows == 686);
}

TEST_CASE("fallback bilinear resize matches align_corners=True") {
    // 2x2 -> 3x3: corners are copied, the centre is the mean.
    const std::vector<float> source{0.0f, 1.0f, 2.0f, 3.0f};
    const std::vector<float> resized = splat::resize_bilinear_align_corners(source, 2, 2, 3, 3);
    REQUIRE(resized.size() == 9);
    CHECK(resized[0] == 0.0f);
    CHECK(resized[2] == 1.0f);
    CHECK(resized[6] == 2.0f);
    CHECK(resized[8] == 3.0f);
    CHECK(std::fabs(resized[4] - 1.5f) < 1e-6f);
}

TEST_CASE("fallback reconstructs a splat scene from a photo") {
    FallbackFixture fixture;
    if (!make_fixture(&fixture, 3.0f)) return;
    REQUIRE(fixture.backend->info().name == "depth-anything-v2-small");
    CHECK(fixture.backend->info().license == "Apache-2.0");

    splat::Progress progress;
    splat::ReconstructionInput input;
    input.rgb = fixture.image;
    input.focal_px = fixture.focal_px > 0.0f ? std::optional<float>(fixture.focal_px) : std::nullopt;
    splat::Result<splat::SplatScene> result = fixture.backend->run(input, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SplatScene& scene = result.value();

    REQUIRE(scene.consistent());
    REQUIRE(scene.count() > 100000);
    MESSAGE("photo " << fixture.image.width << "x" << fixture.image.height << " -> "
            << scene.count() << " splats in " << fixture.backend->last_run_seconds() << " s");

    // Grid reduction: fewer splats than pixels, and not absurdly few.
    const double pixels = static_cast<double>(fixture.image.width) * fixture.image.height;
    CHECK(static_cast<double>(scene.count()) < pixels);
    CHECK(static_cast<double>(scene.count()) > pixels * 0.1);

    // Every attribute finite and in range.
    std::vector<float> depths;
    depths.reserve(scene.count());
    for (size_t index = 0; index < scene.count(); ++index) {
        for (int axis = 0; axis < 3; ++axis) {
            const float position = scene.position[index * 3 + axis];
            CHECK(std::isfinite(position));
        }
        const float z = scene.position[index * 3 + 2];
        CHECK(z > 0.0f);
        CHECK(z <= 100.0f);  // config max_depth_m
        depths.push_back(z);
        for (int axis = 0; axis < 3; ++axis) {
            const float log_scale = scene.log_scale[index * 3 + axis];
            CHECK(std::isfinite(log_scale));
            CHECK(log_scale > -20.0f);
            CHECK(log_scale < 5.0f);
            const float sh = scene.sh_dc[index * 3 + axis];
            CHECK(std::isfinite(sh));
            CHECK(sh > -2.0f);
            CHECK(sh < 2.0f);
        }
        CHECK(std::fabs(scene.rotation[index * 4 + 0] - 1.0f) < 1e-6f);
        for (int axis = 1; axis < 4; ++axis) CHECK(scene.rotation[index * 4 + axis] == 0.0f);
        const float opacity = scene.opacity_logit[index];
        CHECK(std::isfinite(opacity));
        CHECK(opacity > -12.0f);
        CHECK(opacity < 12.0f);
    }

    const splat::Bounds bounds = scene.bounds();
    CHECK(bounds.valid);
    CHECK(bounds.min[2] > 0.0f);
    CHECK(bounds.max[2] <= 100.0f);
    // Scene centred near (0, 0, +z): the photo's centre ray is at x ~ 0.
    CHECK(std::fabs(bounds.min[0]) < 100.0f);  // 100 m cap, ~55 deg half-FOV

    // The assumed scene depth is the *median* depth by construction.
    const double median_depth = median(depths);
    MESSAGE("median depth " << median_depth << " m (assumed 3.0)");
    CHECK(std::fabs(median_depth - 3.0) < 0.6);

    // Intrinsics from EXIF (or the 30 mm default) are used for the footprint:
    // a landscape photo must have a wider extent in x than in y.
    const double extent_x = bounds.max[0] - bounds.min[0];
    const double extent_y = bounds.max[1] - bounds.min[1];
    if (fixture.image.width > fixture.image.height) CHECK(extent_x > extent_y);

    // Neighbouring Gaussians overlap at the capture pose. Project two
    // horizontally adjacent splats exactly like project.comp does and add their
    // alpha at the pixel midway between their centres.
    splat::Result<float> focal_result =
        splat::focal_px_from_exif(splat::ExifFocal{}, fixture.image.width, fixture.image.height);
    REQUIRE(focal_result.ok());
    const float focal = fixture.focal_px > 0.0f ? fixture.focal_px : focal_result.value();
    const float fx = focal;
    const float fy = focal;
    const float cx = 0.5f * static_cast<float>(fixture.image.width);
    const float cy = 0.5f * static_cast<float>(fixture.image.height);
    int grid_columns = 0;
    int grid_rows = 0;
    splat::fallback_grid(fixture.image.width, fixture.image.height, 500000, &grid_columns,
                         &grid_rows);
    const float cell_px = static_cast<float>(fixture.image.width) / grid_columns;

    std::vector<float> pair_coverage;
    pair_coverage.reserve(4096);
    for (size_t index = 0; index + 1 < scene.count() && pair_coverage.size() < 4096; ++index) {
        const float* a = &scene.position[index * 3];
        const float* b = &scene.position[(index + 1) * 3];
        const float z = 0.5f * (a[2] + b[2]);
        if (!(z > 0.0f) || std::fabs(a[2] - b[2]) / z > 0.02f) continue;  // depth step
        const float ax = fx * a[0] / a[2] + cx;
        const float ay = fy * a[1] / a[2] + cy;
        const float bx = fx * b[0] / b[2] + cx;
        const float by = fy * b[1] / b[2] + cy;
        const float dx = bx - ax;
        const float dy = by - ay;
        // Same row, one cell to the right.
        if (!(dx > 0.0f) || dx > 1.6f * cell_px || std::fabs(dy) > 0.3f * cell_px) continue;

        auto axes_of = [&](size_t splat) {
            const float* position = &scene.position[splat * 3];
            const float* scale = &scene.log_scale[splat * 3];
            const float* rotation = &scene.rotation[splat * 4];
            const splat::Mat3 cov3d = splat::gaussian_covariance_3d(
                splat::quaternion_to_rotation(rotation[0], rotation[1], rotation[2], rotation[3]),
                std::exp(scale[0]), std::exp(scale[1]), std::exp(scale[2]));
            return splat::covariance_axes(splat::project_covariance_2d(
                cov3d, position[0], position[1], position[2], fx, fy, 0.0f));
        };
        const splat::CovarianceAxes axes_a = axes_of(index);
        const splat::CovarianceAxes axes_b = axes_of(index + 1);
        const float opacity_a = 1.0f / (1.0f + std::exp(-scene.opacity_logit[index]));
        const float opacity_b = 1.0f / (1.0f + std::exp(-scene.opacity_logit[index + 1]));
        const float mx = 0.5f * (ax + bx);
        const float my = 0.5f * (ay + by);
        const float alpha_a = gaussian_alpha(axes_a, mx - ax, my - ay, opacity_a);
        const float alpha_b = gaussian_alpha(axes_b, mx - bx, my - by, opacity_b);
        pair_coverage.push_back(1.0f - (1.0f - alpha_a) * (1.0f - alpha_b));
    }
    REQUIRE(pair_coverage.size() > 100);
    std::sort(pair_coverage.begin(), pair_coverage.end());
    const float worst_pair = pair_coverage[pair_coverage.size() / 20];  // 5th percentile
    const double median_pair = median(pair_coverage);
    MESSAGE("neighbour seam coverage: median " << median_pair << ", 5th percentile "
            << worst_pair << " over " << pair_coverage.size() << " pairs, cell "
            << cell_px << " px");
    CHECK(median_pair > 0.95);
    CHECK(worst_pair > 0.85);
}

TEST_CASE("fallback assumed scene depth scales the whole scene") {
    FallbackFixture near_fixture;
    FallbackFixture far_fixture;
    if (!make_fixture(&near_fixture, 2.0f)) return;
    if (!make_fixture(&far_fixture, 6.0f)) return;

    splat::Progress progress;
    splat::ReconstructionInput near_input;
    near_input.rgb = near_fixture.image;
    near_input.focal_px = near_fixture.focal_px > 0.0f ? std::optional<float>(near_fixture.focal_px)
                                                       : std::nullopt;
    splat::Result<splat::SplatScene> near_scene = near_fixture.backend->run(near_input, progress);
    REQUIRE_MESSAGE(near_scene.ok(), near_scene.message());

    splat::ReconstructionInput far_input;
    far_input.rgb = far_fixture.image;
    far_input.focal_px = far_fixture.focal_px > 0.0f ? std::optional<float>(far_fixture.focal_px)
                                                     : std::nullopt;
    splat::Result<splat::SplatScene> far_scene = far_fixture.backend->run(far_input, progress);
    REQUIRE_MESSAGE(far_scene.ok(), far_scene.message());

    std::vector<float> near_depths;
    near_depths.reserve(near_scene.value().count());
    for (size_t index = 0; index < near_scene.value().count(); ++index) {
        near_depths.push_back(near_scene.value().position[index * 3 + 2]);
    }
    std::vector<float> far_depths;
    far_depths.reserve(far_scene.value().count());
    for (size_t index = 0; index < far_scene.value().count(); ++index) {
        far_depths.push_back(far_scene.value().position[index * 3 + 2]);
    }
    const double ratio = median(far_depths) / median(near_depths);
    MESSAGE("median depth ratio 6 m / 2 m = " << ratio);
    CHECK(std::fabs(ratio - 3.0) < 0.6);
}

TEST_CASE("fallback fill takes colour and depth only from the far side") {
    // Two fronto-parallel planes with distinct colours: the near half is red at
    // 1 m, the far half blue at 4 m. Everything the fill adds must be blue and
    // at least at the far depth; a single near-side sample would show up as a
    // red channel above the blue one.
    constexpr int kWidth = 64;
    constexpr int kHeight = 48;
    constexpr float kNearZ = 1.0f;
    constexpr float kFarZ = 4.0f;
    splat::Image image;
    image.resize(kWidth, kHeight);
    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t index = static_cast<size_t>(y) * kWidth + x;
            uint8_t* pixel = image.pixel(x, y);
            if (x < kWidth / 2) {
                depth[index] = kNearZ;
                pixel[0] = 210;
                pixel[1] = 30;
                pixel[2] = 30;
            } else {
                depth[index] = kFarZ;
                pixel[0] = 30;
                pixel[1] = 30;
                pixel[2] = 210;
            }
        }
    }

    splat::FallbackConfig config;
    config.target_splats = static_cast<size_t>(kWidth) * kHeight;
    config.prefer_vulkan = false;
    constexpr float kFocal = 100.0f;
    splat::Progress progress;
    splat::Result<splat::SplatScene> without =
        splat::fallback_scene_from_depth(depth, image, kFocal, config, 0.0f, progress);
    REQUIRE_MESSAGE(without.ok(), without.message());
    splat::FallbackFillStats stats;
    splat::Result<splat::SplatScene> with =
        splat::fallback_scene_from_depth(depth, image, kFocal, config, 0.10f, progress, &stats);
    REQUIRE_MESSAGE(with.ok(), with.message());
    REQUIRE(with.value().count() > without.value().count());
    // The producer's own ghost check sees the red/blue edge every sample came from.
    CHECK(stats.fill_samples == with.value().count() - without.value().count());
    CHECK(stats.ghost_samples == 0);

    const float near_red = linear_from_sh(
        splat::fallback_sh_dc_from_srgb_value(210));
    const float far_blue = linear_from_sh(
        splat::fallback_sh_dc_from_srgb_value(210));
    size_t fill_count = 0;
    for (size_t index = without.value().count(); index < with.value().count(); ++index) {
        const splat::SplatScene& scene = with.value();
        const float r = linear_from_sh(scene.sh_dc[index * 3 + 0]);
        const float g = linear_from_sh(scene.sh_dc[index * 3 + 1]);
        const float b = linear_from_sh(scene.sh_dc[index * 3 + 2]);
        // Blue far side: the blue channel is strongly dominant.
        CHECK(b > r);
        CHECK(b > g);
        CHECK(std::fabs(b - far_blue) < 0.05f);
        CHECK(std::fabs(r - near_red) > 0.2f);
        // Pushed strictly behind the far-side depth.
        CHECK(scene.position[index * 3 + 2] > kFarZ);
        ++fill_count;
    }
    MESSAGE("fill splats: " << fill_count << " (surface " << without.value().count() << ")");
    CHECK(fill_count > 0);
}

TEST_CASE("fallback snaps the network's smooth silhouette ramp onto its two surfaces") {
    // The same red-near / blue-far planes, but with the depth step smeared over
    // a 6 px ramp the way Depth Anything's upsampled output smears it, and the
    // colour edge sharp in the middle. A ramp pixel left at its own depth is a
    // splat floating between the planes; under a head move those splats
    // spread across the revealed gap as streaks of the near colour. Every
    // splat must sit on one of the two planes, red ones on the near plane.
    constexpr int kWidth = 64;
    constexpr int kHeight = 48;
    constexpr int kRamp = 6;
    constexpr float kNearZ = 1.0f;
    constexpr float kFarZ = 4.0f;
    splat::Image image;
    image.resize(kWidth, kHeight);
    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t index = static_cast<size_t>(y) * kWidth + x;
            // Linear in inverse depth across the ramp, like the network's map.
            const float t = std::min(std::max((x - (kWidth - kRamp) / 2 + 0.5f) / kRamp, 0.0f),
                                     1.0f);
            depth[index] = 1.0f / ((1.0f - t) / kNearZ + t / kFarZ);
            uint8_t* pixel = image.pixel(x, y);
            const bool red = x < kWidth / 2;
            pixel[0] = red ? 210 : 30;
            pixel[1] = 30;
            pixel[2] = red ? 30 : 210;
        }
    }

    splat::FallbackConfig config;
    config.target_splats = static_cast<size_t>(kWidth) * kHeight;
    // The network ran at half resolution: the ramp search reaches 16 px.
    config.input_size = kHeight / 2;
    config.prefer_vulkan = false;
    constexpr float kFocal = 200.0f;
    splat::Progress progress;
    splat::Result<splat::SplatScene> result =
        splat::fallback_scene_from_depth(depth, image, kFocal, config, 0.10f, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::SplatScene& scene = result.value();
    size_t floating = 0;
    size_t red_far = 0;
    for (size_t index = 0; index < scene.count(); ++index) {
        const float z = scene.position[index * 3 + 2];
        const bool near = std::fabs(z - kNearZ) < 0.02f * kNearZ;
        const bool far = z > 0.98f * kFarZ && z < 1.02f * kFarZ;
        if (!near && !far) ++floating;
        const float r = linear_from_sh(scene.sh_dc[index * 3 + 0]);
        const float b = linear_from_sh(scene.sh_dc[index * 3 + 2]);
        if (r > b && !near) ++red_far;
    }
    MESSAGE("splats " << scene.count() << ", floating " << floating << ", red off the near plane "
                      << red_far);
    CHECK(floating == 0);
    CHECK(red_far == 0);
}

TEST_CASE("fallback leaves a one-cell-wide rod without fill") {
    // A 1-cell-wide near rod on a far background. It is thinner than three
    // cells, so it keeps the single layer it has without the fill pass: same
    // splat count, same colours, no extra splats on or behind it.
    constexpr int kWidth = 48;
    constexpr int kHeight = 32;
    constexpr float kRodZ = 1.0f;
    constexpr float kBackgroundZ = 5.0f;
    splat::Image image;
    image.resize(kWidth, kHeight);
    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t index = static_cast<size_t>(y) * kWidth + x;
            uint8_t* pixel = image.pixel(x, y);
            if (x == kWidth / 2) {
                depth[index] = kRodZ;
                pixel[0] = 20;
                pixel[1] = 20;
                pixel[2] = 20;
            } else {
                depth[index] = kBackgroundZ;
                pixel[0] = 185;
                pixel[1] = 195;
                pixel[2] = 205;
            }
        }
    }

    splat::FallbackConfig config;
    config.target_splats = static_cast<size_t>(kWidth) * kHeight;
    config.prefer_vulkan = false;
    constexpr float kFocal = 100.0f;
    splat::Progress progress;
    splat::Result<splat::SplatScene> without =
        splat::fallback_scene_from_depth(depth, image, kFocal, config, 0.0f, progress);
    REQUIRE_MESSAGE(without.ok(), without.message());
    splat::Result<splat::SplatScene> with =
        splat::fallback_scene_from_depth(depth, image, kFocal, config, 0.10f, progress);
    REQUIRE_MESSAGE(with.ok(), with.message());

    CHECK(with.value().count() == without.value().count());
    REQUIRE(with.value().count() == without.value().count());
    const splat::SplatScene& a = without.value();
    const splat::SplatScene& b = with.value();
    for (size_t index = 0; index < a.count(); ++index) {
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(a.position[index * 3 + axis] == b.position[index * 3 + axis]);
            CHECK(a.sh_dc[index * 3 + axis] == b.sh_dc[index * 3 + axis]);
        }
        CHECK(a.opacity_logit[index] == b.opacity_logit[index]);
    }
}

TEST_CASE("fallback fill colour comes from the far side of its edge") {
    // The producer judges every fill sample against the near and far mean
    // colours of the edge it was built from. On a real photo a few percent are
    // far-side texture closer to the near mean (0-4 % on the five test photos);
    // a fill that read the near side would be judged a ghost on most edges.
    FallbackFixture fixture;
    if (!make_fixture(&fixture, 3.0f)) return;
    splat::Progress progress;
    splat::ReconstructionInput input;
    input.rgb = fixture.image;
    input.focal_px = fixture.focal_px > 0.0f ? std::optional<float>(fixture.focal_px)
                                             : std::nullopt;
    splat::Result<splat::SplatScene> result = fixture.backend->run(input, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const splat::FallbackFillStats& stats = fixture.backend->last_fill_stats();
    MESSAGE("fill colour ghosts: " << stats.ghost_samples << " of " << stats.fill_samples);
    CHECK(stats.fill_samples > 0);
    CHECK(static_cast<double>(stats.ghost_samples) < 0.05 * stats.fill_samples);
}

TEST_CASE("external depth maps go through the fallback's own metric mapping") {
    // The external-depth CLI (tools/depth_scene) feeds a saved map to
    // fallback_scene_from_depth. Relative inverse depth (DA2, DA3 metric 0)
    // must land on the same metric depths run() would give, and a metric map
    // (MoGe-2, DA3 metric) must be clamped, with a masked sky at the far plane.
    splat::FallbackConfig config;
    config.assumed_scene_depth_m = 4.0f;
    config.max_depth_m = 50.0f;
    config.target_splats = 1000;

    const std::vector<float> disparity = {0.5f, 1.0f, 2.0f, 0.0f, -1.0f};
    const std::vector<float> metric = splat::fallback_metric_depth_from_disparity(disparity, config);
    REQUIRE(metric.size() == disparity.size());
    // The median positive disparity (1.0) is the assumed scene depth.
    CHECK(metric[1] == 4.0f);
    CHECK(metric[0] == 8.0f);
    CHECK(metric[2] == 2.0f);
    // Zero and negative disparity are the far plane, never a division blow-up.
    CHECK(metric[3] == 50.0f);
    CHECK(metric[4] == 50.0f);

    const std::vector<float> metres = {0.01f, 2.5f, 1e6f,
                                       std::numeric_limits<float>::infinity(),
                                       std::numeric_limits<float>::quiet_NaN()};
    const std::vector<float> clean = splat::fallback_metric_depth_from_metres(metres, config);
    REQUIRE(clean.size() == metres.size());
    CHECK(clean[0] == 0.05f);   // kMinDepth
    CHECK(clean[1] == 2.5f);    // untouched
    CHECK(clean[2] == 50.0f);   // clamped to max_depth_m
    CHECK(clean[3] == 50.0f);   // masked sky -> far plane
    CHECK(clean[4] == 50.0f);   // NaN -> far plane

    // DA3-Small predicts depth up to scale: the median value becomes the
    // assumed scene depth and everything scales with it.
    const std::vector<float> relative = {0.5f, 1.0f, 2.0f, 4.0f};
    const std::vector<float> scaled =
        splat::fallback_metric_depth_from_relative_depth(relative, config);
    REQUIRE(scaled.size() == relative.size());
    // Median of (0.5, 1, 2, 4) is the upper middle (2.0, nth_element), so the
    // scale is 4.0 / 2.0 and nothing is clamped at this small size.
    CHECK(scaled[1] == 2.0f);
    CHECK(scaled[3] == 8.0f);
}
