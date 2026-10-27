// Stage 6 parity: the composer's activations (080_*), the unprojection
// (090_*) and the reference .ply.
//
// Two things are deliberately *not* compared element-wise:
//   - the quaternion sign: q and -q are the same rotation and the reference
//     picks the sign with scipy's SVD, which is unstable even between two
//     PyTorch backends (docs/BACKLOG.md). The rotation is compared through the
//     covariance R diag(sigma^2) R^T, which is what rendering uses.
//   - the axis order inside a Gaussian: the reference's SVD emits the singular
//     values in descending order, and this port reproduces that order, but a
//     near-degenerate covariance can still swap two equal axes. The covariance
//     comparison covers that too.
//
//   build-F8-sharp/tests/splat_tests -tc="sharp composer*"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "common/exif.h"
#include "recon/sharp/sharp_composer.h"
#include "scene/ply.h"
#include "sharp_golden.h"

namespace {

using namespace splat;
using namespace splat::sharp_test;

struct Expectation {
    const char* name;
    const char* file;
    std::vector<int64_t> ne;
    // fp32 gates for the composer's own outputs: host fp64 arithmetic on the
    // reference's inputs, so the spec's 1e-3 maximum relative error holds
    // everywhere. Numbers are the measured worst case over photo01..03 plus
    // headroom (mean 6.8e-3 m on splats up to 300 m is 2.0e-4 relative).
    double max_abs;
    double max_rel;
};

// (1, N, 3) / (1, N, 4) / (1, N) dumps, N = 2 layers * 768^2 Gaussians in
// (layer, y, x) order. The ggml ne is the transposed shape; the linear element
// order is the same, so the comparison is element-wise.
const Expectation kComposerStages[] = {
    {"composer_mean", "080_composer_mean.npy", {3, 1179648, 1, 1}, 1e-2, 1e-3},
    {"composer_singular", "080_composer_singular.npy", {3, 1179648, 1, 1}, 2e-4, 1e-3},
    {"composer_quaternion", "080_composer_quaternion.npy", {4, 1179648, 1, 1}, 1e-6, 1e-3},
    {"composer_color", "080_composer_color.npy", {3, 1179648, 1, 1}, 1e-5, 1e-3},
    {"composer_opacity", "080_composer_opacity.npy", {1179648, 1, 1, 1}, 1e-6, 1e-3},
};

// Covariance of one Gaussian from its quaternion and singular values, fp64.
std::array<double, 9> covariance(const float quaternion[4], const float singular[3]) {
    double norm = 0.0;
    for (int index = 0; index < 4; ++index) norm += quaternion[index] * quaternion[index];
    norm = std::sqrt(norm);
    const double w = quaternion[0] / norm;
    const double x = quaternion[1] / norm;
    const double y = quaternion[2] / norm;
    const double z = quaternion[3] / norm;
    const double rotation[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                                2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    std::array<double, 9> result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                sum += rotation[row * 3 + axis] * singular[axis] * singular[axis] *
                       rotation[column * 3 + axis];
            }
            result[static_cast<size_t>(row) * 3 + column] = sum;
        }
    }
    return result;
}

TEST_CASE("sharp composer parity") {
    // The composer is host arithmetic on golden inputs: no GGUF is loaded here,
    // so only the golden data has to be present.
    if (golden_root().empty() || !fs::is_directory(golden_root())) {
        MESSAGE("skipping composer: set SPLAT_SHARP_GOLDEN to the golden/ folder");
        return;
    }
    const std::vector<Photo> found = photos(golden_root());
    REQUIRE_MESSAGE(!found.empty(), "no golden photos under " << golden_root());

    for (const Photo& photo : found) {
        MESSAGE("photo " << photo.id);
        std::string error;
        PhotoMeta meta;
        REQUIRE_MESSAGE(read_photo_meta(photo, &meta, &error), error);

        // ---- inputs: the reference image, monodepth and delta values --------
        std::vector<float> image;
        std::vector<float> monodepth;
        std::vector<float> delta;
        REQUIRE_MESSAGE(read_golden(photo, "050_gaussian_decoder_in.npy", &image, &error), error);
        REQUIRE_MESSAGE(read_golden(photo, "041_monodepth.npy", &monodepth, &error), error);
        REQUIRE_MESSAGE(read_golden(photo, "070_delta_values.npy", &delta, &error), error);
        REQUIRE(delta.size() == 14u * 2u * 768 * 768);

        const float disparity_factor = meta.focal_px / static_cast<float>(meta.width);
        Result<SharpBaseValues> base =
            sharp_initialize(image.data(), 1536, monodepth.data(), meta.layers);
        REQUIRE_MESSAGE(base.ok(), base.message());

        // ---- composer -------------------------------------------------------
        Result<SharpComposedGaussians> composed = sharp_compose(base.value(), delta.data());
        REQUIRE_MESSAGE(composed.ok(), composed.message());
        CHECK(composed.value().count == 1179648);
        MESSAGE("  composer count " << composed.value().count);

        struct ComposedArray {
            const char* name;
            const std::vector<float>* values;
        };
        const ComposedArray composed_arrays[] = {
            {"composer_mean", &composed.value().mean},
            {"composer_singular", &composed.value().singular},
            {"composer_quaternion", &composed.value().quaternion},
            {"composer_color", &composed.value().color},
            {"composer_opacity", &composed.value().opacity},
        };
        for (const Expectation& expectation : kComposerStages) {
            const std::vector<float>* values = nullptr;
            for (const ComposedArray& array : composed_arrays) {
                if (std::string(expectation.name) == array.name) values = array.values;
            }
            REQUIRE(values != nullptr);
            const parity::Stats stats =
                parity::compare_npy((photo.folder / expectation.file).string(), expectation.ne,
                                    values->data());
            print_stats(expectation.name, stats, "");
            REQUIRE_MESSAGE(stats.ok, stats.error);
            // The composer is host arithmetic on the reference's own inputs, so
            // the spec's fp32 gate applies as written: 1e-3 maximum relative
            // error, plus the percentile form the other stage tests use.
            CHECK_MESSAGE(stats.max_rel <= expectation.max_rel,
                          expectation.name << " max rel " << stats.max_rel);
            CHECK_MESSAGE(stats.max_abs <= expectation.max_abs,
                          expectation.name << " max abs " << stats.max_abs);
            CHECK_MESSAGE(stats.p99_rel <= 1e-3, expectation.name << " p99 rel " << stats.p99_rel);
            CHECK_MESSAGE(stats.p999_rel <= 2e-3,
                          expectation.name << " p99.9 rel " << stats.p999_rel);
            CHECK_MESSAGE(stats.cosine >= 0.99999, expectation.name << " cosine " << stats.cosine);
        }

        // ---- intrinsics -----------------------------------------------------
        // The unprojection is M = inv(ndc(1536) @ K_resized), where K_resized is
        // the photo's K scaled to the internal 1536 grid. M is diagonal with
        // W/(2 f) and H/(2 f) only because the principal point lands on the
        // centre of that grid. Both factors are checked against the reference's
        // own 090_intrinsics_resized.npy rather than against a copy of the
        // formula, and the photos are non-square, so an x/y swap or a mix-up of
        // the original size with the 1536 grid cannot hide.
        {
            std::vector<float> k_resized;
            REQUIRE_MESSAGE(read_golden(photo, "090_intrinsics_resized.npy", &k_resized, &error),
                            error);
            REQUIRE(k_resized.size() == 16);
            const double internal = 1536.0;
            const double kx = k_resized[0];
            const double ky = k_resized[5];
            const double cx = k_resized[2];
            const double cy = k_resized[6];
            REQUIRE(meta.width != meta.height);
            CHECK_MESSAGE(std::fabs(cx - internal / 2.0) <= 1e-3,
                          "the reference's principal point x is not the grid centre: " << cx);
            CHECK_MESSAGE(std::fabs(cy - internal / 2.0) <= 1e-3,
                          "the reference's principal point y is not the grid centre: " << cy);
            const double reference_mx = internal / (2.0 * kx);
            const double reference_my = internal / (2.0 * ky);
            const double ours_mx = meta.width / (2.0 * meta.focal_px);
            const double ours_my = meta.height / (2.0 * meta.focal_px);
            // The .psplat scene's positions are M times the composed means, i.e.
            // exactly this scale; the position comparison above is the end-to-end
            // check of it.
            CHECK_MESSAGE(std::fabs(reference_mx - ours_mx) <= 1e-6 * reference_mx,
                          "x scale " << ours_mx << " vs reference " << reference_mx);
            CHECK_MESSAGE(std::fabs(reference_my - ours_my) <= 1e-6 * reference_my,
                          "y scale " << ours_my << " vs reference " << reference_my);
            MESSAGE("  intrinsics: " << meta.width << "x" << meta.height << ", f " << meta.focal_px
                    << " px, M = diag(" << ours_mx << ", " << ours_my << ", 1)");
        }

        // ---- focal length provenance ---------------------------------------
        // None of the golden photos carries an EXIF focal length, so this is the
        // "no EXIF" case the spec's C2 rule covers: SHARP's own 30 mm default
        // must be what the reference used, and the port reproduces it through
        // focal_px_from_exif().
        {
            const fs::path jpeg = photo.folder.parent_path() / "inputs" / (photo.id + ".jpg");
            if (fs::is_regular_file(jpeg)) {
                const Result<ExifFocal> exif = parse_jpeg_exif_focal(jpeg);
                REQUIRE_MESSAGE(exif.ok(), exif.message());
                const bool absent = !exif.value().focal_length_mm &&
                                    !exif.value().focal_length_35mm;
                CHECK_MESSAGE(absent, "this photo has EXIF focal data; the no-EXIF default is not what "
                                      "the reference used");
                const float expected =
                    focal_px_from_mm(kDefaultFocal35mm, meta.width, meta.height);
                CHECK_MESSAGE(std::fabs(expected - meta.focal_px) <= 1e-3f * expected,
                              "no-EXIF default focal " << expected << " vs reference "
                                                       << meta.focal_px);
                const Result<float> ours =
                    focal_px_from_exif(ExifFocal{}, meta.width, meta.height);
                REQUIRE_MESSAGE(ours.ok(), ours.message());
                CHECK_MESSAGE(std::fabs(ours.value() - meta.focal_px) <= 1e-3f * meta.focal_px,
                              "port's no-EXIF focal " << ours.value() << " vs reference "
                                                      << meta.focal_px);
            }
        }

        // ---- unprojection ---------------------------------------------------
        Result<SplatScene> scene =
            sharp_unproject(composed.value(), meta.focal_px, meta.width, meta.height);
        REQUIRE_MESSAGE(scene.ok(), scene.message());
        REQUIRE(scene.value().count() == composed.value().count);
        CHECK(scene.value().consistent());

        SplatScene reference;
        const Status imported = import_ply(photo.folder / "reference.ply", reference);
        REQUIRE_MESSAGE(imported.ok(), imported.message());
        REQUIRE(reference.count() == scene.value().count());

        // Position, log-scale and SH DC are directly comparable: the .ply stores
        // the same 14 values .psplat does.
        struct Attribute {
            const char* name;
            const std::vector<float>* ours;
            const std::vector<float>* theirs;
            int stride;
            double max_abs_gate;
            double max_rel_gate;
        };
        // Positions reach hundreds of metres (photo02's far background), where
        // the reference's own fp32 arithmetic moves a value by ~2e-5 relative;
        // the gate is relative for those. The log-scale and SH DC values are
        // O(1), so those are gated on absolute error.
        const Attribute attributes[] = {
            {"position", &scene.value().position, &reference.position, 3, 1e-2, 1e-3},
            {"log_scale", &scene.value().log_scale, &reference.log_scale, 3, 1e-3, 1e-2},
            {"sh_dc", &scene.value().sh_dc, &reference.sh_dc, 3, 1e-3, 1e-2},
        };
        for (const Attribute& attribute : attributes) {
            double max_abs = 0.0;
            double max_rel = 0.0;
            size_t differing = 0;
            for (size_t index = 0; index < reference.count(); ++index) {
                for (int component = 0; component < attribute.stride; ++component) {
                    const size_t at = index * attribute.stride + component;
                    const double theirs = (*attribute.theirs)[at];
                    const double ours = (*attribute.ours)[at];
                    if (!std::isfinite(theirs)) continue;
                    const double difference = std::fabs(ours - theirs);
                    if (difference > 0.0) ++differing;
                    max_abs = std::max(max_abs, difference);
                    if (std::fabs(theirs) > parity::kRelativeFloor) {
                        max_rel = std::max(max_rel, difference / std::fabs(theirs));
                    }
                }
            }
            char line[192];
            std::snprintf(line, sizeof(line),
                          "  %-14s vs ply: differing=%-8zu max_abs=%.3e max_rel=%.3e",
                          attribute.name, differing, max_abs, max_rel);
            MESSAGE(line);
            CHECK_MESSAGE(max_abs <= attribute.max_abs_gate,
                          attribute.name << " max abs " << max_abs);
            CHECK_MESSAGE(max_rel <= attribute.max_rel_gate,
                          attribute.name << " max rel " << max_rel);
        }

        // Opacity is compared as alpha, not as its logit: the logit is unbounded
        // near saturation (the .ply carries +inf for alpha == 1) and the useful
        // quantity is the alpha the renderer blends.
        {
            double max_abs = 0.0;
            size_t finite_reference = 0;
            size_t saturated = 0;
            for (size_t index = 0; index < reference.count(); ++index) {
                const double theirs = reference.opacity_logit[index];
                const double ours = scene.value().opacity_logit[index];
                const double ours_alpha = 1.0 / (1.0 + std::exp(-ours));
                if (!std::isfinite(theirs)) {
                    // The reference wrote +inf; the port clamps to 1 - 1e-4.
                    ++saturated;
                    max_abs = std::max(max_abs, std::fabs(ours_alpha - 1.0));
                    continue;
                }
                ++finite_reference;
                const double theirs_alpha = 1.0 / (1.0 + std::exp(-theirs));
                max_abs = std::max(max_abs, std::fabs(ours_alpha - theirs_alpha));
            }
            char line[192];
            std::snprintf(line, sizeof(line),
                          "  %-14s vs ply: finite=%zu saturated=%zu max_abs(alpha)=%.3e",
                          "opacity", finite_reference, saturated, max_abs);
            MESSAGE(line);
            CHECK_MESSAGE(max_abs <= 5e-3, "opacity max abs " << max_abs);
        }

        // Rotation and scale together, through the covariance, because the
        // quaternion sign is not part of the parameterisation.
        double max_covariance = 0.0;
        double max_covariance_rel = 0.0;
        double max_axis_swap = 0.0;
        for (size_t index = 0; index < reference.count(); ++index) {
            std::array<float, 3> sigma_reference{};
            std::array<float, 3> sigma_actual{};
            for (int axis = 0; axis < 3; ++axis) {
                sigma_reference[axis] = static_cast<float>(std::exp(reference.log_scale[index * 3 + axis]));
                sigma_actual[axis] = static_cast<float>(std::exp(scene.value().log_scale[index * 3 + axis]));
            }
            const std::array<double, 9> expected =
                covariance(reference.rotation.data() + index * 4, sigma_reference.data());
            const std::array<double, 9> actual =
                covariance(scene.value().rotation.data() + index * 4, sigma_actual.data());
            double scale = 0.0;
            for (int element = 0; element < 9; ++element) {
                scale = std::max(scale, std::fabs(expected[element]));
            }
            for (int element = 0; element < 9; ++element) {
                const double difference = std::fabs(actual[element] - expected[element]);
                max_covariance = std::max(max_covariance, difference);
                if (scale > 1e-12) {
                    max_covariance_rel = std::max(max_covariance_rel, difference / scale);
                }
            }
            std::array<double, 3> sorted_reference{sigma_reference[0], sigma_reference[1],
                                                   sigma_reference[2]};
            std::array<double, 3> sorted_actual{sigma_actual[0], sigma_actual[1], sigma_actual[2]};
            std::sort(sorted_reference.begin(), sorted_reference.end());
            std::sort(sorted_actual.begin(), sorted_actual.end());
            for (int axis = 0; axis < 3; ++axis) {
                max_axis_swap = std::max(
                    max_axis_swap, std::fabs(sorted_actual[axis] - sorted_reference[axis]) /
                                       std::max(sorted_reference[axis], 1e-12));
            }
        }
        char line[192];
        std::snprintf(line, sizeof(line),
                      "  covariance vs ply: max_abs=%.3e max_rel=%.3e sorted_sigma_max_rel=%.3e",
                      max_covariance, max_covariance_rel, max_axis_swap);
        MESSAGE(line);
        CHECK_MESSAGE(max_covariance_rel <= 1e-4, "covariance rel " << max_covariance_rel);
        CHECK_MESSAGE(max_axis_swap <= 1e-4, "sorted sigma rel " << max_axis_swap);
    }
}

}  // namespace
