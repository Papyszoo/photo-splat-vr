#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/image.h"
#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "recon/ggml/backend.h"
#include "recon/reconstructor.h"
#include "recon/sharp/sharp_depth_decoder.h"
#include "recon/sharp/sharp_encoder.h"
#include "recon/sharp/sharp_gaussian_decoder.h"
#include "recon/sharp/sharp_model.h"
#include "scene/splat_scene.h"

namespace splat {

// C3: the SHARP backend behind the C2 interface. Runs stages 0-6 in order and
// keeps one SharpModel (one copy of the weights) for all three networks:
//
//   decode + 1536^2 resize (stage 0)  -> SharEncoder (stage 2)
//   -> SharpDepthDecoder (stages 3 + 4: the adjustment U-Net is skipped)
//   -> initialiser + SharpGaussianDecoder (stages 5 + 6a)
//   -> composer + unprojection into a SplatScene (stage 6b)
//
// The scene is in OpenCV axes with metric scale, which is what the renderer
// consumes (docs/research/sharp-reference.md §9).
struct SharpConfig {
    bool prefer_vulkan = true;
    SharpEncoderConfig encoder;
    SharpDepthConfig depth;
    SharpGaussianConfig gaussians;
};

// Per-stage wall times of the last run(), seconds. Model load is not included.
struct SharpTimings {
    double preprocess = 0.0;
    double encoder = 0.0;
    double depth_decoder = 0.0;
    double initializer = 0.0;
    double gaussian_decoder = 0.0;
    double composer = 0.0;
    double total = 0.0;
};

class SharpReconstructor : public IReconstructor {
public:
    explicit SharpReconstructor(SharpConfig config = {});
    ~SharpReconstructor() override;

    BackendInfo info() const override;
    Status load(const Path& gguf) override;
    Result<SplatScene> run(const ReconstructionInput& input, Progress& progress) override;
    void unload() override;

    const SharpConfig& config() const { return config_; }
    const SharpTimings& last_timings() const { return timings_; }
    // Layer-major monodepth of the last run (2 * 1536^2), for diagnostics.
    const std::vector<float>& last_monodepth() const { return last_monodepth_; }
    // Focal length in original-image pixels used by the last run (EXIF or the
    // 30 mm default the job runner supplies).
    float last_focal_px() const { return last_focal_px_; }

private:
    SharpConfig config_;
    std::shared_ptr<SharpModel> model_;
    std::unique_ptr<SharpEncoder> encoder_;
    std::unique_ptr<SharpDepthDecoder> depth_;
    std::unique_ptr<SharpGaussianDecoder> gaussians_;
    SharpTimings timings_;
    std::vector<float> last_monodepth_;
    float last_focal_px_ = 0.0f;
};

}  // namespace splat
