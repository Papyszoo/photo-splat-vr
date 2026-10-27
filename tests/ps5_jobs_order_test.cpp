// The S1 conversion order, on the host: ConvertGate (platform/ps5/viewer/
// convert_gate.h) over the real JobService + JobQueue + JobRunner with a fake
// reconstructor, the same objects the console title builds (ps5_jobs.cpp).
//
// What this pins down is the order W18 measured on the console (runs/ui_v1/
// reports/W18.md 4.1-4.3): a conversion pauses scene loading, the viewed scene
// is released only when the policy has a release hook (S1 has none - a live
// scene stays), the model is loaded, the photo runs, the model is unloaded, and
// only then is the renderer's side restored and scene loading reopened. A failed
// job restores too: the panel must not stay locked out of viewing because one
// photo could not be converted. The scene-switch permission itself follows
// viewer::scene_loading_allowed: only the model-load phase (queued, or a running
// job still "starting") holds a scene load, so the tests below look at both.
//
// The event log is shared between the test thread (the gate's hooks), the
// service's worker thread (load/run/unload) and nothing else, so its order is
// the real order; the mutex makes the sequence total.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/image.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "test_util.h"
#include "ui/job_service.h"

#if defined(SPLAT_HAVE_VULKAN)
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif
#endif

#include "../platform/ps5/viewer/convert_gate.h"

using namespace std::chrono_literals;

namespace {

using splat::JobService;
using splat::JobView;
using splat_test::release;
using splat_test::wait_until;
using splat_test::write_test_jpeg;
using viewer::ConvertGate;
using viewer::ConvertGatePhase;

// The one place every step appends to, whoever ran it.
class EventLog {
public:
    void add(const std::string& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }

    std::string joined() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string text;
        for (const std::string& event : events_) {
            if (!text.empty()) text += ' ';
            text += event;
        }
        return text;
    }

    int count(const std::string& event) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int total = 0;
        for (const std::string& other : events_) {
            if (other == event) ++total;
        }
        return total;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};

// The fake backend: it logs the three calls the runner makes around a photo and
// can fail run(), or hold inside it so the test can look at the gate mid-job.
class LoggingReconstructor : public splat::IReconstructor {
public:
    struct Control {
        std::mutex mutex;
        std::condition_variable cv;
        bool block = false;
        bool released = false;
        bool fail = false;
        bool running = false;
        // Hold inside load(), so the model-load phase can be observed (that is
        // the one phase a scene switch waits for).
        bool block_load = false;
        bool load_started = false;
        bool load_released = false;
    };

    LoggingReconstructor(std::shared_ptr<EventLog> log, std::shared_ptr<Control> control)
        : log_(std::move(log)), control_(std::move(control)) {}

    splat::BackendInfo info() const override { return {"fake", "test-only", 0}; }

    splat::Status load(const splat::Path&) override {
        log_->add("load");
        {
            std::lock_guard<std::mutex> lock(control_->mutex);
            control_->load_started = true;
            control_->cv.notify_all();
        }
        {
            std::unique_lock<std::mutex> lock(control_->mutex);
            if (control_->block_load) {
                control_->cv.wait_for(lock, 15s, [this] { return control_->load_released; });
            }
        }
        return splat::Status::success();
    }

    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput&,
                                         splat::Progress& progress) override {
        progress.report(0.5f, "inferring");
        {
            std::lock_guard<std::mutex> lock(control_->mutex);
            control_->running = true;
            control_->cv.notify_all();
        }
        {
            std::unique_lock<std::mutex> lock(control_->mutex);
            if (control_->block) {
                control_->cv.wait_for(lock, 15s, [this] { return control_->released; });
            }
            control_->running = false;
        }
        log_->add("run");
        if (control_->fail) return splat::Result<splat::SplatScene>::failure("the fake backend failed");
        return splat::Result<splat::SplatScene>::success(splat_test::make_test_scene(7));
    }

    void unload() override { log_->add("unload"); }

private:
    std::shared_ptr<EventLog> log_;
    std::shared_ptr<Control> control_;
};

// Lets a fake load() that was held by block_load finish.
void release_load(const std::shared_ptr<LoggingReconstructor::Control>& control) {
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        control->load_released = true;
    }
    control->cv.notify_all();
}

// The console's conversion chain, on the host: queue -> guard -> runner (fake
// backend) -> service, plus the gate whose hooks write into the same log.
struct Fixture {
    splat_test::TempDir dir{"ps5-jobs-order"};
    std::shared_ptr<EventLog> log = std::make_shared<EventLog>();
    std::shared_ptr<LoggingReconstructor::Control> control =
        std::make_shared<LoggingReconstructor::Control>();
    splat::JobQueue queue;
    splat::GpuPhaseGuard guard;
    splat::JobRunner runner;
    std::unique_ptr<JobService> service;
    ConvertGate gate;
    // Set by the gate's hooks, so the test can assert the machine is closed
    // exactly between pause and resume.
    bool loading_closed_seen = false;

    Fixture()
        : queue(splat::JobQueue::open(dir.path() / "state" / "jobs.json").value()),
          runner(queue, guard,
                 [this](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
                     if (backend != "fake") return nullptr;
                     return std::make_unique<LoggingReconstructor>(log, control);
                 },
                 splat::JobRunnerConfig{dir.path() / "scenes", dir.path() / "model.gguf", 64, 90}) {
        // The S1 hook set: pause, no release (W18 keeps the viewed scene),
        // restore and resume. `release` is logged by the unload-only variant
        // test below through its own gate.
        gate = ConvertGate(ConvertGate::Hooks{
            [this] { log->add("pause"); },
            {},
            [this] { log->add("restore"); },
            [this] { log->add("resume"); }});
        service = std::make_unique<JobService>(queue, runner);
    }

    splat::Path photo(const std::string& name) const { return dir.path() / name; }

    // One "frame": the gate follows the service. Returns the phase it ended in.
    ConvertGatePhase frame() {
        gate.poll(*service);
        if (!viewer::scene_loading_allowed(*service)) loading_closed_seen = true;
        return gate.phase();
    }

    // Runs frames until the gate is idle again after following at least one job.
    bool run_until_idle(std::chrono::milliseconds timeout = 20s) {
        return wait_until([this] {
            frame();
            return gate.phase() == ConvertGatePhase::Idle && gate.completed() > 0;
        }, timeout);
    }
};

}  // namespace

TEST_CASE("ps5_jobs: S1 order is pause, job, restore, resume") {
    Fixture fixture;
    fixture.control->block = true;  // hold inference so the gate can be seen mid-job
    write_test_jpeg(fixture.photo("photo.jpg"));

    REQUIRE(fixture.service->enqueue({fixture.photo("photo.jpg")}, "fake", "").ok());

    // First frame after the enqueue: a queued job is the model-load window
    // (the worker is about to load), so a scene switch waits.
    CHECK(fixture.frame() == ConvertGatePhase::Paused);
    CHECK_FALSE(viewer::scene_loading_allowed(*fixture.service));

    // The worker starts the job: Paused -> Converting, and once the model is
    // loaded (stage "inferring") the user may switch scenes again.
    REQUIRE(wait_until([&] { return fixture.service->busy(); }));
    REQUIRE(wait_until([&] { return fixture.service->snapshot()[0].stage == "inferring"; }));
    CHECK(fixture.frame() == ConvertGatePhase::Converting);
    CHECK(viewer::scene_loading_allowed(*fixture.service));
    {
        std::lock_guard<std::mutex> lock(fixture.control->mutex);
        CHECK(fixture.control->running);
    }

    // A ViewScene asked for now is allowed: this is what main.cpp/vr_mode check.
    CHECK(viewer::scene_loading_allowed(*fixture.service));

    release(fixture.control);
    REQUIRE(fixture.run_until_idle());

    CHECK(fixture.gate.phase() == ConvertGatePhase::Idle);
    CHECK(viewer::scene_loading_allowed(*fixture.service));
    CHECK(fixture.loading_closed_seen);
    CHECK(fixture.gate.completed() == 1);
    // The real order, across the two threads.
    CHECK(fixture.log->joined() == "pause load run unload restore resume");
    CHECK(fixture.log->count("load") == 1);
    CHECK(fixture.log->count("unload") == 1);
    // No release hook in S1: nothing was unloaded, the scene stays.
    CHECK(fixture.log->count("release") == 0);

    const std::vector<JobView> views = fixture.service->snapshot();
    REQUIRE(views.size() == 1);
    CHECK(views[0].state == "done");
    CHECK(std::filesystem::is_directory(fixture.dir.path() / "scenes" / "photo"));
}

TEST_CASE("ps5_jobs: only the model load holds a scene switch") {
    Fixture fixture;
    fixture.control->block_load = true;
    fixture.control->block = true;  // hold the inference so "inferring" is observable
    write_test_jpeg(fixture.photo("load.jpg"));

    REQUIRE(fixture.service->enqueue({fixture.photo("load.jpg")}, "fake", "").ok());
    // The worker is inside load(), before the runner reports "inferring": the
    // tight memory window W18 measured (8.65 GiB free at model load).
    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(fixture.control->mutex);
        return fixture.control->load_started;
    }));
    CHECK(fixture.frame() == ConvertGatePhase::Paused);       // Idle -> Paused
    CHECK_FALSE(viewer::scene_loading_allowed(*fixture.service));
    CHECK(fixture.frame() == ConvertGatePhase::Converting);
    CHECK_FALSE(viewer::scene_loading_allowed(*fixture.service));

    // The load returns, inference starts: switching is free again and the job
    // still finishes normally.
    release_load(fixture.control);
    REQUIRE(wait_until([&] { return fixture.service->snapshot()[0].stage == "inferring"; }));
    CHECK(viewer::scene_loading_allowed(*fixture.service));
    release(fixture.control);
    REQUIRE(fixture.run_until_idle());
    CHECK(viewer::scene_loading_allowed(*fixture.service));
    CHECK(fixture.service->snapshot()[0].state == "done");
}

TEST_CASE("ps5_jobs: a failed job still restores and reopens viewing") {
    Fixture fixture;
    fixture.control->fail = true;
    write_test_jpeg(fixture.photo("bad.jpg"));

    REQUIRE(fixture.service->enqueue({fixture.photo("bad.jpg")}, "fake", "").ok());
    REQUIRE(fixture.run_until_idle());

    CHECK(fixture.gate.phase() == ConvertGatePhase::Idle);
    CHECK(viewer::scene_loading_allowed(*fixture.service));
    CHECK(fixture.loading_closed_seen);
    CHECK(fixture.log->joined() == "pause load run unload restore resume");
    const std::vector<JobView> views = fixture.service->snapshot();
    REQUIRE(views.size() == 1);
    CHECK(views[0].state == "failed");
    CHECK(views[0].message.find("the fake backend failed") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.dir.path() / "scenes" / "bad"));
}

TEST_CASE("ps5_jobs: a queue of two photos restores once, after both") {
    Fixture fixture;
    write_test_jpeg(fixture.photo("one.jpg"));
    write_test_jpeg(fixture.photo("two.jpg"));

    REQUIRE(fixture.service->enqueue({fixture.photo("one.jpg"), fixture.photo("two.jpg")}, "fake", "")
                .ok());
    REQUIRE(fixture.run_until_idle());

    // The model is loaded and unloaded once per job (the runner's contract), but
    // the renderer's side stays closed between the two: one pause, one restore.
    CHECK(fixture.log->count("pause") == 1);
    CHECK(fixture.log->count("restore") == 1);
    CHECK(fixture.log->count("resume") == 1);
    CHECK(fixture.log->count("run") == 2);
    CHECK(fixture.gate.completed() == 1);
    CHECK(fixture.log->joined() ==
          "pause load run unload load run unload restore resume");
    CHECK(std::filesystem::is_directory(fixture.dir.path() / "scenes" / "one"));
    CHECK(std::filesystem::is_directory(fixture.dir.path() / "scenes" / "two"));
}

TEST_CASE("ps5_jobs: the release hook runs between pause and the job") {
    // The same machine with the unload policy S1 does not need (W18 4.2 measured
    // that a live scene fits, so the console passes no release hook): the hook
    // order must stay pause -> release -> job ... restore -> resume.
    Fixture fixture;
    fixture.gate = ConvertGate(ConvertGate::Hooks{[&] { fixture.log->add("pause"); },
                                                  [&] { fixture.log->add("release"); },
                                                  [&] { fixture.log->add("restore"); },
                                                  [&] { fixture.log->add("resume"); }});
    write_test_jpeg(fixture.photo("variant.jpg"));

    REQUIRE(fixture.service->enqueue({fixture.photo("variant.jpg")}, "fake", "").ok());
    REQUIRE(fixture.run_until_idle());

    CHECK(fixture.log->joined() == "pause release load run unload restore resume");
}

TEST_CASE("ps5_jobs: leaving mid-job reopens viewing without restoring") {
    Fixture fixture;
    fixture.control->block = true;
    write_test_jpeg(fixture.photo("quit.jpg"));

    REQUIRE(fixture.service->enqueue({fixture.photo("quit.jpg")}, "fake", "").ok());
    REQUIRE(wait_until([&] { return fixture.service->busy(); }));
    // Two frames: the first one closes scene loading (the job is already
    // running, but the gate starts from Idle), the second follows it into
    // Converting.
    CHECK(fixture.frame() == ConvertGatePhase::Paused);
    CHECK(fixture.frame() == ConvertGatePhase::Converting);

    // What main.cpp/vr_mode do before the teardown: only the loading side is
    // reopened; nothing is put back under a GPU that is still writing.
    fixture.gate.abort();
    CHECK(fixture.gate.phase() == ConvertGatePhase::Idle);
    CHECK(fixture.log->count("resume") == 1);
    CHECK(fixture.log->count("restore") == 0);
    CHECK(fixture.log->count("unload") == 0);  // still inside the fake's run()

    // The worker is joined by the service's destructor once the fake returns.
    release(fixture.control);
    fixture.service.reset();
    CHECK(fixture.log->count("unload") == 1);
}

TEST_CASE("ps5_jobs: a queue with no active job leaves the gate open") {
    Fixture fixture;
    CHECK(fixture.frame() == ConvertGatePhase::Idle);
    CHECK(viewer::scene_loading_allowed(*fixture.service));
    CHECK(fixture.gate.completed() == 0);
    CHECK(fixture.log->joined().empty());

    // Only terminal rows are left: a restart with a finished queue must not lock
    // scene viewing.
    write_test_jpeg(fixture.photo("done.jpg"));
    REQUIRE(fixture.service->enqueue({fixture.photo("done.jpg")}, "fake", "").ok());
    REQUIRE(fixture.run_until_idle());
    CHECK(fixture.service->snapshot()[0].state == "done");
    CHECK(fixture.frame() == ConvertGatePhase::Idle);
    CHECK(fixture.gate.completed() == 1);
}

#if defined(SPLAT_HAVE_VULKAN)

namespace {

// Intensity centroid in pixel-centre coordinates (pixel i spans [i, i+1)), the
// same measure tests/render_projection_test.cpp uses.
bool bright_centroid(const splat::Image& image, double& x, double& y) {
    float peak = 0.0f;
    for (size_t i = 0; i < image.pixel_count(); ++i) {
        const uint8_t* p = image.rgb.data() + i * 3;
        peak = std::max(peak, (p[0] + p[1] + p[2]) / 3.0f);
    }
    if (peak < 1.0f) return false;
    const float threshold = 0.2f * peak;
    double sum = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* p = image.pixel(px, py);
            const float value = (p[0] + p[1] + p[2]) / 3.0f;
            if (value < threshold) continue;
            sum += value;
            sx += (px + 0.5) * value;
            sy += (py + 0.5) * value;
        }
    }
    if (sum <= 0.0) return false;
    x = sx / sum;
    y = sy / sum;
    return true;
}

}  // namespace

TEST_CASE("ps5_jobs: set_intrinsics draws a scene with its own capture camera") {
    // The TV viewer's use (W18 9.2): a converted scene becomes visible after the
    // renderer was built for whatever was at boot, so main.cpp replaces the eye
    // and capture intrinsics from the new meta.json.
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 64;
    options.eye_height = 64;
    options.stereo = false;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    options.intrinsics = {50.0f, 50.0f, 32.0f, 32.0f};  // the "boot" camera
    // A black backdrop, so the centroid below measures the splat and not the
    // renderer's neutral 0.18 clear colour (a 60 % grey is "bright" otherwise).
    options.backdrop[0] = 0.0f;
    options.backdrop[1] = 0.0f;
    options.backdrop[2] = 0.0f;

    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.2f, 0.0f, 1.0f};
    scene.log_scale = {std::log(0.05f), std::log(0.05f), std::log(0.05f)};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    const float white_dc = 0.8f / 0.28209479177387814f;
    scene.sh_dc = {white_dc, white_dc, white_dc};
    scene.opacity_logit = {12.0f};

    auto renderer = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(renderer.ok(), renderer.message());
    splat::SplatRenderer& splat_renderer = *renderer.value();

    auto image = splat::create_vulkan_color_image(
        device, options.target_format, splat_renderer.target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image.ok(), image.message());

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = splat_renderer.target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    auto render_frame = [&]() {
        const splat::Status status = splat_renderer.render(splat::FrameView{}, target, true);
        REQUIRE_MESSAGE(status.ok(), status.message());
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = splat_renderer.read_image();
        REQUIRE_MESSAGE(pixels.ok(), pixels.message());
        return pixels.take();
    };

    // Boot camera fx 50: the splat at x/z = 0.2 lands at 32 + 10 = 42 px.
    double boot_x = 0.0;
    double boot_y = 0.0;
    REQUIRE(bright_centroid(render_frame(), boot_x, boot_y));
    CHECK(std::abs(boot_x - 42.0) < 1.0);

    // Setter camera fx 100: the same ray lands at 32 + 20 = 52 px.
    splat_renderer.set_intrinsics({100.0f, 100.0f, 32.0f, 32.0f}, {}, 0, 0);
    double set_x = 0.0;
    double set_y = 0.0;
    REQUIRE(bright_centroid(render_frame(), set_x, set_y));
    CHECK(std::abs(set_x - 52.0) < 1.0);
    CHECK(set_x > boot_x + 8.0);

    // The capture parameters take effect with the same call: a capture image
    // covering only the left half fades the right edge to the backdrop while the
    // splat (view x/z 0.2 -> capture px 26) stays visible.
    splat_renderer.set_intrinsics({50.0f, 50.0f, 32.0f, 32.0f},
                                  {50.0f, 50.0f, 16.0f, 16.0f}, 32, 32);
    const splat::Image captured = render_frame();
    const uint8_t* right = captured.pixel(60, 32);
    const uint8_t* splat = captured.pixel(42, 32);
    CHECK((right[0] + right[1] + right[2]) / 3.0f < 80.0f);
    CHECK((splat[0] + splat[1] + splat[2]) / 3.0f > 180.0f);

    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("ps5_jobs: renderer intrinsics are skipped without Vulkan") { CHECK(true); }

#endif
