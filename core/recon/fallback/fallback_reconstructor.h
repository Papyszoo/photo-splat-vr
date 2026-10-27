#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "recon/ggml/depth_anything.h"
#include "recon/reconstructor.h"
#include "scene/splat_scene.h"

namespace splat {

// C4 tuning. The spec asks for a fixed "assumed scene depth" that the user can
// adjust in VR; the runtime keeps the mapping linear in inverse depth so a UI
// slider can scale the whole scene without re-running inference.
// Quality counters for the disocclusion fill, from the last run. A fill splat
// is a "ghost" when its colour is closer to the near side of the edge it was
// built from than to the far side: the mean colours of the nearer object and of
// the source's depth layer in the patch around the source pixel. Edges whose
// two sides look alike are not judged. Correct fill reads only the far side;
// the remainder on real photos is far-side texture (a far mean lifted by a
// bright rim the fill skips), 0-4 % of the fill samples.
struct FallbackFillStats {
    size_t fill_samples = 0;
    size_t ghost_samples = 0;
};

struct FallbackConfig {
    // Metric depth of the *median* pixel. Relative depth is inverse depth, so
    // depth_m = assumed_scene_depth_m * median(disparity) / disparity.
    float assumed_scene_depth_m = 3.0f;
    // Upper bound on metric depth. Relative depth decays toward zero on sky and
    // far background; without a cap those pixels become kilometre-wide splats.
    float max_depth_m = 100.0f;
    // Gaussians to emit; the sampling grid is chosen to land near this count.
    size_t target_splats = 500'000;
    // Opacity of a Gaussian whose neighbourhood has no depth step. Near-opaque:
    // the cells overlap by design, so a lower value would only let the grey
    // backdrop through and lift the blacks.
    float base_opacity = 0.99f;
    // Relative depth step that separates two layers. A grid cell whose
    // full-resolution depths span this much is split at its largest gap, and a
    // neighbour farther than `own_depth * (1 + step)` is the background of a
    // disocclusion. This replaces the old opacity fade: both sides are emitted
    // at their own depth, and the background is extended behind the edge.
    float drop_relative_step = 0.5f;
    // Network input short side (Depth Anything V2 reference value).
    int input_size = 518;
    bool prefer_vulkan = true;
    size_t nodes_per_submit = 64;
};

// C4 fallback: Depth Anything V2 Small relative depth -> Gaussians on a fixed
// grid, metric scale from `assumed_scene_depth_m`, colour from the photo. A
// grid cell that straddles a silhouette is split at full resolution into its
// two depth layers, and the background layer is extended (depth and colour)
// behind the foreground edge to fill the parallax gap a head move reveals.
//
// Output is a SplatScene in OpenCV axes (x right, y down, z forward) with the
// photo's own intrinsics; singular values are the pixel footprint at that
// depth, so the splats tile the photo exactly at the assumed plane.
class FallbackReconstructor : public IReconstructor {
public:
    explicit FallbackReconstructor(FallbackConfig config = {});
    ~FallbackReconstructor() override;

    BackendInfo info() const override;
    Status load(const Path& gguf) override;
    Result<SplatScene> run(const ReconstructionInput& input, Progress& progress) override;
    void unload() override;

    const FallbackConfig& config() const { return config_; }
    // Timing of the last run(), seconds; model load is not included.
    double last_run_seconds() const { return last_run_seconds_; }
    // Relative inverse-depth map of the last run, at the photo's resolution.
    const std::vector<float>& last_disparity() const { return last_disparity_; }
    int last_disparity_width() const { return last_disparity_width_; }
    int last_disparity_height() const { return last_disparity_height_; }
    const FallbackFillStats& last_fill_stats() const { return last_fill_stats_; }

private:
    FallbackConfig config_;
    std::unique_ptr<DepthAnythingV2> model_;
    double last_run_seconds_ = 0.0;
    std::vector<float> last_disparity_;
    int last_disparity_width_ = 0;
    int last_disparity_height_ = 0;
    FallbackFillStats last_fill_stats_;
};

// Bilinear resize matching F.interpolate(..., align_corners=True); used for the
// depth map and kept separate so the fallback test can exercise it.
std::vector<float> resize_bilinear_align_corners(const std::vector<float>& source, int width,
                                                int height, int out_width, int out_height);

// Exposed for tests: the sampling grid (columns, rows) for a photo and the
// Gaussian target.
void fallback_grid(int width, int height, size_t target_splats, int* columns, int* rows);

// Gaussian sigma as a fraction of the grid cell it covers. The renderer
// projects the x/y sigma to `fx * sigma / z` pixels, so at the capture pose a
// cell's footprint is independent of depth and this ratio decides the seams:
// with 0.72 two neighbours still cover their midpoint to 95.1 % at opacity
// 0.99 (`fallback footprint covers the seam between two cells`), which is the
// smallest sigma that keeps the backdrop out of the seams. The old 0.75 was
// safe but 4 % wider than needed; 0.5 brings the screen-door grid back.
inline constexpr float kFallbackFootprintOverlap = 0.72f;

// Head motion the disocclusion band is sized for, metres. The brief's ~10 cm
// (twice the ±5 cm verification poses) is the design point; the band is
// shortened proportionally, never widened, if the scene budget would overflow.
inline constexpr float kFallbackDisocclusionHeadMotionM = 0.10f;

// A near-side region narrower than this many grid cells (rods, posts, hair) is
// below the grid's resolution. It is not split into a near/far pair and gets
// no disocclusion fill: the cell keeps the single layer it had before the
// fill pass, so the capture pose cannot gain beads or extra splats there.
inline constexpr float kFallbackThinFeatureCells = 3.0f;

// Fill depth is pushed this fraction behind the far-side depth it was sampled
// from, so a fill splat can never z-fight with the surface layer behind it.
// It is a few surface z-sigmas (0.06 % of z at the 500 k grid) but small
// enough that the shorter parallax does not open a seam: at a 100 px band a
// 0.2 % margin slips 0.2 px, whereas the old 1 % margin left a 1 px
// pure-backdrop line along every silhouette at +5 cm.
inline constexpr float kFallbackFillDepthMargin = 0.002f;

// Width in photo pixels of the background hidden behind a foreground edge
// that a head move of `head_motion_m` reveals. The revealed strip is the
// parallax difference between the two depths.
float fallback_disocclusion_band_px(float focal_px, float foreground_z, float background_z,
                                    float head_motion_m = kFallbackDisocclusionHeadMotionM);

// Exposed for tests: the fronto-parallel pixel sigma of a Gaussian that covers
// a `cell_px`-wide grid cell at the capture pose.
inline float fallback_footprint_sigma_px(float cell_px) {
    return kFallbackFootprintOverlap * cell_px;
}

// Exposed for tests: the `.psplat` SH degree-0 coefficient for an 8-bit sRGB
// channel value. Inverse of the renderer's `0.5 + 0.2820948 * sh_dc` followed
// by the sRGB EOTF, used for a single-colour average.
float fallback_sh_dc_from_srgb_value(uint8_t value);

// Metric depth from a relative inverse-depth map (what DA2 predicts), with the
// same median -> assumed_scene_depth_m mapping `run()` applies: the median
// positive disparity lands on the assumed scene depth, the rest scale linearly.
// Values at or below the floor decode to the far plane (`max_depth_m`), so sky
// does not become a kilometre-wide splat. `run()` and the external-depth CLI
// both use this, so a converted model's depth map and a saved one produce the
// same scene.
std::vector<float> fallback_metric_depth_from_disparity(const std::vector<float>& disparity,
                                                        const FallbackConfig& config);

// Metric depth from a depth map that is only defined up to scale (DA3's
// non-metric output): the median positive value lands on
// assumed_scene_depth_m and the map scales linearly around it. A relative
// depth model has no metric anchor, so this is the fallback's own convention
// (`--scene-depth`, default 3 m) and the scene's absolute scale is arbitrary.
std::vector<float> fallback_metric_depth_from_relative_depth(
    const std::vector<float>& depth_up_to_scale, const FallbackConfig& config);

// Metric depth in metres, clamped to [0.05, max_depth_m]. A masked sky comes
// back as +inf from MoGe-2 and DA3, and a clamp alone would keep NaN, so any
// non-finite pixel becomes the far plane.
std::vector<float> fallback_metric_depth_from_metres(const std::vector<float>& depth_m,
                                                     const FallbackConfig& config);

// Build the Gaussian scene from a full-resolution metric depth map and the
// photo. `run()` uses this after inference; tests call it directly with a
// synthetic depth map. The network's smooth depth ramps at silhouettes are
// first snapped onto the near or far surface (`config.input_size` gives the
// ramp width at full resolution), so no splat floats between the two. The
// disocclusion band is sized for `fill_head_motion_m` metres of head travel
// and is disabled at 0 (surface layers only), which is how a test compares a
// scene with and without fill.
Result<SplatScene> fallback_scene_from_depth(const std::vector<float>& depth,
                                             const Image& image, float focal_px,
                                             const FallbackConfig& config,
                                             float fill_head_motion_m, Progress& progress,
                                             FallbackFillStats* stats = nullptr);

}  // namespace splat
