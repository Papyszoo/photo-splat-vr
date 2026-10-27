// One SHARP model, kept loaded for a batch of photos inside the viewer title.
//
// This is the converter's engine half, build-only for now: W18 decides when the
// title opens it (in-process next to the renderer, or in a relaunched convert
// mode) and who runs it. It is deliberately the same per-photo sequence as
// platform/ps5/bench/convert_inbox.cpp and core/jobs/job_runner.cpp: decode
// (core/jobs/image_io), focal length (EXIF or SHARP's 30 mm default), the C2
// backend (SharpReconstructor load/run/unload), then write_scene_folder with
// original.jpg, thumb.jpg, scene.psplat and meta.json -- so a scene converted
// in the viewer is the same folder a console bench or the desktop writes.
//
// open() is the expensive step (a few GB of weights); run() may be called many
// times for one open() and fails instead of throwing once the model is gone.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"

namespace splat {
class SharpReconstructor;
}

namespace viewer {

// What one photo produced: its wall time (decode through scene folder, model
// load excluded), the splat count and a one-line summary for the UI log.
struct ConvertOutcome {
    double seconds = 0.0;
    uint32_t splats = 0;
    std::string message;
};

class ConvertSession {
public:
    ConvertSession();
    ~ConvertSession();

    ConvertSession(const ConvertSession&) = delete;
    ConvertSession& operator=(const ConvertSession&) = delete;

    // Loads the GGUF. A session that is already open unloads first (the
    // reconstructor does that itself); `gguf` is the model file, not a folder.
    splat::Status open(const splat::Path& gguf);

    // Decodes `photo`, reconstructs it and writes the scene folder. A null
    // `progress` is valid. `album_unused` is the caller's album choice: the
    // scene folder already is the photo's place, so this call never needs it.
    splat::Result<ConvertOutcome> run(const splat::Path& photo, const splat::Path& out_scene_folder,
                                      const std::string& album_unused,
                                      splat::Progress* progress);

    // Unloads the model; leaves the session closed. Safe to call twice.
    void close();

    bool is_open() const;

private:
    std::unique_ptr<splat::SharpReconstructor> reconstructor_;
};

}  // namespace viewer
