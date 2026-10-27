#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/image.h"
#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "scene/splat_scene.h"

namespace splat {

struct ReconstructionInput {
    Image rgb;
    // Focal length in original-image pixels, from EXIF or a user override.
    // When empty the backend estimates it, or assumes a default field of view.
    std::optional<float> focal_px;
};

struct BackendInfo {
    std::string name;
    std::string license;
    uint64_t vram_bytes = 0;
};

// C2. One interface for the SHARP and fallback backends. load() takes the
// converted GGUF; run() must be called only between load() and unload().
class IReconstructor {
public:
    virtual ~IReconstructor() = default;

    virtual BackendInfo info() const = 0;
    virtual Status load(const Path& gguf) = 0;
    virtual Result<SplatScene> run(const ReconstructionInput& input, Progress& progress) = 0;
    virtual void unload() = 0;
};

}  // namespace splat
