// End-to-end C3 run through the C2 interface: SharpReconstructor on the golden
// photos and on the ImmichVR demo photos, with the shipped fp16 GGUF.
//
// Two inputs per golden photo:
//   - the JPEG decoded by this build (stb), i.e. the real pipeline;
//   - the reference's own uint8 pixels (010_image_uint8.npy, PIL-decoded), which
//     isolates the port from the JPEG decoder difference the spec's testing
//     note describes (stb and PIL disagree by up to 3/255 on 4-6% of bytes).
//
// The scenes are written to $SPLAT_DATA_DIR/runs/sharp_e2e/<photo>[_refpx]/ as
// scene.psplat + meta.json, which is what tools/parity/image_parity.py renders.
//
// This case is opt-in (it writes ~34 MB per photo and takes a minute per photo
// on MoltenVK):
//
//   export SPLAT_SHARP_E2E=1
//   export SPLAT_SHARP_GOLDEN=/path/to/golden
//   export SPLAT_SHARP_GGUF_F16=/path/to/sharp-...-f16-v1.gguf
//   build-F8-sharp/tests/splat_tests -tc="sharp end to end*"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/exif.h"
#include "jobs/image_io.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "sharp_golden.h"

namespace {

using namespace splat;
using namespace splat::sharp_test;

// $SPLAT_DATA_DIR, else the parent of the golden folder (golden/ lives in it).
std::string data_root() {
    const std::string from_env = env_or_empty("SPLAT_DATA_DIR");
    if (!from_env.empty()) return from_env;
    const std::filesystem::path golden = golden_root();
    if (!golden.empty()) return golden.parent_path().string();
    return {};
}

struct Case {
    std::string id;
    std::filesystem::path jpeg;
    std::filesystem::path golden_folder;  // may be empty (ImmichVR)
};

// Decodes `case.jpeg`, or loads the reference pixels when `reference_pixels`.
bool load_input(const Case& test, bool reference_pixels, ReconstructionInput* input,
                std::string* error) {
    Result<std::vector<uint8_t>> bytes = read_file_bytes(test.jpeg);
    if (!bytes.ok()) {
        *error = bytes.message();
        return false;
    }
    Result<Image> image = decode_image(bytes.value().data(), bytes.value().size());
    if (!image.ok()) {
        *error = image.message();
        return false;
    }
    if (reference_pixels) {
        if (test.golden_folder.empty()) {
            *error = "no reference pixels for " + test.id;
            return false;
        }
        std::vector<int64_t> shape;
        std::vector<uint8_t> pixels;
        if (!parity::read_npy_u8((test.golden_folder / "010_image_uint8.npy").string(), &shape,
                                 &pixels, error)) {
            return false;
        }
        if (shape.size() != 3 || static_cast<int>(shape[1]) != image.value().width ||
            static_cast<int>(shape[0]) != image.value().height) {
            *error = "reference pixels do not match the decoded size";
            return false;
        }
        image.value().rgb = std::move(pixels);
    }
    input->rgb = std::move(image.value());
    // Same focal-length rule as the job runner: EXIF when the JPEG has it,
    // SHARP's 30 mm default otherwise.
    const Result<float> focal = focal_px_from_jpeg(bytes.value().data(), bytes.value().size(),
                                                   input->rgb.width, input->rgb.height);
    if (!focal.ok()) {
        *error = focal.message();
        return false;
    }
    input->focal_px = focal.value();
    return true;
}

void run_case(SharpReconstructor& reconstructor, const Case& test, bool reference_pixels) {
    std::string error;
    ReconstructionInput input;
    REQUIRE_MESSAGE(load_input(test, reference_pixels, &input, &error), error);
    Progress progress;
    const Memory before = process_memory();
    Result<SplatScene> scene = reconstructor.run(input, progress);
    const Memory after = process_memory();
    REQUIRE_MESSAGE(scene.ok(), scene.message());
    const SharpTimings& timings = reconstructor.last_timings();
    MESSAGE("  " << test.id << (reference_pixels ? " [reference pixels]" : " [stb decode]")
            << ": " << scene.value().count() << " splats, " << timings.total << " s"
            << " (preprocess " << timings.preprocess << ", encoder " << timings.encoder
            << ", depth " << timings.depth_decoder << ", init " << timings.initializer
            << ", gaussians " << timings.gaussian_decoder << ", composer " << timings.composer
            << "), peak RSS " << mib(after.peak) << " (" << mib(before.peak) << " before)");

    CHECK(scene.value().count() == 1179648);  // 768^2 * 2 layers
    CHECK(scene.value().consistent());
    // Metric space, OpenCV axes: z forward around the capture distance.
    const Bounds bounds = scene.value().bounds();
    CHECK(bounds.valid);
    CHECK_MESSAGE(bounds.min[2] > 0.0f, "a Gaussian is behind the camera");
    // One aggregate check instead of one assertion per value (14 * 1.18 M).
    size_t non_finite = 0;
    for (const std::vector<float>* attribute : {&scene.value().position, &scene.value().log_scale,
                                                &scene.value().rotation, &scene.value().sh_dc,
                                                &scene.value().opacity_logit}) {
        for (float value : *attribute) {
            if (!std::isfinite(value)) ++non_finite;
        }
    }
    CHECK_MESSAGE(non_finite == 0, non_finite << " non-finite Gaussian attributes");

    // Write scene.psplat + meta.json next to the CLI's output, so
    // tools/parity/image_parity.py can render it.
    const int width = input.rgb.width;
    const int height = input.rgb.height;
    const float focal = input.focal_px.value_or(0.0f);
    const std::filesystem::path folder =
        std::filesystem::path(data_root()) / "runs" / "sharp_e2e" /
        (test.id + (reference_pixels ? "_refpx" : ""));
    std::error_code filesystem_error;
    std::filesystem::create_directories(folder, filesystem_error);
    REQUIRE_MESSAGE(!filesystem_error, "cannot create " << folder << ": "
                                                        << filesystem_error.message());
    const Status written =
        write_psplat(folder / "scene.psplat", scene.value(), focal,
                     static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    REQUIRE_MESSAGE(written.ok(), written.message());

    SceneMeta meta;
    meta.backend = "sharp";
    meta.splat_count = static_cast<uint32_t>(scene.value().count());
    meta.seconds_taken = timings.total;
    meta.intrinsics.width = static_cast<uint32_t>(width);
    meta.intrinsics.height = static_cast<uint32_t>(height);
    meta.intrinsics.fx = focal;
    meta.intrinsics.fy = focal;
    meta.intrinsics.cx = static_cast<float>(width) / 2.0f;
    meta.intrinsics.cy = static_cast<float>(height) / 2.0f;
    const Status meta_written = write_scene_meta(folder / "meta.json", meta);
    REQUIRE_MESSAGE(meta_written.ok(), meta_written.message());
    MESSAGE("  wrote " << folder);
}

}  // namespace

TEST_CASE("sharp end to end") {
    if (env_or_empty("SPLAT_SHARP_E2E").empty()) {
        MESSAGE("skipping: set SPLAT_SHARP_E2E=1 to run the end-to-end cases");
        return;
    }
    if (skip_case("end to end", true)) return;
    const std::filesystem::path golden = golden_root();

    std::vector<Case> cases;
    for (const Photo& photo : photos(golden)) {
        Case test;
        test.id = photo.id;
        test.jpeg = golden / "inputs" / (photo.id + ".jpg");
        test.golden_folder = photo.folder;
        if (std::filesystem::exists(test.jpeg)) cases.push_back(std::move(test));
    }
    const std::filesystem::path immich = golden / "immichvr";
    for (int index = 1; index <= 3; ++index) {
        const std::string id = "immichvr-" + std::to_string(index);
        const std::filesystem::path jpeg = immich / (std::to_string(index) + ".jpg");
        if (!std::filesystem::exists(jpeg)) continue;
        cases.push_back(Case{id, jpeg, {}});
    }
    REQUIRE_MESSAGE(!cases.empty(), "no inputs under " << golden);

    SharpConfig config;
    SharpReconstructor reconstructor(config);
    const Status loaded = reconstructor.load(gguf_path(true));
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    MESSAGE("backend: " << reconstructor.info().name << " (" << reconstructor.info().license << ")");

    for (const Case& test : cases) {
        MESSAGE("photo " << test.id);
        run_case(reconstructor, test, /*reference_pixels=*/false);
        if (!test.golden_folder.empty()) run_case(reconstructor, test, /*reference_pixels=*/true);
    }
    reconstructor.unload();
}
