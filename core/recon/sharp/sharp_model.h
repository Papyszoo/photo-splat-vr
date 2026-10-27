#pragma once

#include <memory>
#include <string>

#include "common/path.h"
#include "common/status.h"
#include "recon/ggml/backend.h"
#include "recon/ggml/weights.h"

namespace splat {

// The converted SHARP checkpoint plus the ggml backend it lives in. The three
// network stages (encoder, depth decoder, Gaussian decoder) share one instance
// so a run keeps exactly one copy of the weights (1.31 GiB fp16, 2.44 GiB
// fp32) instead of one per stage.
//
// The stages are separate objects because they run at different times and have
// very different activation footprints; only the weights and the backend are
// shared.
class SharpModel {
public:
    SharpModel(const SharpModel&) = delete;
    SharpModel& operator=(const SharpModel&) = delete;

    // Reads `gguf` (architecture "sharp", converted by splat_convert).
    static Result<std::shared_ptr<SharpModel>> load(const Path& gguf, bool prefer_vulkan);

    GgmlBackend& backend() { return backend_; }
    const GgmlBackend& backend() const { return backend_; }
    GgmlWeights& weights() { return weights_; }
    const GgmlWeights& weights() const { return weights_; }
    const Path& path() const { return path_; }

private:
    SharpModel() = default;
    Status initialize(const Path& gguf, bool prefer_vulkan);

    // Declaration order matters: the weights hold a pointer to the backend.
    GgmlBackend backend_;
    GgmlWeights weights_;
    Path path_;
};

}  // namespace splat
