// Spike A on the console: the SHARP backend (C3) on a photo from the console
// bench directory, with the converted GGUF from /data/ps5-splat-vr/models/.
//
//   photo  /data/ps5-splat-vr/bench/photo.jpg  (or the first *.jpg there)
//   GGUF   /data/ps5-splat-vr/models/sharp_2572gikvuh-f16-v1.gguf
//   output <bench dir>/<photo stem>.sharp.psplat
//
// The payload drives the C2 interface (SharpReconstructor), not a fixed stage
// list, so a newer tree that adds a stage runs it without a change here: the
// stages that exist report their own time and the ones that do not stay at
// 0.0 s in the summary. Every submission is one small graph (GraphSession),
// which is what keeps the RADV device-lost risk (spec Risks) bounded. The
// watchdog bounds every wait ggml cannot bound (see bench_env.h) and, like a
// compute error, abandons the process instead of tearing down GPU memory that
// may still be in use.
//
// Build: platform/ps5/bench/Makefile  ->  sharp_bench.elf
// Run:   platform/ps5/scripts/run.sh platform/ps5/bench/sharp_bench.elf 900
// Title: the same source is one of the benches the bench title runs (built with
//        -DPS5_TITLE; docs/research/spike-a.md section 2.4).
//
// Exit: 0 scene written, 1 inference/write failure, 2 bad setup (missing file).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "common/exif.h"
#include "common/status.h"
#include "ggml-vulkan.h"
#include "jobs/image_io.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/psplat.h"

#include "bench_common.h"
#include "bench_env.h"
#include "bench_title.h"
#include "ggml_logging.h"

using splat::bench::Watchdog;
using splat::bench::human_bytes;
using splat::bench::output_path;
using splat::bench::peak_rss_bytes;

namespace {

const char* const kTag = "sharp_bench";

#ifdef PS5_TITLE
// The title build logs through sceKernelDebugOutText (no stdout reader, no
// klog_puts), with the same text as the payload line.
void
log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void
log_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    splat::bench::title_vlog(fmt, args);
    va_end(args);
}
#else
using splat::bench::log_line;
#endif

}  // namespace

int
sharp_bench_run(int argc, char** argv) {
    Watchdog watchdog(kTag);
    splat::bench::apply_bench_radv_defaults(kTag);
    splat::bench::load_env_file(splat::bench::bench_path("bench.env"), kTag);
#ifdef PS5_TITLE
    splat::bench::install_title_ggml_logging(kTag);
#else
    splat::bench::install_ggml_logging(kTag);
#endif

    const std::string dir = splat::bench::bench_dir();
    std::string photo;
    if (argc > 1 && argv[1][0] != '\0') {
        photo = argv[1];
    } else {
        photo = splat::bench::first_existing(splat::bench::bench_path("photo.jpg"), {});
        if (photo.empty()) photo = splat::bench::first_image_in(dir);
    }
    if (photo.empty()) {
        log_line("%s: no photo in %s (expected photo.jpg or any *.jpg; push-data.sh sends one)", kTag,
                 dir.c_str());
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }

    const std::string requested = argc > 2 && argv[2][0] != '\0'
                                      ? argv[2]
                                      : splat::bench::model_path(
                                            "sharp_2572gikvuh-f16-v1.gguf");
    const splat::bench::SharpModelFile model = splat::bench::resolve_sharp_gguf(requested);
    if (model.path.empty()) {
        // The f32 parity file is 2.6 GB but usable; it is preferred over failing
        // when it is the only SHARP GGUF on the console.
        const std::string fp32 = splat::bench::model_path("sharp_2572gikvuh-f32-v1.gguf");
        log_line("%s: no SHARP GGUF at %s or %s (push-data.sh sends the f16 file)", kTag,
                 requested.c_str(), fp32.c_str());
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }
    if (model.f32_fallback) {
        log_line("%s: %s missing, using %s (f32: 2.6 GB, slower)", kTag, requested.c_str(),
                 model.path.c_str());
    }
    const std::string& gguf = model.path;

    log_line("%s: start (ps5-splat-vr Spike A, SHARP stages as built)", kTag);
    log_line("%s: photo %s", kTag, photo.c_str());
    log_line("%s: gguf %s", kTag, gguf.c_str());

    const double t_decode = splat::bench::now_seconds();
    splat::Result<splat::Image> image = splat::decode_image(splat::Path(photo));
    if (!image.ok()) {
        log_line("%s: cannot decode %s: %s", kTag, photo.c_str(), image.message().c_str());
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }
    log_line("%s: decoded %dx%d in %.3f s", kTag, image.value().width, image.value().height,
             splat::bench::now_seconds() - t_decode);

    splat::ReconstructionInput input;
    input.rgb = std::move(image.value());
    splat::Result<std::vector<uint8_t>> bytes = splat::read_file_bytes(splat::Path(photo));
    if (bytes.ok()) {
        splat::Result<float> focal = splat::focal_px_from_jpeg(bytes.value().data(), bytes.value().size(),
                                                             input.rgb.width, input.rgb.height);
        if (focal.ok()) {
            input.focal_px = focal.value();
            log_line("%s: focal %.1f px (EXIF)", kTag, focal.value());
        } else {
            log_line("%s: focal: %s", kTag, focal.message().c_str());
        }
    }

    // Vulkan first: a missing device is a setup failure, not a slow CPU run,
    // and ggml_backend_vk_get_device_memory() asserts a device exists.
    ggml_backend_dev_t vk_dev = ggml_backend_dev_by_name("Vulkan0");
    if (vk_dev == nullptr) {
        log_line("%s: no Vulkan device (RADV init failed; see the [radv] lines above)", kTag);
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }
    log_line("%s: device %s", kTag, ggml_backend_dev_description(vk_dev));

    size_t heap_free = 0, heap_total = 0;
    ggml_backend_vk_get_device_memory(0, &heap_free, &heap_total);
    log_line("%s: heap before load: %s free of %s", kTag, human_bytes(heap_free).c_str(),
             human_bytes(heap_total).c_str());

    splat::SharpReconstructor reconstructor;
    const double t_load = splat::bench::now_seconds();
    watchdog.arm(900.0, "model load");
    splat::Status loaded = reconstructor.load(splat::Path(gguf));
    watchdog.disarm();
    const double load_seconds = splat::bench::now_seconds() - t_load;
    if (!loaded.ok()) {
        log_line("%s: load failed: %s", kTag, loaded.message().c_str());
        log_line("%s: FAIL load, leaking the GPU state and exiting 1 (no teardown)", kTag);
        splat::bench::abandon(kTag, 1);
    }
    log_line("%s: model loaded in %.2f s (%s, %s needed)",
             kTag, load_seconds, reconstructor.info().name.c_str(),
             human_bytes(reconstructor.info().vram_bytes).c_str());
    ggml_backend_vk_get_device_memory(0, &heap_free, &heap_total);
    log_line("%s: heap after load: %s free of %s", kTag, human_bytes(heap_free).c_str(),
             human_bytes(heap_total).c_str());

    double stage_start = splat::bench::now_seconds();
    std::string stage_name;
    splat::Progress progress([&](float fraction, const std::string& stage) {
        // Only stage transitions are logged: the splat assembly reports every
        // row of the grid, which would be thousands of klog lines.
        if (!stage_name.empty() && stage == stage_name) return;
        const double now = splat::bench::now_seconds();
        if (!stage_name.empty()) {
            log_line("%s: stage %-22s %7.3f s  (overall %.0f%%)", kTag, stage_name.c_str(),
                     now - stage_start, fraction * 100.0f);
        }
        stage_name = stage.empty() ? "run" : stage;
        stage_start = now;
    });

    splat::bench::reset_submits();
    watchdog.arm(1800.0, "SHARP inference");
    const double t_run = splat::bench::now_seconds();
    splat::Result<splat::SplatScene> scene = reconstructor.run(input, progress);
    const double run_seconds = splat::bench::now_seconds() - t_run;
    watchdog.disarm();
    if (!scene.ok()) {
        // The GPU may be wedged (device lost / compute error): log and exit
        // without running any destructor, so no buffer, model weight or
        // backend is freed under work that may still be in flight.
        log_line("%s: inference failed: %s", kTag, scene.message().c_str());
        log_line("%s: FAIL run, leaking the GPU state and exiting 1 (no teardown)", kTag);
        splat::bench::abandon(kTag, 1);
    }
    log_line("%s: stage %-22s %7.3f s  (overall 100%%)", kTag, stage_name.c_str(),
             splat::bench::now_seconds() - stage_start);

    const splat::SharpTimings& timings = reconstructor.last_timings();
    // A stage that is not in this tree stays at 0.0 s; the line is printed
    // anyway so a newer tree shows up without a change to this payload.
    log_line("%s: timings preprocess %.2f s, encoder %.2f s, depth %.2f s, initialiser %.2f s, "
             "gaussians %.2f s, composer %.2f s, total %.2f s",
             kTag, timings.preprocess, timings.encoder, timings.depth_decoder, timings.initializer,
             timings.gaussian_decoder, timings.composer, timings.total);
    log_line("%s: inference %.2f s, %d submits, %zu splats, focal %.1f px", kTag, run_seconds,
             splat::bench::submits(), scene.value().count(), reconstructor.last_focal_px());
    log_line("%s: peak RSS after inference %s", kTag, human_bytes(peak_rss_bytes()).c_str());
    ggml_backend_vk_get_device_memory(0, &heap_free, &heap_total);
    log_line("%s: heap after run: %s free of %s", kTag, human_bytes(heap_free).c_str(),
             human_bytes(heap_total).c_str());

    const std::string out = output_path(photo, ".sharp.psplat");
    watchdog.arm(180.0, "write_psplat");
    splat::Status written = splat::write_psplat(splat::Path(out), scene.value(),
                                               reconstructor.last_focal_px(),
                                               static_cast<uint32_t>(input.rgb.width),
                                               static_cast<uint32_t>(input.rgb.height));
    watchdog.disarm();
    if (!written.ok()) {
        log_line("%s: cannot write %s: %s", kTag, out.c_str(), written.message().c_str());
        log_line("%s: FAIL write, exit 1", kTag);
        return 1;
    }
    log_line("%s: wrote %s (%s)", kTag, out.c_str(),
             human_bytes(splat::psplat_file_size(scene.value().count())).c_str());

    reconstructor.unload();
    log_line("%s: model unloaded", kTag);
    log_line("%s: peak RSS %s", kTag, human_bytes(peak_rss_bytes()).c_str());
    log_line("%s: PASS", kTag);
    log_line("%s: done, exit 0", kTag);
    return 0;
}

#ifndef PS5_TITLE
// The payload build's own main; the title links this file with -DPS5_TITLE and
// calls sharp_bench_run from bench_title.cpp (the BENCH= selector).
int
main(int argc, char** argv) {
    return sharp_bench_run(argc, argv);
}
#endif
