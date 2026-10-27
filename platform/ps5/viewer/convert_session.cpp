// The per-photo half of the in-app converter: see convert_session.h.
//
// The sequence below is the one convert_inbox.cpp runs for each inbox photo and
// job_runner.cpp runs for each queued photo, in the same order and with the same
// defaults (512 px thumb.jpg, JPEG quality 90, the source JPEG kept verbatim).
// The only things it does not copy are the bench's console concerns (watchdog,
// submit counter, skip-if-the-folder-exists, logging): those belong to whoever
// calls run() here.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "convert_session.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include <ps5platform/klog.h>

#include "common/image.h"
#include "jobs/image_io.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

using splat::BackendInfo;
using splat::Image;
using splat::Path;
using splat::ReconstructionInput;
using splat::Result;
using splat::SceneMeta;
using splat::SharpReconstructor;
using splat::SplatScene;
using splat::Status;

namespace viewer {

namespace {

// The job runner's defaults (JobRunnerConfig) and convert_inbox.cpp's: the two
// JPEGs share one quality, and thumb.jpg is at most 512 px on its long edge.
constexpr int kThumbnailMaxEdge = 512;
constexpr int kJpegQuality = 90;

using Clock = std::chrono::steady_clock;

double seconds_since(const Clock::time_point& started) {
    return std::chrono::duration<double>(Clock::now() - started).count();
}

}  // namespace

ConvertSession::ConvertSession() = default;

ConvertSession::~ConvertSession() { close(); }

Status ConvertSession::open(const Path& gguf) {
    // A second open() replaces the model; SharpReconstructor::load unloads first.
    if (!reconstructor_) reconstructor_ = std::make_unique<SharpReconstructor>();
    const Status loaded = reconstructor_->load(gguf);
    if (!loaded.ok()) {
        reconstructor_.reset();
        return loaded;
    }
    return Status::success();
}

Result<ConvertOutcome> ConvertSession::run(const Path& photo, const Path& out_scene_folder,
                                           const std::string& album_unused,
                                           splat::Progress* progress) {
    // One scene per photo: the folder is the destination, so the album has no
    // say here. Named in the signature so a caller cannot forget it exists.
    (void)album_unused;

    if (!reconstructor_) {
        return Result<ConvertOutcome>::failure("the converter has no model open");
    }

    const Clock::time_point started = Clock::now();

    const Result<std::vector<uint8_t>> read = splat::read_file_bytes(photo);
    if (!read.ok()) {
        return Result<ConvertOutcome>::failure("cannot read the photo: " + read.message());
    }
    const std::vector<uint8_t>& file_bytes = read.value();

    Result<Image> decoded = splat::decode_image(file_bytes.data(), file_bytes.size());
    if (!decoded.ok()) {
        return Result<ConvertOutcome>::failure("cannot decode the photo: " + decoded.message());
    }
    Image image = decoded.take();
    const int width = image.width;
    const int height = image.height;
    const bool source_is_jpeg = splat::looks_like_jpeg(file_bytes.data(), file_bytes.size());

    const Result<float> focal =
        splat::focal_px_from_photo(file_bytes.data(), file_bytes.size(), width, height);
    if (!focal.ok()) {
        return Result<ConvertOutcome>::failure("cannot compute the focal length: " +
                                              focal.message());
    }

    // Both image outputs are made before the RGB buffer moves into the backend,
    // as the job runner and convert mode do.
    const Result<Image> thumbnail = splat::make_thumbnail(image, kThumbnailMaxEdge);
    if (!thumbnail.ok()) {
        return Result<ConvertOutcome>::failure("cannot build the thumbnail: " +
                                               thumbnail.message());
    }
    const Result<std::vector<uint8_t>> thumbnail_jpeg =
        splat::encode_jpeg(thumbnail.value(), kJpegQuality);
    if (!thumbnail_jpeg.ok()) {
        return Result<ConvertOutcome>::failure("cannot encode the thumbnail: " +
                                               thumbnail_jpeg.message());
    }

    std::vector<uint8_t> original_jpeg;
    if (source_is_jpeg) {
        original_jpeg = file_bytes;  // keep the source JPEG verbatim
    } else {
        const Result<std::vector<uint8_t>> encoded = splat::encode_jpeg(image, kJpegQuality);
        if (!encoded.ok()) {
            return Result<ConvertOutcome>::failure("cannot encode the original: " +
                                                   encoded.message());
        }
        original_jpeg = encoded.value();
    }

    ReconstructionInput input;
    input.rgb = std::move(image);
    input.focal_px = focal.value();

    splat::Progress discard;
    splat::Progress& sink = progress != nullptr ? *progress : discard;
    const Clock::time_point inference_started = Clock::now();
    Result<SplatScene> scene = reconstructor_->run(input, sink);
    const double inference_seconds = seconds_since(inference_started);
    if (!scene.ok()) {
        return Result<ConvertOutcome>::failure("inference failed: " + scene.message());
    }
    if (!scene.value().consistent()) {
        return Result<ConvertOutcome>::failure("reconstruction returned an inconsistent scene");
    }

    const BackendInfo backend = reconstructor_->info();
    SceneMeta meta;
    meta.backend = backend.name.empty() ? std::string("sharp") : backend.name;
    meta.intrinsics.width = static_cast<uint32_t>(width);
    meta.intrinsics.height = static_cast<uint32_t>(height);
    meta.intrinsics.fx = focal.value();
    meta.intrinsics.fy = focal.value();
    meta.intrinsics.cx = static_cast<float>(width) / 2.0f;
    meta.intrinsics.cy = static_cast<float>(height) / 2.0f;
    meta.seconds_taken = seconds_since(started);

    const Status written = splat::write_scene_folder(out_scene_folder, scene.value(), meta,
                                                     original_jpeg, thumbnail_jpeg.value());
    if (!written.ok()) {
        return Result<ConvertOutcome>::failure("cannot write the scene folder: " +
                                               written.message());
    }

    ConvertOutcome outcome;
    outcome.seconds = seconds_since(started);
    outcome.splats = static_cast<uint32_t>(scene.value().count());
    char message[512];
    std::snprintf(message, sizeof(message), "wrote %s: inference %.2f s, focal %.1f px",
                  out_scene_folder.c_str(), inference_seconds, focal.value());
    outcome.message = message;
    return Result<ConvertOutcome>::success(std::move(outcome));
}

void ConvertSession::close() {
    if (reconstructor_) reconstructor_->unload();
    reconstructor_.reset();
}

bool ConvertSession::is_open() const { return reconstructor_ != nullptr; }

}  // namespace viewer

// ---- the glue's klog line ---------------------------------------------------
//
// The title links splat_ps5_glue for ps5_vulkan.cpp's shims and for
// bench_env.cpp's --wrap'd graph submission, and bench_env.cpp's log_line()
// calls klog_puts: a payload imports that from a kernel module, a title cannot
// (platform/ps5/bench/bench_title.cpp bridges it the same way for the bench
// title). This is the one definition the link needs; a glue line then lands in
// the same klog stream as the viewer's own lines.
extern "C" int klog_puts(const char* text) {
    if (text == nullptr) return -1;
    // sceKernelDebugOutText writes exactly the bytes it is given, and
    // platform/ps5/ggml/ggml_logging.h has already stripped the newline ggml
    // ended its line with (it is written for payloads, where klog_puts is the
    // kernel import that adds one). Without this the title's ggml lines run
    // together with the viewer's own line that follows.
    char line[1024];
    std::snprintf(line, sizeof(line), "%s\n", text);
    return sceKernelDebugOutText(0, line);
}
