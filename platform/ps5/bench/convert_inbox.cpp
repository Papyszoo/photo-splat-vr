// Convert the console inbox: every photo in /data/ps5-splat-vr/inbox that has no
// scene folder yet, through the same pieces the desktop job runner uses.
//
// This is the batch mode of the one homebrew title (BENCH=convert in
// /data/ps5-splat-vr/bench/bench.env), not a payload bench: like op_audit and
// sharp_bench it needs a title's direct-memory pool for RADV.
//
//   inbox  /data/ps5-splat-vr/inbox/*.jpg|*.jpeg|*.png   (sorted by name)
//   scenes /data/ps5-splat-vr/scenes/<photo stem>/       (C5 folder per photo)
//   model  /data/ps5-splat-vr/models/sharp_2572gikvuh-f16-v1.gguf (f32 fallback)
//
// One photo is: decode (core/jobs/image_io), focal length (focal_px_from_photo:
// EXIF or SHARP's 30 mm default), the C2 backend (SharpReconstructor load/run/
// unload), then write_scene_folder with original.jpg, thumb.jpg, scene.psplat
// and meta.json -- the same calls core/jobs/job_runner.cpp makes, so a console
// scene and a desktop scene are the same file. The model is loaded once for the
// batch and unloaded at the end.
//
// Rules that make a re-run safe:
//   - a photo whose scene folder exists is skipped, never overwritten (checked
//     again right before writing, in case something else created it);
//   - nothing in the inbox is deleted, moved or renamed;
//   - a photo that fails is logged and skipped, and the batch continues.
//
// Exit: 0 every pending photo converted (or nothing pending), 1 at least one
// photo failed, 2 bad setup (no inbox directory, no Vulkan device, no model).
// A wedged GPU is not recovered in-process: the Watchdog abandons the process
// with the busy GPU left alone, as every other console bench does.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "ggml-vulkan.h"
#include "jobs/image_io.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

#include "bench_common.h"
#include "bench_env.h"
#include "bench_title.h"
#include "ggml_logging.h"

using splat::Image;
using splat::Path;
using splat::ReconstructionInput;
using splat::Result;
using splat::SceneMeta;
using splat::SharpReconstructor;
using splat::SplatScene;
using splat::Status;
using splat::bench::Watchdog;
using splat::bench::human_bytes;
using splat::bench::peak_rss_bytes;

namespace {

const char* const kTag = "convert";

// The job runner's defaults (JobRunnerConfig): a 512 px thumb.jpg and quality
// 90 for both JPEGs, so convert mode writes the same folder the desktop does.
constexpr int kThumbnailMaxEdge = 512;
constexpr int kJpegQuality = 90;

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

std::string env_dir(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

struct InboxPhoto {
    std::string name;
    Path path;
};

// *.jpg, *.jpeg and *.png in `dir`, sorted by name. Anything else (a .heic, a
// leftover note) is ignored: the inbox is shared with the uploader, not owned by
// this mode.
std::vector<InboxPhoto> list_photos(const Path& dir) {
    std::vector<InboxPhoto> photos;
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(dir, error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error) continue;
        const std::string name = it->path().filename().string();
        const bool wanted = splat::bench::has_suffix(name, ".jpg") ||
                            splat::bench::has_suffix(name, ".jpeg") ||
                            splat::bench::has_suffix(name, ".png");
        if (wanted) photos.push_back({name, it->path()});
    }
    std::sort(photos.begin(), photos.end(),
              [](const InboxPhoto& a, const InboxPhoto& b) { return a.name < b.name; });
    return photos;
}

double seconds_since(double started) { return splat::bench::now_seconds() - started; }

}  // namespace

int
convert_inbox_run(int argc, char** argv) {
    Watchdog watchdog(kTag);
    splat::bench::apply_bench_radv_defaults(kTag);
    splat::bench::load_env_file(splat::bench::bench_path("bench.env"), kTag);
#ifdef PS5_TITLE
    splat::bench::install_title_ggml_logging(kTag);
#else
    splat::bench::install_ggml_logging(kTag);
#endif

    const Path inbox(env_dir("PS5_INBOX_DIR", "/data/ps5-splat-vr/inbox"));
    const Path scenes(env_dir("PS5_SCENES_DIR", "/data/ps5-splat-vr/scenes"));
    log_line("%s: inbox %s, scenes %s", kTag, inbox.c_str(), scenes.c_str());

    std::error_code error;
    if (!std::filesystem::is_directory(inbox, error) || error) {
        log_line("%s: no inbox directory %s (convert-inbox.sh creates and fills it)", kTag,
                 inbox.c_str());
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }

    const std::vector<InboxPhoto> photos = list_photos(inbox);
    std::vector<InboxPhoto> pending;
    size_t skipped = 0;
    for (const InboxPhoto& photo : photos) {
        const Path folder = scenes / splat::bench::stem_of(photo.name);
        std::error_code exists_error;
        // A scene folder is complete once meta.json is there (its writer puts it
        // last), but the folder itself is the marker: any existing folder for a
        // photo is left exactly as it is.
        if (std::filesystem::exists(folder, exists_error) && !exists_error) {
            log_line("%s: skip %s (scene folder %s exists)", kTag, photo.name.c_str(),
                     folder.c_str());
            ++skipped;
            continue;
        }
        pending.push_back(photo);
    }

    if (pending.empty()) {
        log_line("%s: %zu photo(s) in the inbox, nothing to convert (%zu already have a scene)", kTag,
                 photos.size(), skipped);
        log_line("%s: PASS", kTag);
        log_line("%s: done, exit 0", kTag);
        return 0;
    }

    const std::string requested = argc > 2 && argv[2][0] != '\0'
                                      ? std::string(argv[2])
                                      : splat::bench::model_path("sharp_2572gikvuh-f16-v1.gguf");
    const splat::bench::SharpModelFile model = splat::bench::resolve_sharp_gguf(requested);
    if (model.path.empty()) {
        log_line("%s: no SHARP GGUF at %s or %s (push-data.sh sends the f16 file)", kTag,
                 requested.c_str(), splat::bench::model_path("sharp_2572gikvuh-f32-v1.gguf").c_str());
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }
    if (model.f32_fallback) {
        log_line("%s: %s missing, using %s (f32: 2.6 GB, slower)", kTag, requested.c_str(),
                 model.path.c_str());
    }
    log_line("%s: model %s", kTag, model.path.c_str());

    // Vulkan first: a missing device is a setup failure, not a slow CPU run.
    ggml_backend_dev_t vk_dev = ggml_backend_dev_by_name("Vulkan0");
    if (vk_dev == nullptr) {
        log_line("%s: no Vulkan device (RADV init failed; see the [radv] lines above)", kTag);
        log_line("%s: FAIL setup, exit 2", kTag);
        return 2;
    }
    log_line("%s: device %s", kTag, ggml_backend_dev_description(vk_dev));

    size_t heap_free = 0;
    size_t heap_total = 0;
    ggml_backend_vk_get_device_memory(0, &heap_free, &heap_total);
    log_line("%s: heap before load: %s free of %s", kTag, human_bytes(heap_free).c_str(),
             human_bytes(heap_total).c_str());

    SharpReconstructor reconstructor;
    const double t_load = splat::bench::now_seconds();
    watchdog.arm(900.0, "model load");
    const Status loaded = reconstructor.load(Path(model.path));
    watchdog.disarm();
    if (!loaded.ok()) {
        log_line("%s: load failed: %s", kTag, loaded.message().c_str());
        log_line("%s: FAIL load, leaking the GPU state and exiting 1 (no teardown)", kTag);
        splat::bench::abandon(kTag, 1);
    }
    log_line("%s: model loaded in %.2f s (%s, %s needed)", kTag, seconds_since(t_load),
             reconstructor.info().name.c_str(),
             human_bytes(reconstructor.info().vram_bytes).c_str());
    ggml_backend_vk_get_device_memory(0, &heap_free, &heap_total);
    log_line("%s: heap after load: %s free of %s", kTag, human_bytes(heap_free).c_str(),
             human_bytes(heap_total).c_str());

    const double batch_start = splat::bench::now_seconds();
    const int total = static_cast<int>(pending.size());
    int converted = 0;
    int failed = 0;
    for (int index = 0; index < total; ++index) {
        const InboxPhoto& photo = pending[static_cast<size_t>(index)];
        const Path folder = scenes / splat::bench::stem_of(photo.name);
        log_line("%s: photo %s (%d/%d)", kTag, photo.name.c_str(), index + 1, total);

        const double t_photo = splat::bench::now_seconds();
        // One line per photo whatever happens: name, seconds, splats, result.
        auto finish = [&](const char* result, size_t splats, const std::string& detail) {
            log_line("%s: photo %s: %.2f s, %zu splats, %s%s%s", kTag, photo.name.c_str(),
                     seconds_since(t_photo), splats, result, detail.empty() ? "" : ": ",
                     detail.c_str());
        };
        auto fail = [&](const std::string& reason) {
            ++failed;
            finish("FAILED", 0, reason);
        };

        // Both image outputs are made before the RGB buffer moves into the
        // backend, as the job runner does.
        const Result<std::vector<uint8_t>> read = splat::read_file_bytes(photo.path);
        if (!read.ok()) {
            fail("cannot read the photo: " + read.message());
            continue;
        }
        const std::vector<uint8_t>& file_bytes = read.value();
        Result<Image> decoded = splat::decode_image(file_bytes.data(), file_bytes.size());
        if (!decoded.ok()) {
            fail("cannot decode the photo: " + decoded.message());
            continue;
        }
        Image image = decoded.take();
        const int width = image.width;
        const int height = image.height;
        const bool source_is_jpeg = splat::looks_like_jpeg(file_bytes.data(), file_bytes.size());

        const Result<float> focal =
            splat::focal_px_from_photo(file_bytes.data(), file_bytes.size(), width, height);
        if (!focal.ok()) {
            fail("cannot compute the focal length: " + focal.message());
            continue;
        }

        const Result<Image> thumbnail = splat::make_thumbnail(image, kThumbnailMaxEdge);
        if (!thumbnail.ok()) {
            fail("cannot build the thumbnail: " + thumbnail.message());
            continue;
        }
        const Result<std::vector<uint8_t>> thumbnail_jpeg =
            splat::encode_jpeg(thumbnail.value(), kJpegQuality);
        if (!thumbnail_jpeg.ok()) {
            fail("cannot encode the thumbnail: " + thumbnail_jpeg.message());
            continue;
        }

        std::vector<uint8_t> original_jpeg;
        if (source_is_jpeg) {
            original_jpeg = file_bytes;  // keep the source JPEG verbatim
        } else {
            const Result<std::vector<uint8_t>> encoded = splat::encode_jpeg(image, kJpegQuality);
            if (!encoded.ok()) {
                fail("cannot encode the original: " + encoded.message());
                continue;
            }
            original_jpeg = encoded.value();
        }

        ReconstructionInput input;
        input.rgb = std::move(image);
        input.focal_px = focal.value();

        splat::bench::reset_submits();
        watchdog.arm(1800.0, "SHARP inference");
        const double t_run = splat::bench::now_seconds();
        splat::Progress progress;
        Result<SplatScene> scene = reconstructor.run(input, progress);
        const double run_seconds = seconds_since(t_run);
        watchdog.disarm();
        if (!scene.ok()) {
            fail("inference failed: " + scene.message());
            continue;
        }
        if (!scene.value().consistent()) {
            fail("reconstruction returned an inconsistent scene");
            continue;
        }

        // Never overwrite a scene folder: something may have created it since
        // the batch started (another convert run, a manual copy).
        std::error_code exists_error;
        if (std::filesystem::exists(folder, exists_error) && !exists_error) {
            ++skipped;
            finish("skipped", scene.value().count(), "the scene folder appeared during the run");
            continue;
        }

        SceneMeta meta;
        meta.backend = reconstructor.info().name.empty() ? kTag : reconstructor.info().name;
        meta.intrinsics.width = static_cast<uint32_t>(width);
        meta.intrinsics.height = static_cast<uint32_t>(height);
        meta.intrinsics.fx = focal.value();
        meta.intrinsics.fy = focal.value();
        meta.intrinsics.cx = static_cast<float>(width) / 2.0f;
        meta.intrinsics.cy = static_cast<float>(height) / 2.0f;
        meta.seconds_taken = seconds_since(t_photo);

        watchdog.arm(180.0, "write_scene_folder");
        const Status written = splat::write_scene_folder(folder, scene.value(), meta, original_jpeg,
                                                         thumbnail_jpeg.value());
        watchdog.disarm();
        if (!written.ok()) {
            fail("cannot write the scene folder: " + written.message());
            continue;
        }
        ++converted;
        char detail[512];
        std::snprintf(detail, sizeof(detail),
                      "wrote %s, inference %.2f s, %d submits, focal %.1f px", folder.c_str(),
                      run_seconds, splat::bench::submits(), focal.value());
        finish("ok", scene.value().count(), detail);
    }

    reconstructor.unload();
    log_line("%s: model unloaded", kTag);
    log_line("%s: %d converted, %zu skipped, %d failed in %.1f s", kTag, converted, skipped, failed,
             seconds_since(batch_start));
    log_line("%s: peak RSS %s", kTag, human_bytes(peak_rss_bytes()).c_str());
    if (failed != 0) {
        log_line("%s: FAIL %d photo(s)", kTag, failed);
        log_line("%s: done, exit 1", kTag);
        return 1;
    }
    log_line("%s: PASS", kTag);
    log_line("%s: done, exit 0", kTag);
    return 0;
}
