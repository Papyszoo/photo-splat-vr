// splat_reconstruct: photo -> C5 scene folder through the C9 job runner.
//
//   splat_reconstruct --backend fallback|sharp [options] <photo> <out_scene_dir>
//
// The point is to exercise the real pipeline (decode, EXIF focal, backend,
// scene folder writer) rather than a test harness: the job is enqueued in a
// throwaway queue, run by core/jobs/job_runner, and the produced folder is
// moved to <out_scene_dir>.
//
// Options:
//   --backend <name>       reconstruction backend: fallback (C4) or sharp (C3)
//   --model <file.gguf>    converted model (default: $SPLAT_MODEL_PATH, else
//                          <weights>/dav2s-f16.gguf or
//                          <weights>/sharp_2572gikvuh-f16-v1.gguf)
//   --depth <metres>       assumed scene depth for the fallback (default 3.0)
//   --splats <count>       target Gaussian count for the fallback (default 500000)
//   --cpu                  force the CPU backend

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "common/bytes.h"
#include "common/path.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "recon/fallback/fallback_reconstructor.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/scene_folder.h"

namespace {

using splat::human_bytes;

namespace fs = std::filesystem;

uint64_t peak_rss_bytes() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

void usage() {
    std::fprintf(stderr,
                 "usage: splat_reconstruct --backend fallback|sharp [--model <gguf>]\n"
                 "                         [--depth <m>] [--splats <count>] [--cpu]\n"
                 "                         <photo> <out_scene_dir>\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string backend = "fallback";
    std::string model;
    std::string photo;
    std::string output;
    splat::FallbackConfig config;
    splat::SharpConfig sharp_config;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "splat_reconstruct: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++index];
        };
        if (argument == "-h" || argument == "--help") {
            usage();
            return 0;
        } else if (argument == "--backend") {
            backend = next("--backend");
        } else if (argument == "--model") {
            model = next("--model");
        } else if (argument == "--depth") {
            config.assumed_scene_depth_m = std::stof(next("--depth"));
        } else if (argument == "--splats") {
            config.target_splats = static_cast<size_t>(std::stoul(next("--splats")));
        } else if (argument == "--cpu") {
            config.prefer_vulkan = false;
            sharp_config.prefer_vulkan = false;
        } else if (photo.empty()) {
            photo = argument;
        } else if (output.empty()) {
            output = argument;
        } else {
            std::fprintf(stderr, "splat_reconstruct: unexpected argument '%s'\n", argument.c_str());
            usage();
            return 2;
        }
    }
    if (photo.empty() || output.empty()) {
        usage();
        return 2;
    }
    if (backend != "fallback" && backend != "sharp") {
        std::fprintf(stderr, "splat_reconstruct: unknown backend '%s' (fallback or sharp)\n",
                     backend.c_str());
        return 2;
    }

    if (model.empty()) {
        const char* from_env = std::getenv("SPLAT_MODEL_PATH");
        if (from_env != nullptr) {
            model = from_env;
        } else {
            // weights/ lives outside the repo, next to it (AGENTS.md).
            const char* weights = backend == "sharp" ? "sharp_2572gikvuh-f16-v1.gguf"
                                                     : "dav2s-f16.gguf";
            const fs::path guess = fs::path(SPLAT_SOURCE_DIR) / ".." / "weights" / weights;
            model = guess.lexically_normal().string();
        }
    }
    if (!fs::is_regular_file(model)) {
        std::fprintf(stderr, "splat_reconstruct: model not found: %s (use --model or SPLAT_MODEL_PATH)\n",
                     model.c_str());
        return 2;
    }

    const fs::path out_dir = fs::absolute(output);
    if (fs::exists(out_dir) && !fs::is_empty(out_dir)) {
        std::fprintf(stderr, "splat_reconstruct: %s already exists and is not empty\n",
                     out_dir.string().c_str());
        return 2;
    }

    // Throwaway queue + output root; the finished folder is moved into place.
    std::error_code error;
    fs::path scratch = fs::temp_directory_path(error) / ("splat-reconstruct-" +
                                                         std::to_string(::getpid()));
    fs::remove_all(scratch, error);
    fs::create_directories(scratch / "out", error);
    if (error) {
        std::fprintf(stderr, "splat_reconstruct: cannot create %s: %s\n", scratch.string().c_str(),
                     error.message().c_str());
        return 1;
    }

    splat::Result<splat::JobQueue> queue = splat::JobQueue::open(scratch / "queue.json");
    if (!queue.ok()) {
        std::fprintf(stderr, "splat_reconstruct: %s\n", queue.message().c_str());
        return 1;
    }
    const splat::Result<std::string> enqueued = queue.value().enqueue(photo, backend);
    if (!enqueued.ok()) {
        std::fprintf(stderr, "splat_reconstruct: %s\n", enqueued.message().c_str());
        return 1;
    }

    splat::GpuPhaseGuard guard;
    splat::JobRunnerConfig runner_config;
    runner_config.output_root = scratch / "out";
    runner_config.model_path = model;
    splat::JobRunner runner(
        queue.value(), guard,
        [config, sharp_config](const std::string& name) -> std::unique_ptr<splat::IReconstructor> {
            if (name == "fallback") {
                return std::unique_ptr<splat::IReconstructor>(new splat::FallbackReconstructor(config));
            }
            if (name == "sharp") {
                return std::unique_ptr<splat::IReconstructor>(
                    new splat::SharpReconstructor(sharp_config));
            }
            return nullptr;
        },
        runner_config);

    std::fprintf(stderr, "backend  %s\n", backend.c_str());
    std::fprintf(stderr, "model    %s\n", model.c_str());
    if (backend == "fallback") {
        std::fprintf(stderr, "depth    %.2f m assumed\n", config.assumed_scene_depth_m);
    }

    int last_decile = -1;
    std::string stage;
    double stage_started = 0.0;
    std::vector<std::pair<std::string, double>> stage_seconds;
    const auto clock_started = std::chrono::steady_clock::now();
    auto seconds_now = [&clock_started]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - clock_started).count();
    };
    splat::Progress progress([&](float fraction, const std::string& label) {
        if (label != stage) {
            if (!stage.empty()) stage_seconds.emplace_back(stage, seconds_now() - stage_started);
            stage = label;
            stage_started = seconds_now();
        }
        const int decile = static_cast<int>(fraction * 10.0f);
        if (decile > last_decile) {
            last_decile = decile;
            std::fprintf(stderr, "  %3d%% %s\n", decile * 10, label.c_str());
        }
    });

    const splat::Result<std::optional<splat::JobOutcome>> outcome = runner.run_next(
        [&progress](float fraction, const std::string& stage) { progress.report(fraction, stage); });
    if (!outcome.ok()) {
        std::fprintf(stderr, "splat_reconstruct: %s\n", outcome.message().c_str());
        fs::remove_all(scratch, error);
        return 1;
    }
    if (!outcome.value()) {
        std::fprintf(stderr, "splat_reconstruct: nothing ran\n");
        fs::remove_all(scratch, error);
        return 1;
    }
    const splat::JobOutcome& result = outcome.value().value();
    if (result.state != splat::JobState::Done) {
        std::fprintf(stderr, "splat_reconstruct: job %s: %s\n", result.job_id.c_str(),
                     result.message.c_str());
        fs::remove_all(scratch, error);
        return 1;
    }

    fs::create_directories(out_dir.parent_path(), error);
    fs::remove_all(out_dir, error);
    fs::rename(result.scene_folder, out_dir, error);
    if (error) {
        // rename() cannot cross filesystems, and the scratch folder is on the
        // system temp volume while the scene goes to the data volume.
        error.clear();
        fs::copy(result.scene_folder, out_dir,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
        if (!error) fs::remove_all(result.scene_folder, error);
    }
    if (error) {
        std::fprintf(stderr, "splat_reconstruct: cannot move the scene to %s: %s\n",
                     out_dir.string().c_str(), error.message().c_str());
        fs::remove_all(scratch, error);
        return 1;
    }

    splat::Result<splat::LoadedScene> scene = splat::read_scene_folder(out_dir);
    if (!scene.ok()) {
        std::fprintf(stderr, "splat_reconstruct: wrote %s but cannot read it back: %s\n",
                     out_dir.string().c_str(), scene.message().c_str());
        return 1;
    }
    std::printf("scene    %s\n", out_dir.string().c_str());
    std::printf("backend  %s\n", scene.value().meta.backend.c_str());
    std::printf("splats   %zu\n", scene.value().scene.count());
    std::printf("intr     fx %.2f fy %.2f cx %.1f cy %.1f (%ux%u)\n", scene.value().meta.intrinsics.fx,
                scene.value().meta.intrinsics.fy, scene.value().meta.intrinsics.cx,
                scene.value().meta.intrinsics.cy, scene.value().meta.intrinsics.width,
                scene.value().meta.intrinsics.height);
    std::printf("seconds  %.3f\n", result.seconds_taken);
    for (const auto& entry : stage_seconds) {
        std::printf("stage    %-20s %.3f s\n", entry.first.c_str(), entry.second);
    }
    std::printf("peak rss %s (%" PRIu64 " bytes)\n", human_bytes(peak_rss_bytes()).c_str(),
                peak_rss_bytes());
    fs::remove_all(scratch, error);
    return 0;
}
