// Stage 3-4 parity: the SHARP depth decoder (DPT with two depth layers), the
// disparity head, the monodepth conversion, and the identity "depth adjustment".
//
// The decoder is fed the *reference* encoder levels (021_encoder_out_XX.npy), so
// this case isolates stage 3 from stage 2.
//
//   export SPLAT_SHARP_GOLDEN=/path/to/golden
//   export SPLAT_SHARP_GGUF=/path/to/sharp-...-f32-v1.gguf
//   export SPLAT_SHARP_GGUF_F16=/path/to/sharp-...-f16-v1.gguf
//   export SPLAT_SHARP_PHOTOS=photo02_street        # optional, shortens the run
//   build-F8-sharp/tests/splat_tests -tc="sharp depth*"
//
// Gates: fp32 CPU (the parity target) asserts the spec's 1e-3 *maximum*
// relative error per stage, plus p99 <= 1e-3 / p99.9 <= 2e-3 and cosine. fp16
// (the shipped file) asserts the spec's cosine >= 0.999 per tensor; MoltenVK is
// gated that way too, since the spec excludes it from the fp32 target. Every
// tensor carries an explicit gate — see kStages for the two places where the
// measured worst case is above the spec number and why.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "recon/sharp/sharp_depth_decoder.h"
#include "sharp_golden.h"

namespace {

using namespace splat;
using namespace splat::sharp_test;

// The initialiser clamps the normalised depth to 100
// (`_rescale_depth(..., depth_max=1e2)`), so depth beyond ~100 normalised units
// never reaches a Gaussian.
//
// `monodepth` is the one tensor where the spec's 1e-3 max-relative gate cannot
// hold over the full range, and the reason is in the reference's own formula:
// monodepth = disparity_factor / clamp(disparity, 1e-4, 1e4). ~10% of photo02's
// pixels (the sky) sit on the 1e-4 clamp, where the fp32 disparity itself is
// ~1e-4 and an absolute difference of 1e-7 (one fp32 ulp of the head output) is
// a 1e-3 relative depth difference; in fp16 the head's absolute error is 6.5e-4,
// i.e. several times the clamp value, so those pixels jump by tens of metres.
// The reference's own CPU-vs-MPS comparison shows the same effect (40.3 m,
// docs/research/sharp-reference.md §10). So the full-range numbers are gated at
// the measured worst case plus headroom (`kMonodepthFull*`), and the spec's
// 1e-3 / cosine-0.999 gates are asserted on the range the next stage keeps,
// which is what the network's depth parity actually means.
constexpr double kMonodepthGateRange = 100.0;
constexpr double kMonodepthFullMaxAbs = 55.0;   // m, measured worst 44.61
constexpr double kMonodepthFullMaxRel = 1e-2;   // measured worst 5.74e-3
constexpr double kMonodepthFullFp16Cos = 0.98;  // measured worst 0.9810

struct Expectation {
    const char* stage;
    const char* file;
    // Full-tensor gates. fp32 CPU max_abs/max_rel are measured worst + headroom
    // (the comment in the table); fp16_cos is the spec's 0.999 except for
    // monodepth.
    double max_abs;
    double max_rel;
    double fp16_cos;
    // Second pass restricted to |reference| <= range_max (0 = none). Carries the
    // spec gate itself: 1e-3 max relative, 0.999 cosine.
    double range_max;
    double range_max_rel;
    double range_fp16_cos;
};

const Expectation kStages[] = {
    // 256@768^2, measured fp32 max_abs 2.6e-6 / max_rel 9.4e-5.
    {"depth_decoder_out", "030_depth_decoder_out.npy", 1e-5, 1e-3, 0.999, 0, 0, 0},
    // 2@1536^2 disparity, measured fp32 max_abs 9.8e-7 / max_rel 8.3e-5.
    {"depth_head_out", "031_depth_head_out.npy", 1e-5, 1e-3, 0.999, 0, 0, 0},
    // 041 is the reference's post-adjustment depth (040_depth_align_out is the
    // same tensor; the identity case below checks that).
    {"monodepth", "041_monodepth.npy", kMonodepthFullMaxAbs, kMonodepthFullMaxRel,
     kMonodepthFullFp16Cos, kMonodepthGateRange, 1e-3, 0.999},
};

void run_depth_case(bool prefer_vulkan, bool fp16) {
    const char* label = fp16 ? (prefer_vulkan ? "vulkan fp16" : "cpu fp16")
                             : (prefer_vulkan ? "vulkan fp32" : "cpu fp32");
    if (skip_case(label, fp16)) return;
    const std::vector<Photo> found = photos(golden_root());
    REQUIRE_MESSAGE(!found.empty(), "no golden photos under " << golden_root());

    Result<std::shared_ptr<SharpModel>> model = SharpModel::load(gguf_path(fp16), prefer_vulkan);
    REQUIRE_MESSAGE(model.ok(), model.message());
    const bool on_vulkan =
        model.value()->backend().info().kind == GgmlBackend::Kind::Vulkan;
    MESSAGE("backend: " << model.value()->backend().describe() << " (" << label << ")");

    Result<std::unique_ptr<SharpDepthDecoder>> decoder =
        SharpDepthDecoder::create(model.value());
    REQUIRE_MESSAGE(decoder.ok(), decoder.message());

    for (const Photo& photo : found) {
        MESSAGE("photo " << photo.id);
        std::string error;
        PhotoMeta meta;
        REQUIRE_MESSAGE(read_photo_meta(photo, &meta, &error), error);
        const float disparity_factor = meta.focal_px / static_cast<float>(meta.width);
        // 1 GiB holds the five reference level maps (837 MiB).
        GraphSession levels_session(model.value()->backend(), model.value()->weights(), 64,
                                    1024u << 20);
        LevelTensors levels;
        REQUIRE_MESSAGE(upload_levels(levels_session, model.value()->weights(), photo, &levels,
                                      &error),
                        error);

        std::map<std::string, SharpStage> stages;
        const Memory before = process_memory();
        const auto started = std::chrono::steady_clock::now();
        Result<SharpDepthResult> result = decoder.value()->run(
            levels.levels, 1536, disparity_factor,
            [&stages](const SharpStage& stage) { stages[stage.name] = stage; });
        const double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE_MESSAGE(result.ok(), result.message());
        const Memory after = process_memory();
        MESSAGE("  seconds: decoder " << result.value().decoder_seconds << ", head "
                << result.value().head_seconds << ", wall " << wall << ", submits "
                << result.value().submits << ", arena " << mib(result.value().arena_bytes)
                << ", RSS before " << mib(before.resident) << " after " << mib(after.resident)
                << " peak " << mib(after.peak));

        for (const Expectation& expectation : kStages) {
            // `monodepth` is the port's own conversion of the head's disparity
            // (stage 4 is the identity), not a stage dump.
            std::vector<float> owned;
            std::vector<int64_t> shape;
            const std::vector<float>* values = nullptr;
            if (std::string(expectation.stage) == "monodepth") {
                owned = result.value().monodepth;
                shape = {1536, 1536, 2, 1};
                values = &owned;
            } else {
                const auto stage = stages.find(expectation.stage);
                if (stage == stages.end()) {
                    FAIL("the depth decoder did not report stage " << expectation.stage);
                    continue;
                }
                shape = stage->second.shape;
                values = &stage->second.values;
            }
            parity::Stats stats = parity::compare_npy(
                (photo.folder / expectation.file).string(), shape, values->data());
            print_stats(expectation.stage, stats, fp16 ? " [fp16]" : " [fp32]");
            REQUIRE_MESSAGE(stats.ok, stats.error);
            // The spec's fp16 gate is cosine >= 0.999 per tensor, and it is the
            // only gate on MoltenVK (the spec excludes it as a parity target);
            // both are asserted everywhere.
            CHECK_MESSAGE(stats.cosine >= expectation.fp16_cos,
                          expectation.stage << " cosine " << stats.cosine);
            if (!fp16 && !on_vulkan) {
                // The spec's fp32 gates, per element.
                CHECK_MESSAGE(stats.max_abs <= expectation.max_abs,
                              expectation.stage << " max abs " << stats.max_abs);
                CHECK_MESSAGE(stats.max_rel <= expectation.max_rel,
                              expectation.stage << " max rel " << stats.max_rel);
                CHECK_MESSAGE(stats.p99_rel <= 1e-3, expectation.stage << " p99 rel "
                              << stats.p99_rel);
                CHECK_MESSAGE(stats.p999_rel <= 2e-3,
                              expectation.stage << " p99.9 rel " << stats.p999_rel);
            }
            if (expectation.range_max > 0.0) {
                stats = parity::compare_npy((photo.folder / expectation.file).string(), shape,
                                            values->data(), expectation.range_max);
                print_stats(std::string(expectation.stage) + " <=" +
                                std::to_string(static_cast<int>(expectation.range_max)) + "m",
                            stats, fp16 ? " [fp16, spec gate]" : " [fp32, spec gate]");
                REQUIRE_MESSAGE(stats.ok, stats.error);
                CHECK_MESSAGE(stats.cosine >= expectation.range_fp16_cos,
                              expectation.stage << " in-range cosine " << stats.cosine);
                if (!fp16 && !on_vulkan) {
                    CHECK_MESSAGE(stats.max_rel <= expectation.range_max_rel,
                                  expectation.stage << " in-range max rel " << stats.max_rel);
                }
            }
        }

        // ---- stage 4: the depth adjustment is the identity at inference -----
        // RGBGaussianPredictor.forward always passes depth=None, so DepthAlignment
        // takes its else branch and returns an all-ones map without touching
        // scale_map_estimator; the port therefore has no stage-4 code at all.
        // This is checked against the reference's own dumps rather than against a
        // buffer the port filled in: the map is exactly 1.0, the reference's
        // output is bit-identical to its monodepth, and that output is what our
        // monodepth above was compared with.
        std::vector<float> align_map;
        std::vector<float> align_out;
        std::vector<float> reference_monodepth;
        REQUIRE_MESSAGE(read_golden(photo, "040_depth_align_map.npy", &align_map, &error), error);
        REQUIRE_MESSAGE(read_golden(photo, "040_depth_align_out.npy", &align_out, &error), error);
        REQUIRE_MESSAGE(read_golden(photo, "041_monodepth.npy", &reference_monodepth, &error),
                        error);
        REQUIRE(align_out.size() == reference_monodepth.size());
        REQUIRE(align_map.size() == align_out.size());
        size_t not_one = 0;
        size_t differs = 0;
        for (size_t index = 0; index < align_map.size(); ++index) {
            if (align_map[index] != 1.0f) ++not_one;
            if (align_out[index] != reference_monodepth[index]) ++differs;
        }
        MESSAGE("  depth adjustment: reference align map is 1.0 at " << (align_map.size() - not_one)
                << "/" << align_map.size() << " pixels, its output differs from 041_monodepth at "
                << differs << " pixels, ours matched 041 above");
        CHECK_MESSAGE(not_one == 0, not_one << " alignment-map pixels are not 1.0");
        CHECK_MESSAGE(differs == 0,
                      "the reference's depth adjustment is not the identity at " << differs
                          << " pixels; stage 4 is not a no-op for this data");
    }
}

}  // namespace

TEST_CASE("sharp depth decoder parity cpu fp32") { run_depth_case(false, false); }
TEST_CASE("sharp depth decoder parity vulkan fp32") { run_depth_case(true, false); }
TEST_CASE("sharp depth decoder parity cpu fp16") { run_depth_case(false, true); }
TEST_CASE("sharp depth decoder parity vulkan fp16") { run_depth_case(true, true); }
