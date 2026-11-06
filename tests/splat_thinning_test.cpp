// Thinning an imported scene to the renderer's 2 M splat budget
// (core/splat_import/thinning.*).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "scene/scene_folder.h"
#include "splat_import/splat_import.h"
#include "splat_import/thinning.h"
#include "test_util.h"

using splat::ImportReport;
using splat::JobQueue;
using splat::SplatScene;

namespace {

// A scene whose importance is exactly `index`: the volume is 1
// (log scale 0 each axis) and the opacity logit is monotone in the index, so
// sigmoid() alone ranks the splats 0, 1, 2, ....
SplatScene importance_ramp(size_t count) {
    SplatScene scene;
    scene.resize(count);
    for (size_t i = 0; i < count; ++i) {
        scene.position[i * 3 + 0] = static_cast<float>(i);
        scene.position[i * 3 + 1] = 0.0f;
        scene.position[i * 3 + 2] = 0.0f;
        for (size_t axis = 0; axis < 3; ++axis) scene.log_scale[i * 3 + axis] = 0.0f;
        scene.rotation[i * 4 + 0] = 1.0f;
        scene.rotation[i * 4 + 1] = 0.0f;
        scene.rotation[i * 4 + 2] = 0.0f;
        scene.rotation[i * 4 + 3] = 0.0f;
        for (size_t channel = 0; channel < 3; ++channel) scene.sh_dc[i * 3 + channel] = 0.0f;
        // logit(p) strictly increasing in i, never infinite:
        // sigmoid(logit(i)) = (i + 1) / (count + 1).
        const float t = static_cast<float>(i + 1) / static_cast<float>(count + 1);
        scene.opacity_logit[i] = std::log(t / (1.0f - t));
    }
    return scene;
}

void put_f32(std::vector<uint8_t>& bytes, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bytes.push_back(static_cast<uint8_t>(bits & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 8) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 16) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 24) & 0xFFu));
}

// Three .splat records at x = 0, 1, 2, all red, alpha 128, +Z rotation.
std::vector<uint8_t> three_splat_file() {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 3; ++i) {
        put_f32(bytes, static_cast<float>(i));
        put_f32(bytes, 0.0f);
        put_f32(bytes, 1.0f);
        put_f32(bytes, 0.01f);
        put_f32(bytes, 0.01f);
        put_f32(bytes, 0.01f);
        bytes.push_back(255);
        bytes.push_back(0);
        bytes.push_back(0);
        bytes.push_back(128);
        bytes.push_back(128);
        bytes.push_back(128);
        bytes.push_back(128);
        bytes.push_back(255);
    }
    return bytes;
}

}  // namespace

TEST_CASE("thinning keeps the most important splats, in index order") {
    SplatScene scene = importance_ramp(1000);
    ImportReport report;
    splat::thin_scene(scene, 100, &report);

    REQUIRE(scene.count() == 100);
    CHECK(report.source_count == 1000);
    CHECK(report.kept_count == 100);
    CHECK(report.thinned);
    CHECK(report.note == "thinned 1,000 to 100 splats");

    // Importance rises with the index, so exactly 900..999 survive, ascending.
    for (size_t i = 0; i < 100; ++i) {
        CHECK(scene.position[i * 3 + 0] == doctest::Approx(static_cast<float>(900 + i)));
        // One attribute beyond the position, to prove the copy moved them all.
        const float t = (901.0f + static_cast<float>(i)) / 1001.0f;
        CHECK(scene.opacity_logit[i] == doctest::Approx(std::log(t / (1.0f - t))));
    }
}

TEST_CASE("a non-finite importance is ranked last instead of breaking the cut") {
    // A NaN score used to reach the comparator as "not equal and not greater",
    // which is not a strict weak ordering and is undefined behaviour for
    // nth_element. The four splats with a finite score must survive in order.
    SplatScene scene = importance_ramp(8);
    scene.log_scale[0] = std::nanf("");
    scene.log_scale[4 * 3 + 0] = std::nanf("");
    ImportReport report;
    splat::thin_scene(scene, 4, &report);

    REQUIRE(scene.count() == 4);
    CHECK(report.thinned);
    CHECK(report.kept_count == 4);
    for (size_t i = 0; i < 4; ++i) {
        const float index = scene.position[i * 3 + 0];
        CHECK(std::isfinite(index));
        CHECK(index >= 3.0f);  // the four highest finite scores, 4..7
    }
}

TEST_CASE("a scene that fits is left alone and reports nothing dropped") {
    SplatScene scene = importance_ramp(10);
    ImportReport report;
    splat::thin_scene(scene, 10, &report);
    CHECK(scene.count() == 10);
    CHECK(!report.thinned);
    CHECK(report.source_count == 10);
    CHECK(report.kept_count == 10);
    CHECK(report.note.empty());

    // A limit of 0 means "no limit", not "keep nothing".
    splat::thin_scene(scene, 0, &report);
    CHECK(scene.count() == 10);
    CHECK(!report.thinned);
}

TEST_CASE("2.1 M synthetic splats thin to the 2 M budget") {
    // Built in memory, not from a file: this is the largest source the import
    // limit allows plus a margin, and the test proves the cut itself.
    constexpr size_t kCount = 2'100'000;
    SplatScene scene;
    scene.resize(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        // Every 1000th splat is much bigger, so the survivors are predictable in
        // shape even though this only asserts the count.
        const bool large = i % 1000 == 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            scene.position[i * 3 + axis] = static_cast<float>(i) * (axis == 0 ? 0.001f : 0.0f);
            scene.log_scale[i * 3 + axis] = large ? 0.5f : -3.0f;
            scene.sh_dc[i * 3 + axis] = 0.0f;
        }
        scene.rotation[i * 4 + 0] = 1.0f;
        scene.rotation[i * 4 + 1] = 0.0f;
        scene.rotation[i * 4 + 2] = 0.0f;
        scene.rotation[i * 4 + 3] = 0.0f;
        scene.opacity_logit[i] = 0.0f;
    }

    ImportReport report;
    splat::thin_scene(scene, splat::kMaxSceneSplats, &report);

    CHECK(scene.count() == splat::kMaxSceneSplats);
    CHECK(scene.consistent());
    CHECK(report.thinned);
    CHECK(report.source_count == 2100000);
    CHECK(report.kept_count == 2000000);
    CHECK(report.note == "thinned 2,100,000 to 2,000,000 splats");
    // Index order is preserved, so the kept positions still ascend.
    float previous = scene.position[0];
    for (size_t i = 1; i < scene.count(); ++i) {
        CHECK(scene.position[i * 3] >= previous);
        previous = scene.position[i * 3];
    }
}

TEST_CASE("an import job through JobRunner writes a readable scene folder") {
    splat_test::TempDir dir("import-job");
    const splat::Path queue_file = dir.path() / "state" / "jobs.json";
    JobQueue queue = JobQueue::open(queue_file).value();
    splat::GpuPhaseGuard guard;
    // An import needs no reconstruction backend at all.
    splat::JobRunner runner(queue, guard, nullptr,
                            splat::JobRunnerConfig{dir.path() / "scenes", dir.path() / "model.gguf",
                                                   512, 90});

    const splat::Path splat_file = dir.path() / "robot.splat";
    const std::vector<uint8_t> bytes = three_splat_file();
    {
        std::ofstream out(splat_file, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const splat::Result<std::string> queued = queue.enqueue_import(splat_file, "");
    REQUIRE_MESSAGE(queued.ok(), queued.message());

    const splat::Result<std::optional<splat::JobOutcome>> outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Done);
    CHECK(outcome.value()->message == "imported 3 splats");

    const splat::Path folder = dir.path() / "scenes" / "robot";
    CHECK(outcome.value()->scene_folder == folder);

    const splat::Result<splat::LoadedScene> loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 3);
    CHECK(loaded.value().meta.backend == "import");
    CHECK(loaded.value().meta.origin_format == "splat");
    CHECK(loaded.value().meta.origin_count == 3);
    CHECK(loaded.value().meta.splat_count == 3);
    // The folder has no original.jpg, and the thumbnail exists.
    CHECK(!std::filesystem::exists(folder / "original.jpg"));
    CHECK(std::filesystem::exists(folder / "thumb.jpg"));
    // The row keeps the notice, and the queue reloads it with kind=import.
    const JobQueue reloaded = JobQueue::open(queue_file).value();
    REQUIRE(reloaded.jobs().size() == 1);
    CHECK(reloaded.jobs()[0].kind == splat::JobKind::import);
    CHECK(reloaded.jobs()[0].message == "imported 3 splats");
    CHECK(reloaded.jobs()[0].state == splat::JobState::Done);

    // A conversion row written before imports existed still loads as convert.
    JobQueue old = JobQueue::open(dir.path() / "state2" / "jobs.json").value();
    CHECK(old.enqueue(splat_file, "fake", "", 0).ok());
    const JobQueue old_reloaded = JobQueue::open(dir.path() / "state2" / "jobs.json").value();
    REQUIRE(old_reloaded.jobs().size() == 1);
    CHECK(old_reloaded.jobs()[0].kind == splat::JobKind::convert);
}
