#include "recon/fallback/fallback_reconstructor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "common/exif.h"

namespace splat {
namespace {

constexpr float kShDcScale = 0.28209479177387814f;  // sqrt(1 / (4 * pi))
constexpr float kMinDepth = 0.05f;
constexpr float kAbsoluteMaxDepth = 1000.0f;
constexpr float kMinDisparity = 1e-3f;
// Both layers of a split cell must cover at least this fraction of the cell.
// A thinner sliver (the rigging on the gondola, a wire) is below the grid's
// resolution: emitting it as its own sub-cell Gaussian turns it into a bead
// chain, so the cell keeps the dominant surface instead.
constexpr float kMinSplitFraction = 0.2f;
// Fill Gaussians are one per block of kFillBlockCells x kFillBlockCells grid
// cells inside the band, with a footprint a little wider than the block (this
// fraction of its spacing): they are only ever seen through the parallax gap,
// and the wider tail is what keeps a one-pixel pure-backdrop line from opening
// at the silhouette as the foreground moves.
constexpr float kFillFootprintCells = 0.9f;
// The fill extends the far side's edge colour, so it carries no detail a
// surface-cell spacing would resolve. A band sized for the design head motion
// behind a 0.5 m person in front of 5 m shelves (photo03) needs 2.5 times the
// 5 % budget reserve at one sample per cell, and the budget then shortened it
// to half of what a 5 cm move uncovers; at one per 2x2 cells it fits whole.
constexpr int kFillBlockCells = 2;
// Below this alpha a cluster is a rounding artefact, not a visible surface.
constexpr float kMinComponentAlpha = 0.02f;
// A neighbour only this much farther already hides background that a 5 cm head
// move can reveal (its parallax is not zero), so the band starts here rather
// than at the full silhouette step. Kept below the split threshold so a soft
// depth ramp is not split into layers, only backed with fill. The same
// threshold classifies every full-resolution pixel as the near or far side of
// a depth edge.
constexpr float kFillTriggerStep = 0.25f;
// Depth Anything's map is smooth at a silhouette: the network output (short
// side `input_size`) is upsampled bilinearly, so a depth step becomes a ramp
// about this many network pixels wide whose pixels lie between the two
// surfaces. The ramp search radius is this width at full resolution.
constexpr float kDepthRampNetworkPx = 8.0f;
// A pixel is on such a ramp when, under the design head motion, its parallax
// differs from a 4-neighbour's by more than this many pixels. A continuous
// surface stretches far less than that (photo03's floor at 0.5 m: 0.5 px), a
// ramp ten times more (the same floor behind a person: 20-30 px per pixel).
constexpr float kRampParallaxPx = 1.0f;
// The ghost check only judges edges whose two sides differ by more than this
// squared linear-RGB distance (about sRGB grey 75 against 100).
constexpr float kGhostMinContrast = 0.01f;

float clamp_float(float value, float low, float high) {
    return std::min(std::max(value, low), high);
}

float median_of(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return values[middle];
}

// Exact sRGB EOTF for the 8-bit input, tabulated once.
const std::array<float, 256>& srgb8_to_linear_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (int index = 0; index < 256; ++index) {
            const float encoded = static_cast<float>(index) / 255.0f;
            values[static_cast<size_t>(index)] =
                encoded <= 0.04045f ? encoded / 12.92f
                                    : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    return table;
}

// Inverse of the sRGB EOTF, as ml-sharp applies it before writing the .ply.
float linear_to_srgb(float value) {
    value = clamp_float(value, 0.0f, 1.0f);
    if (value <= 0.0031308f) return 12.92f * value;
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// `.psplat` SH DC is sRGB-encoded: the renderer draws 0.5 + C0*sh_dc, clamps,
// then decodes with the sRGB EOTF before blending in linear light.
float sh_dc_from_linear(float linear) {
    return (linear_to_srgb(linear) - 0.5f) / kShDcScale;
}

// One depth layer found inside a grid cell: a contiguous run of similar
// full-resolution depths. A cell that straddles a silhouette yields two.
struct CellLevel {
    float z = 0.0f;      // weighted median of the run, metres
    float r = 0.0f;      // linear-light colour average of the run
    float g = 0.0f;
    float b = 0.0f;
    double px = 0.0;     // continuous pixel centroid of the covered area
    double py = 0.0;
    float weight = 0.0f; // covered area, photo pixels
};

struct CellStat {
    CellLevel levels[2];
    int level_count = 0;
    int surface = 0;       // index of the level with the larger covered area
    float total_weight = 0.0f;
    // Covered fraction of a single-level cell whose sub-cell sliver was
    // dropped: the remaining alpha is filled by the background layer behind it.
    float coverage = 1.0f;
    // The cell's near side is a thin feature (rod, post, hair). It keeps one
    // layer and is never backed with fill.
    bool thin_surface = false;
};

struct CellSample {
    float z = 0.0f;
    float weight = 0.0f;
    double px = 0.0;
    double py = 0.0;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    bool thin = false;
};

// Attributes of one Gaussian before they are split into the scene's
// struct-of-arrays. Rotation is always identity for the fallback.
struct Gaussian {
    float position[3];
    float log_scale[3];
    float sh_dc[3];
    float opacity_logit;
};

void append_gaussian(std::vector<Gaussian>* out, float x, float y, float z, float sigma_x,
                     float sigma_y, float sigma_z, float r, float g, float b, float alpha) {
    Gaussian splat;
    splat.position[0] = x;
    splat.position[1] = y;
    splat.position[2] = z;
    splat.log_scale[0] = std::log(std::max(sigma_x, 1e-4f));
    splat.log_scale[1] = std::log(std::max(sigma_y, 1e-4f));
    splat.log_scale[2] = std::log(std::max(sigma_z, 1e-4f));
    splat.sh_dc[0] = sh_dc_from_linear(r);
    splat.sh_dc[1] = sh_dc_from_linear(g);
    splat.sh_dc[2] = sh_dc_from_linear(b);
    const float clamped = clamp_float(alpha, 1e-4f, 1.0f - 1e-4f);
    splat.opacity_logit = std::log(clamped / (1.0f - clamped));
    out->push_back(splat);
}

float relative_depth_step(float a, float b) {
    const float low = std::max(std::min(a, b), kMinDepth);
    return std::fabs(a - b) / low;
}

// Snap Depth Anything's silhouette ramps onto the two surfaces they join. A
// ramp pixel would otherwise become a splat floating between the near object
// and the background ("flying pixel"): under a head move the ramp's splats
// separate from both surfaces and streak the revealed gap with the near
// object's colour, one line per iso-depth level (the zebra along people's
// outlines, a fringe along a rifle stock, a rim along a gondola).
//
// Each ramp pixel takes the depth of the nearest off-ramp pixel on the side it
// belongs to. The side is decided by its depth position along the ramp (where
// the network put the edge: the prior) and by colour (which side's mean colour
// it matches: the evidence), so a pixel that looks like the background goes
// to the background even where the network smeared the edge into it, and a
// pixel the depth clearly places on one side keeps it unless its colour
// clearly belongs to the other. A pixel with no off-ramp pixel on one side
// within `radius` (a rod thinner than the ramp) keeps its depth.
std::vector<float> snap_depth_ramps(const std::vector<float>& depth, const Image& image,
                                    float focal_px, int radius) {
    const int width = image.width;
    const int height = image.height;
    const float parallax_scale = focal_px * kFallbackDisocclusionHeadMotionM;
    std::vector<uint8_t> ramp(depth.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const float inverse = 1.0f / depth[index];
            float steepest = 0.0f;
            if (x + 1 < width) steepest = std::fabs(inverse - 1.0f / depth[index + 1]);
            if (x > 0) steepest = std::max(steepest, std::fabs(inverse - 1.0f / depth[index - 1]));
            if (y + 1 < height) {
                steepest = std::max(steepest, std::fabs(inverse - 1.0f / depth[index + width]));
            }
            if (y > 0) {
                steepest = std::max(steepest, std::fabs(inverse - 1.0f / depth[index - width]));
            }
            ramp[index] = steepest * parallax_scale > kRampParallaxPx ? 1 : 0;
        }
    }
    std::vector<std::array<int, 2>> offsets;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx != 0 || dy != 0) offsets.push_back({dx, dy});
        }
    }
    std::stable_sort(offsets.begin(), offsets.end(),
                     [](const std::array<int, 2>& a, const std::array<int, 2>& b) {
                         return a[0] * a[0] + a[1] * a[1] < b[0] * b[0] + b[1] * b[1];
                     });

    std::vector<float> snapped = depth;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (!ramp[index]) continue;
            const float z = depth[index];
            // Nearest off-ramp depth and mean sRGB colour on each side.
            float side_z[2] = {0.0f, 0.0f};
            std::array<int, 2> side_offset[2] = {{0, 0}, {0, 0}};
            bool found[2] = {false, false};
            float sum[2][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
            int count[2] = {0, 0};
            for (const std::array<int, 2>& offset : offsets) {
                const int nx = x + offset[0];
                const int ny = y + offset[1];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const size_t neighbour = static_cast<size_t>(ny) * width + nx;
                if (ramp[neighbour]) continue;
                const float nz = depth[neighbour];
                const uint8_t* pixel = image.pixel(nx, ny);
                for (int side = 0; side < 2; ++side) {
                    if (side == 0 ? nz > z : nz < z) continue;
                    if (!found[side]) {
                        side_z[side] = nz;
                        side_offset[side] = offset;
                        found[side] = true;
                    }
                    for (int channel = 0; channel < 3; ++channel) {
                        sum[side][channel] += pixel[channel];
                    }
                    ++count[side];
                }
            }
            if (!found[0] || !found[1]) continue;
            // The pixel must lie between its two anchors (more than 120 degrees
            // apart), as on a silhouette. A rod's pixel finds the body it hangs
            // from along the rod and the background beside it; snapping only
            // the part within reach of the body would split the rod in two.
            const float dot = static_cast<float>(side_offset[0][0] * side_offset[1][0] +
                                                 side_offset[0][1] * side_offset[1][1]);
            const float lengths = std::sqrt(static_cast<float>(
                (side_offset[0][0] * side_offset[0][0] + side_offset[0][1] * side_offset[0][1]) *
                (side_offset[1][0] * side_offset[1][0] + side_offset[1][1] * side_offset[1][1])));
            if (dot > -0.5f * lengths) continue;
            const uint8_t* pixel = image.pixel(x, y);
            float distance[2] = {0.0f, 0.0f};
            for (int side = 0; side < 2; ++side) {
                for (int channel = 0; channel < 3; ++channel) {
                    const float difference =
                        pixel[channel] - sum[side][channel] / static_cast<float>(count[side]);
                    distance[side] += difference * difference;
                }
            }
            // t: 0 on the near surface, 1 on the far one (log depth).
            const float span = std::log(side_z[1] / side_z[0]);
            const float t = span > 1e-6f ? std::log(z / side_z[0]) / span : 0.5f;
            const float total = distance[0] + distance[1];
            const float prefers_far = total > 0.0f ? (distance[0] - distance[1]) / total : 0.0f;
            snapped[index] = prefers_far < 1.0f - 2.0f * t ? side_z[0] : side_z[1];
        }
    }
    return snapped;
}

// Classify every full-resolution pixel that touches a depth edge as the near
// or the far side of that edge. Both sides of a pair are marked, so a pixel can
// carry both flags where two edges meet; the fill pass only ever reads the far
// flag as a colour/depth source and the near flag to grow the hidden region.
void classify_edge_pixels(const std::vector<float>& depth, int width, int height,
                          float jump_step, std::vector<uint8_t>* near_edge,
                          std::vector<uint8_t>* far_edge) {
    near_edge->assign(depth.size(), 0);
    far_edge->assign(depth.size(), 0);
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const float z = depth[index];
            for (int direction = 0; direction < 4; ++direction) {
                const int nx = x + kDx[direction];
                const int ny = y + kDy[direction];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const float neighbour = depth[static_cast<size_t>(ny) * width + nx];
                if (relative_depth_step(z, neighbour) <= jump_step) continue;
                if (z < neighbour) {
                    (*near_edge)[index] = 1;
                } else {
                    (*far_edge)[index] = 1;
                }
            }
        }
    }
}

// Grow the near-side edge pixels into the whole surface they belong to, so a
// wide foreground (a person, a wall) has interior pixels that the band can
// reach. Growth stops at a far-side edge pixel, so it never crosses the
// silhouette into the background object. It does not otherwise look at depth:
// a real surface ramps continuously, and cutting the growth at a soft gradient
// would fragment the near mask into thin strips.
void grow_near_regions(const std::vector<float>& depth, int width, int height,
                       const std::vector<uint8_t>& near_edge,
                       const std::vector<uint8_t>& far_edge,
                       std::vector<uint8_t>* near_region) {
    near_region->assign(depth.size(), 0);
    std::vector<int> queue;
    queue.reserve(depth.size() / 8 + 1);
    for (size_t index = 0; index < depth.size(); ++index) {
        if (!near_edge[index]) continue;
        (*near_region)[index] = 1;
        queue.push_back(static_cast<int>(index));
    }
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    size_t head = 0;
    while (head < queue.size()) {
        const int index = queue[head++];
        const int x = index % width;
        const int y = index / width;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + kDx[direction];
            const int ny = y + kDy[direction];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t neighbour = static_cast<size_t>(ny) * width + nx;
            if ((*near_region)[neighbour] || far_edge[neighbour]) continue;
            (*near_region)[neighbour] = 1;
            queue.push_back(static_cast<int>(neighbour));
        }
    }
}

// Mark near-side pixels whose region is narrower than `cell_px *
// kFallbackThinFeatureCells`: a rod, a post, a hair strand. A plain distance
// transform is not enough: every boundary pixel of a large object also has
// distance 1, and marking that rim thin would leave an unfilled one-cell strip
// at every silhouette. Instead erode to the thick core (half-width above 1.5
// cells) and dilate the core back by the same radius; only the pixels the
// dilated core does not reach are thin. A 3-cell-or-wider region keeps a core
// and is wholly thick, while a 1-2 cell rod has no core at all.
void mark_thin_regions(const std::vector<uint8_t>& near_region, int width, int height,
                       float cell_px, std::vector<uint8_t>* thin) {
    thin->assign(near_region.size(), 0);
    if (cell_px <= 0.0f) return;
    const float half_width = 0.5f * kFallbackThinFeatureCells * cell_px;
    const int radius = std::max(1, static_cast<int>(std::ceil(half_width)) + 1);
    constexpr int kUnreached = std::numeric_limits<int>::max();
    std::vector<int> distance(near_region.size(), kUnreached);
    std::vector<int> queue;
    queue.reserve(near_region.size() / 16 + 1);
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (!near_region[index]) continue;
            bool boundary = false;
            for (int direction = 0; direction < 4 && !boundary; ++direction) {
                const int nx = x + kDx[direction];
                const int ny = y + kDy[direction];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    boundary = true;
                } else if (!near_region[static_cast<size_t>(ny) * width + nx]) {
                    boundary = true;
                }
            }
            if (boundary) {
                distance[index] = 1;
                queue.push_back(static_cast<int>(index));
            }
        }
    }
    if (queue.empty()) return;
    size_t head = 0;
    while (head < queue.size()) {
        const int index = queue[head++];
        const int x = index % width;
        const int y = index / width;
        const int next = distance[static_cast<size_t>(index)] + 1;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + kDx[direction];
            const int ny = y + kDy[direction];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t neighbour = static_cast<size_t>(ny) * width + nx;
            if (!near_region[neighbour] || distance[neighbour] != kUnreached) continue;
            distance[neighbour] = next;
            queue.push_back(static_cast<int>(neighbour));
        }
    }
    // Erode to the thick core, then dilate the core back. `reach` counts down
    // from `radius`; a pixel still at 0 was not reached from any core pixel.
    std::vector<int> reach(near_region.size(), -1);
    queue.clear();
    for (size_t index = 0; index < near_region.size(); ++index) {
        if (near_region[index] && distance[index] != kUnreached &&
            static_cast<float>(distance[index]) > half_width) {
            reach[index] = radius;
            queue.push_back(static_cast<int>(index));
        }
    }
    head = 0;
    while (head < queue.size()) {
        const int index = queue[head++];
        const int remaining = reach[static_cast<size_t>(index)];
        if (remaining <= 1) continue;
        const int x = index % width;
        const int y = index / width;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + kDx[direction];
            const int ny = y + kDy[direction];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t neighbour = static_cast<size_t>(ny) * width + nx;
            if (!near_region[neighbour] || reach[neighbour] >= 0) continue;
            reach[neighbour] = remaining - 1;
            queue.push_back(static_cast<int>(neighbour));
        }
    }
    for (size_t index = 0; index < near_region.size(); ++index) {
        if (near_region[index] && reach[index] < 0) (*thin)[index] = 1;
    }
}

// Fill colour and depth, indexed by the near-side pixel they belong to. Both
// come from `source`, a full-resolution far-side edge pixel: the iterative
// dilation of the far-side layer under the near-side mask. `distance` is the
// pixel distance from that far side, used to stop the band at the parallax a
// head move actually reveals.
struct FillField {
    std::vector<int> source;
    std::vector<int> distance;
};

void build_fill_field(const std::vector<float>& depth, int width, int height,
                      const std::vector<uint8_t>& near_region,
                      const std::vector<uint8_t>& far_edge, FillField* field) {
    field->source.assign(depth.size(), -1);
    field->distance.assign(depth.size(), -1);
    std::vector<int> queue;
    queue.reserve(depth.size() / 16 + 1);
    for (size_t index = 0; index < depth.size(); ++index) {
        if (!far_edge[index]) continue;
        field->source[index] = static_cast<int>(index);
        field->distance[index] = 0;
        queue.push_back(static_cast<int>(index));
    }
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    size_t head = 0;
    while (head < queue.size()) {
        const int index = queue[head++];
        const int x = index % width;
        const int y = index / width;
        const int next = field->distance[static_cast<size_t>(index)] + 1;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + kDx[direction];
            const int ny = y + kDy[direction];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t neighbour = static_cast<size_t>(ny) * width + nx;
            // Only the near side is filled; the far side is never sampled as a
            // destination, and a source is never propagated through it.
            if (!near_region[neighbour] || field->distance[neighbour] >= 0) continue;
            field->source[neighbour] = field->source[static_cast<size_t>(index)];
            field->distance[neighbour] = next;
            queue.push_back(static_cast<int>(neighbour));
        }
    }
}

// 4-connected distance of every pixel from the nearest `seed` pixel, capped at
// `limit` (pixels farther away hold `limit`).
std::vector<int> capped_distance(const std::vector<uint8_t>& seed, int width, int height,
                                 int limit) {
    std::vector<int> distance(seed.size(), limit);
    std::vector<int> queue;
    for (size_t index = 0; index < seed.size(); ++index) {
        if (!seed[index]) continue;
        distance[index] = 0;
        queue.push_back(static_cast<int>(index));
    }
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    size_t head = 0;
    while (head < queue.size()) {
        const int index = queue[head++];
        const int next = distance[static_cast<size_t>(index)] + 1;
        if (next >= limit) continue;
        const int x = index % width;
        const int y = index / width;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + kDx[direction];
            const int ny = y + kDy[direction];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t neighbour = static_cast<size_t>(ny) * width + nx;
            if (distance[neighbour] <= next) continue;
            distance[neighbour] = next;
            queue.push_back(static_cast<int>(neighbour));
        }
    }
    return distance;
}

// Full-resolution grid cell statistics. The photo is split inside each cell
// into at most two depth layers, sampled at full resolution: a silhouette
// cell on the grid no longer collapses to one mid-depth average.
class CellBuilder {
public:
    CellBuilder(const std::vector<float>& depth, const Image& image,
                const std::array<float, 256>& srgb,
                const std::vector<uint8_t>& thin, int width, int height, double cell_x,
                double cell_y, float split_step)
        : depth_(depth),
          image_(image),
          srgb_(srgb),
          thin_(thin),
          width_(width),
          height_(height),
          cell_x_(cell_x),
          cell_y_(cell_y),
          split_step_(std::max(split_step, 1e-3f)) {}

    void compute(int column, int row, CellStat* out) const {
        out->level_count = 0;
        out->total_weight = 0.0f;
        out->coverage = 1.0f;
        const double left = column * cell_x_;
        const double top = row * cell_y_;
        const double right = left + cell_x_;
        const double bottom = top + cell_y_;
        const int x0 = std::min(std::max(static_cast<int>(std::floor(left)), 0), width_ - 1);
        const int x1 = std::min(std::max(static_cast<int>(std::ceil(right)), x0 + 1), width_);
        const int y0 = std::min(std::max(static_cast<int>(std::floor(top)), 0), height_ - 1);
        const int y1 = std::min(std::max(static_cast<int>(std::ceil(bottom)), y0 + 1), height_);

        samples_.clear();
        float total = 0.0f;
        for (int sample_y = y0; sample_y < y1; ++sample_y) {
            const double covered_top = std::max(static_cast<double>(sample_y), top);
            const double covered_bottom = std::min(sample_y + 1.0, bottom);
            const double weight_y = covered_bottom - covered_top;
            if (weight_y <= 0.0) continue;
            const double centre_y = 0.5 * (covered_top + covered_bottom);
            const uint8_t* line = image_.pixel(0, sample_y);
            for (int sample_x = x0; sample_x < x1; ++sample_x) {
                const double covered_left = std::max(static_cast<double>(sample_x), left);
                const double covered_right = std::min(sample_x + 1.0, right);
                const double weight_x = covered_right - covered_left;
                if (weight_x <= 0.0) continue;
                const float weight = static_cast<float>(weight_x * weight_y);
                const uint8_t* pixel = line + sample_x * 3;
                CellSample sample;
                sample.z = depth_[static_cast<size_t>(sample_y) * width_ + sample_x];
                sample.weight = weight;
                sample.px = 0.5 * (covered_left + covered_right);
                sample.py = centre_y;
                sample.r = srgb_[pixel[0]];
                sample.g = srgb_[pixel[1]];
                sample.b = srgb_[pixel[2]];
                sample.thin = thin_[static_cast<size_t>(sample_y) * width_ + sample_x] != 0;
                samples_.push_back(sample);
                total += weight;
            }
        }
        out->total_weight = total;
        if (samples_.empty() || total <= 0.0f) return;
        float thin_weight = 0.0f;
        for (const CellSample& sample : samples_) {
            if (sample.thin) thin_weight += sample.weight;
        }
        out->thin_surface = thin_weight > 0.5f * total;

        std::sort(samples_.begin(), samples_.end(),
                  [](const CellSample& a, const CellSample& b) { return a.z < b.z; });
        const int sample_count = static_cast<int>(samples_.size());

        // Split at the largest relative depth gap, but only when the cell's
        // total depth range is a real silhouette. Full-resolution sampling is
        // what keeps the silhouette from quantising to the grid.
        int split = -1;
        float best_gap = 0.0f;
        for (int index = 0; index + 1 < sample_count; ++index) {
            const float gap = (samples_[static_cast<size_t>(index) + 1].z -
                               samples_[static_cast<size_t>(index)].z) /
                              std::max(samples_[static_cast<size_t>(index)].z, kMinDepth);
            if (gap > best_gap) {
                best_gap = gap;
                split = index + 1;
            }
        }
        const float range =
            (samples_.back().z - samples_.front().z) / std::max(samples_.front().z, kMinDepth);
        if (split > 0 && range > split_step_ && best_gap > 0.5f * split_step_) {
            float near_weight = 0.0f;
            for (int index = 0; index < split; ++index) {
                near_weight += samples_[static_cast<size_t>(index)].weight;
            }
            const float near_fraction = near_weight / total;
            const float minor_fraction = std::min(near_fraction, 1.0f - near_fraction);
            float near_thin_weight = 0.0f;
            for (int index = 0; index < split; ++index) {
                const CellSample& sample = samples_[static_cast<size_t>(index)];
                if (sample.thin) near_thin_weight += sample.weight;
            }
            // A thin near-side feature (rod, post, hair) is narrower than the
            // grid's 3-cell resolution: splitting it gives a bead chain of
            // sub-cell Gaussians. Keep one opaque layer, as the cell had before
            // the fill pass, and never fill behind it.
            if (near_thin_weight > 0.5f * near_weight) {
                out->coverage = 1.0f;
                out->thin_surface = true;
                build_level(0, sample_count, &out->levels[0]);
                // Keep the soft whole-cell colour (so the 1 px rod does not
                // become a hard dark bar) but put the layer at the thin
                // feature's own depth. A whole-cell median can land on the sky
                // side in one cell and on the rod in the next; under a 5 cm
                // head move those neighbours then shift by different amounts
                // and the rod turns into a bead chain.
                float cumulative = 0.0f;
                float near_median = samples_[0].z;
                for (int index = 0; index < split; ++index) {
                    cumulative += samples_[static_cast<size_t>(index)].weight;
                    if (cumulative >= 0.5f * near_weight) {
                        near_median = samples_[static_cast<size_t>(index)].z;
                        break;
                    }
                }
                out->levels[0].z = near_median;
                out->level_count = 1;
                out->surface = 0;
                return;
            }
            if (minor_fraction >= kMinSplitFraction) {
                build_level(0, split, &out->levels[0]);
                build_level(split, sample_count, &out->levels[1]);
                out->level_count = 2;
                out->surface = out->levels[1].weight > out->levels[0].weight ? 1 : 0;
                return;
            }
            // A sub-cell sliver is below the grid's resolution. Emitting it as
            // its own sub-cell Gaussian turns a wire or the gondola rigging
            // into a bead chain, so this cell keeps the whole-cell average
            // (soft, like the photo's own blur) and only covers the dominant
            // fraction of the cell; the background band behind it supplies the
            // rest, which is what keeps the backdrop out (the old step fade
            // had no background layer to fill the gap).
            out->coverage = std::max(near_fraction, 1.0f - near_fraction);
        }
        build_level(0, sample_count, &out->levels[0]);
        out->level_count = 1;
        out->surface = 0;
    }

private:
    void build_level(int begin, int end, CellLevel* out) const {
        float weight = 0.0f;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        double px = 0.0;
        double py = 0.0;
        for (int index = begin; index < end; ++index) {
            const CellSample& sample = samples_[static_cast<size_t>(index)];
            weight += sample.weight;
            r += sample.weight * sample.r;
            g += sample.weight * sample.g;
            b += sample.weight * sample.b;
            px += sample.weight * sample.px;
            py += sample.weight * sample.py;
        }
        const double inverse = weight > 0.0f ? 1.0 / weight : 0.0;
        float cumulative = 0.0f;
        float median_z = samples_[static_cast<size_t>(begin)].z;
        for (int index = begin; index < end; ++index) {
            cumulative += samples_[static_cast<size_t>(index)].weight;
            if (cumulative >= 0.5f * weight) {
                median_z = samples_[static_cast<size_t>(index)].z;
                break;
            }
        }
        out->z = median_z;
        out->r = static_cast<float>(r * inverse);
        out->g = static_cast<float>(g * inverse);
        out->b = static_cast<float>(b * inverse);
        out->px = px * inverse;
        out->py = py * inverse;
        out->weight = weight;
    }

    const std::vector<float>& depth_;
    const Image& image_;
    const std::array<float, 256>& srgb_;
    const std::vector<uint8_t>& thin_;
    int width_;
    int height_;
    double cell_x_;
    double cell_y_;
    float split_step_;
    mutable std::vector<CellSample> samples_;
};

}  // namespace

float fallback_sh_dc_from_srgb_value(uint8_t value) {
    return sh_dc_from_linear(srgb8_to_linear_table()[value]);
}

float fallback_disocclusion_band_px(float focal_px, float foreground_z, float background_z,
                                    float head_motion_m) {
    if (focal_px <= 0.0f || foreground_z <= 0.0f) return 0.0f;
    const float behind = std::max(background_z, foreground_z);
    const float inverse_difference = 1.0f / foreground_z - 1.0f / behind;
    if (inverse_difference <= 0.0f) return 0.0f;
    return focal_px * head_motion_m * inverse_difference;
}

std::vector<float> resize_bilinear_align_corners(const std::vector<float>& source, int width,
                                                int height, int out_width, int out_height) {
    std::vector<float> out(static_cast<size_t>(out_width) * out_height, 0.0f);
    if (width <= 0 || height <= 0 || out_width <= 0 || out_height <= 0) return out;
    const double scale_x = out_width > 1 ? static_cast<double>(width - 1) / (out_width - 1) : 0.0;
    const double scale_y = out_height > 1 ? static_cast<double>(height - 1) / (out_height - 1) : 0.0;
    for (int y = 0; y < out_height; ++y) {
        const double source_y = y * scale_y;
        const int y0 = std::min(static_cast<int>(source_y), height - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const double fy = source_y - y0;
        for (int x = 0; x < out_width; ++x) {
            const double source_x = x * scale_x;
            const int x0 = std::min(static_cast<int>(source_x), width - 1);
            const int x1 = std::min(x0 + 1, width - 1);
            const double fx = source_x - x0;
            const double top = (1.0 - fx) * source[static_cast<size_t>(y0) * width + x0] +
                               fx * source[static_cast<size_t>(y0) * width + x1];
            const double bottom = (1.0 - fx) * source[static_cast<size_t>(y1) * width + x0] +
                                  fx * source[static_cast<size_t>(y1) * width + x1];
            out[static_cast<size_t>(y) * out_width + x] =
                static_cast<float>((1.0 - fy) * top + fy * bottom);
        }
    }
    return out;
}

void fallback_grid(int width, int height, size_t target_splats, int* columns, int* rows) {
    if (width <= 0 || height <= 0) {
        *columns = 0;
        *rows = 0;
        return;
    }
    const double aspect = static_cast<double>(width) / height;
    double column_count = std::sqrt(static_cast<double>(target_splats) * aspect);
    int gw = static_cast<int>(std::lround(column_count));
    gw = std::min(std::max(gw, 1), width);
    int gh = static_cast<int>(std::lround(static_cast<double>(target_splats) / gw));
    gh = std::min(std::max(gh, 1), height);
    *columns = gw;
    *rows = gh;
}

std::vector<float> fallback_metric_depth_from_disparity(const std::vector<float>& disparity,
                                                        const FallbackConfig& config) {
    std::vector<float> positive;
    positive.reserve(disparity.size());
    for (float value : disparity) {
        if (value > kMinDisparity) positive.push_back(value);
    }
    const float reference_disparity =
        positive.empty() ? kMinDisparity : std::max(median_of(std::move(positive)), kMinDisparity);
    const float scale = std::max(config.assumed_scene_depth_m, kMinDepth) * reference_disparity;
    const float max_depth = clamp_float(config.max_depth_m, kMinDepth, kAbsoluteMaxDepth);
    std::vector<float> depth(disparity.size());
    for (size_t index = 0; index < disparity.size(); ++index) {
        depth[index] =
            clamp_float(scale / std::max(disparity[index], kMinDisparity), kMinDepth, max_depth);
    }
    return depth;
}

std::vector<float> fallback_metric_depth_from_relative_depth(
    const std::vector<float>& depth_up_to_scale, const FallbackConfig& config) {
    std::vector<float> positive;
    positive.reserve(depth_up_to_scale.size());
    for (float value : depth_up_to_scale) {
        if (std::isfinite(value) && value > kMinDepth) positive.push_back(value);
    }
    const float reference =
        positive.empty() ? 1.0f : std::max(median_of(std::move(positive)), kMinDepth);
    const float scale = std::max(config.assumed_scene_depth_m, kMinDepth) / reference;
    std::vector<float> scaled(depth_up_to_scale.size());
    for (size_t index = 0; index < depth_up_to_scale.size(); ++index) {
        scaled[index] = depth_up_to_scale[index] * scale;
    }
    return fallback_metric_depth_from_metres(scaled, config);
}

std::vector<float> fallback_metric_depth_from_metres(const std::vector<float>& depth_m,
                                                     const FallbackConfig& config) {
    const float max_depth = clamp_float(config.max_depth_m, kMinDepth, kAbsoluteMaxDepth);
    std::vector<float> depth(depth_m.size());
    for (size_t index = 0; index < depth_m.size(); ++index) {
        const float value = depth_m[index];
        depth[index] = std::isfinite(value) ? clamp_float(value, kMinDepth, max_depth) : max_depth;
    }
    return depth;
}

Result<SplatScene> fallback_scene_from_depth(const std::vector<float>& network_depth,
                                             const Image& image, float focal_px,
                                             const FallbackConfig& config,
                                             float fill_head_motion_m, Progress& progress,
                                             FallbackFillStats* stats) {
    using ResultT = Result<SplatScene>;
    if (stats != nullptr) *stats = FallbackFillStats{};
    const int width = image.width;
    const int height = image.height;
    if (width <= 0 || height <= 0 ||
        network_depth.size() != static_cast<size_t>(width) * height) {
        return ResultT::failure("fallback depth map does not match the photo");
    }
    if (!(focal_px > 0.0f)) return ResultT::failure("fallback needs a positive focal length");
    // Snap the network's silhouette ramps first: everything below (layers,
    // edge classes, fill) then sees a step between two surfaces.
    const float upsample = static_cast<float>(std::min(width, height)) /
                           static_cast<float>(std::max(config.input_size, 1));
    const int ramp_radius =
        std::max(2, static_cast<int>(std::ceil(kDepthRampNetworkPx * upsample)));
    const std::vector<float> depth = snap_depth_ramps(network_depth, image, focal_px, ramp_radius);
    const float fx = focal_px;
    const float fy = focal_px;
    const float cx = 0.5f * static_cast<float>(width);
    const float cy = 0.5f * static_cast<float>(height);
    const std::array<float, 256>& srgb_linear = srgb8_to_linear_table();

    // Reserve room for the disocclusion band by shrinking the surface grid a
    // little; the spec budget is "about 0.5 million" per scene, and the band is
    // a small multiple of the silhouette length.
    const size_t target = std::max<size_t>(config.target_splats, 1);
    const size_t surface_target = static_cast<size_t>(static_cast<double>(target) * 0.95);
    int columns = 0;
    int rows = 0;
    fallback_grid(width, height, surface_target, &columns, &rows);
    const double cell_x_px = static_cast<double>(width) / std::max(columns, 1);
    const double cell_y_px = static_cast<double>(height) / std::max(rows, 1);
    const float cell_x_m_factor = static_cast<float>(cell_x_px) / fx;
    const float cell_y_m_factor = static_cast<float>(cell_y_px) / fy;
    const float drop = std::max(config.drop_relative_step, 1e-3f);
    const float mean_cell_px =
        0.5f * static_cast<float>(cell_x_px + cell_y_px);

    // Full-resolution near/far classification. Every pixel that touches a depth
    // edge is labelled by which side of it is nearer, then the near-side pixels
    // are grown into the surface they belong to. Fill reads only the far side.
    std::vector<uint8_t> near_edge;
    std::vector<uint8_t> far_edge;
    classify_edge_pixels(depth, width, height, kFillTriggerStep, &near_edge, &far_edge);
    std::vector<uint8_t> near_region;
    grow_near_regions(depth, width, height, near_edge, far_edge, &near_region);
    std::vector<uint8_t> thin;
    mark_thin_regions(near_region, width, height, mean_cell_px, &thin);

    CellBuilder builder(depth, image, srgb_linear, thin, width, height, cell_x_px, cell_y_px,
                        drop);
    std::vector<CellStat> cells(static_cast<size_t>(columns) * rows);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            builder.compute(column, row, &cells[static_cast<size_t>(row) * columns + column]);
        }
        progress.report(0.8f + 0.05f * static_cast<float>(row + 1) / static_cast<float>(rows),
                        "splat grid");
    }

    // Surface Gaussians: one per full-resolution depth layer found in the
    // cell. A cell that straddles a silhouette emits both sides with their
    // covered-area alpha, so the edge lands within the cell instead of being
    // dropped or faded to a single mid-depth smudge.
    std::vector<Gaussian> surface;
    surface.reserve(static_cast<size_t>(columns) * rows * 5 / 4);
    for (const CellStat& cell : cells) {
        for (int level_index = 0; level_index < cell.level_count; ++level_index) {
            const CellLevel& level = cell.levels[level_index];
            const float fraction = cell.level_count > 1
                                       ? level.weight / std::max(cell.total_weight, 1e-6f)
                                       : cell.coverage;
            const float alpha = config.base_opacity * fraction;
            if (alpha < kMinComponentAlpha) continue;
            const float z = level.z;
            const float cell_x_m = z * cell_x_m_factor;
            const float cell_y_m = z * cell_y_m_factor;
            // A partial layer covers only part of the cell, so its footprint
            // shrinks with its linear extent: a thin foreground sliver stays
            // thin instead of being smeared into a cell-sized blob, while the
            // half-and-half layer of a silhouette still overlaps its
            // neighbours (plus the next cell) to ~99 % coverage.
            const float extent = std::sqrt(std::max(fraction, 0.0f));
            const float sigma_x =
                std::max(kFallbackFootprintOverlap * extent * cell_x_m, 1e-4f);
            const float sigma_y =
                std::max(kFallbackFootprintOverlap * extent * cell_y_m, 1e-4f);
            const float sigma_z = std::max(0.25f * std::min(cell_x_m, cell_y_m), 1e-4f);
            append_gaussian(&surface, (static_cast<float>(level.px) - cx) * z / fx,
                            (static_cast<float>(level.py) - cy) * z / fy, z, sigma_x, sigma_y,
                            sigma_z, level.r, level.g, level.b, alpha);
        }
    }

    // Disocclusion fill. Every near-side pixel within reach of a far-side edge
    // pixel gets that pixel's depth and colour (iterative dilation of the far
    // layer under the near mask). Fill samples are one per 2x2 cells inside the
    // revealed strip, at the far depth pushed behind by a small margin, so they
    // are always hidden behind the near surface at the capture pose.
    FillField field;
    if (fill_head_motion_m > 0.0f) {
        build_fill_field(depth, width, height, near_region, far_edge, &field);
    }
    struct FillSample {
        float px = 0.0f;
        float py = 0.0f;
        float z = 0.0f;
        int source = -1;
    };
    const bool fill_enabled = fill_head_motion_m > 0.0f;
    // One fill sample per block of kFillBlockCells x kFillBlockCells grid cells.
    auto evaluate_fill = [&](int block_column, int block_row, float band_scale,
                             FillSample* out) {
        if (!fill_enabled) return false;
        const double left = block_column * kFillBlockCells * cell_x_px;
        const double top = block_row * kFillBlockCells * cell_y_px;
        const double right =
            std::min(columns, (block_column + 1) * kFillBlockCells) * cell_x_px;
        const double bottom = std::min(rows, (block_row + 1) * kFillBlockCells) * cell_y_px;
        const int x0 = std::min(std::max(static_cast<int>(std::floor(left)), 0), width - 1);
        const int x1 = std::min(std::max(static_cast<int>(std::ceil(right)), x0 + 1), width);
        const int y0 = std::min(std::max(static_cast<int>(std::floor(top)), 0), height - 1);
        const int y1 = std::min(std::max(static_cast<int>(std::ceil(bottom)), y0 + 1), height);
        // A block only partly covered by the near side still needs its fill:
        // that partial block is the first strip the moving foreground uncovers.
        // Place the sample at the covered-area centroid of its valid fill
        // pixels, like a surface splat, and take the colour/depth from the pixel
        // nearest the far edge.
        double weight_sum = 0.0;
        double px_sum = 0.0;
        double py_sum = 0.0;
        int best_distance = std::numeric_limits<int>::max();
        int best_index = -1;
        for (int py = y0; py < y1; ++py) {
            const double covered_top = std::max(static_cast<double>(py), top);
            const double covered_bottom = std::min(py + 1.0, bottom);
            const double weight_y = covered_bottom - covered_top;
            if (weight_y <= 0.0) continue;
            const int row = std::min(rows - 1, static_cast<int>((py + 0.5) / cell_y_px));
            for (int px = x0; px < x1; ++px) {
                const double covered_left = std::max(static_cast<double>(px), left);
                const double covered_right = std::min(px + 1.0, right);
                const double weight_x = covered_right - covered_left;
                if (weight_x <= 0.0) continue;
                const int column =
                    std::min(columns - 1, static_cast<int>((px + 0.5) / cell_x_px));
                const CellStat& cell = cells[static_cast<size_t>(row) * columns + column];
                if (cell.level_count == 0 || cell.thin_surface) continue;
                const size_t index = static_cast<size_t>(py) * width + px;
                if (!near_region[index] || thin[index]) continue;
                if (field.distance[index] <= 0) continue;
                const int source = field.source[index];
                if (source < 0) continue;
                const float z_near = depth[index];
                const float z_far = depth[static_cast<size_t>(source)];
                if (!(z_far > z_near)) continue;
                const float band = fallback_disocclusion_band_px(fx, z_near, z_far,
                                                                 fill_head_motion_m);
                if (band <= 0.0f) continue;
                if (static_cast<float>(field.distance[index]) > band * band_scale) continue;
                const double weight = weight_x * weight_y;
                weight_sum += weight;
                px_sum += weight * (px + 0.5);
                py_sum += weight * (py + 0.5);
                if (field.distance[index] < best_distance) {
                    best_distance = field.distance[index];
                    best_index = static_cast<int>(index);
                }
            }
        }
        if (best_index < 0 || weight_sum <= 0.0) return false;
        const int source = field.source[static_cast<size_t>(best_index)];
        const float z_far = depth[static_cast<size_t>(source)];
        out->px = static_cast<float>(px_sum / weight_sum);
        out->py = static_cast<float>(py_sum / weight_sum);
        // Behind the far surface, but never past the scene's depth cap: sky
        // at `max_depth_m` is backed at its own depth.
        out->z = std::min(z_far * (1.0f + kFallbackFillDepthMargin),
                          std::max(z_far, config.max_depth_m));
        out->source = source;
        return true;
    };
    const int block_columns = (columns + kFillBlockCells - 1) / kFillBlockCells;
    const int block_rows = (rows + kFillBlockCells - 1) / kFillBlockCells;
    auto count_fills = [&](float band_scale) {
        size_t total = 0;
        FillSample sample;
        for (int row = 0; row < block_rows; ++row) {
            for (int column = 0; column < block_columns; ++column) {
                if (evaluate_fill(column, row, band_scale, &sample)) ++total;
            }
        }
        return total;
    };

    const size_t room = target > surface.size() ? target - surface.size() : 0;
    float band_scale = 1.0f;
    if (room == 0) {
        band_scale = 0.0f;
    } else if (count_fills(1.0f) > room) {
        float low = 0.0f;
        float high = 1.0f;
        for (int iteration = 0; iteration < 24; ++iteration) {
            const float middle = 0.5f * (low + high);
            if (count_fills(middle) <= room) {
                low = middle;
            } else {
                high = middle;
            }
        }
        band_scale = low;
    }

    // One network pixel: how far the network's edge and the colour edge
    // disagree, so the fill colour is read beyond it.
    const int colour_margin = std::max(2, static_cast<int>(std::lround(upsample)));
    const std::vector<int> edge_distance =
        fill_enabled ? capped_distance(near_edge, width, height, colour_margin + 1)
                     : std::vector<int>();
    std::vector<Gaussian> fills;
    fills.reserve(std::min(room, count_fills(band_scale)));
    for (int row = 0; row < block_rows && fills.size() < room; ++row) {
        for (int column = 0; column < block_columns && fills.size() < room; ++column) {
            FillSample sample;
            if (!evaluate_fill(column, row, band_scale, &sample)) continue;
            const int sx = sample.source % width;
            const int sy = sample.source / width;
            // Fill colour is the far side's colour a little away from the edge:
            // the mean over the patch around the source of the pixels on the
            // source's surface (same depth layer, so never the nearer object)
            // more than `colour_margin` px from any near-side edge pixel. The
            // edge's own pixels are where the network and the colour edge
            // disagree by a pixel or two (anti-aliased rims, a highlight put on
            // the wrong side); copied across the gap they become coloured teeth
            // and bands. A far side thinner than the margin falls back to the
            // far-edge pixels of the 3x3 patch, as before.
            const float source_z = depth[static_cast<size_t>(sample.source)];
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            int samples = 0;
            for (int pass = 0; pass < 2 && samples == 0; ++pass) {
                const int reach = pass == 0 ? 2 * colour_margin : 1;
                for (int dy = -reach; dy <= reach; ++dy) {
                    for (int dx = -reach; dx <= reach; ++dx) {
                        const int nx = sx + dx;
                        const int ny = sy + dy;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                        const size_t neighbour = static_cast<size_t>(ny) * width + nx;
                        const bool far_colour =
                            pass == 0 ? edge_distance[neighbour] > colour_margin &&
                                            relative_depth_step(depth[neighbour], source_z) <=
                                                kFillTriggerStep
                                      : far_edge[neighbour] != 0;
                        if (!far_colour) continue;
                        const uint8_t* pixel = image.pixel(nx, ny);
                        r += srgb_linear[pixel[0]];
                        g += srgb_linear[pixel[1]];
                        b += srgb_linear[pixel[2]];
                        ++samples;
                    }
                }
            }
            const float inverse = 1.0f / static_cast<float>(std::max(samples, 1));
            if (stats != nullptr) {
                ++stats->fill_samples;
                // Ghost check against the edge the sample was built from: the
                // mean colour of the near object and of the far layer in the
                // same patch around the source pixel, rim pixels included. It
                // does not depend on how the fill colour was chosen, so a fill
                // that took the near object's colour counts here.
                float side_sum[2][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
                int side_count[2] = {0, 0};
                const int reach = 2 * colour_margin;
                for (int dy = -reach; dy <= reach; ++dy) {
                    for (int dx = -reach; dx <= reach; ++dx) {
                        const int nx = sx + dx;
                        const int ny = sy + dy;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                        const float nz = depth[static_cast<size_t>(ny) * width + nx];
                        const bool far_layer =
                            relative_depth_step(nz, source_z) <= kFillTriggerStep;
                        if (!far_layer && nz > source_z) continue;  // a farther surface
                        const int side = far_layer ? 1 : 0;
                        const uint8_t* pixel = image.pixel(nx, ny);
                        for (int channel = 0; channel < 3; ++channel) {
                            side_sum[side][channel] += srgb_linear[pixel[channel]];
                        }
                        ++side_count[side];
                    }
                }
                if (side_count[0] > 0 && side_count[1] > 0) {
                    const float fill[3] = {r * inverse, g * inverse, b * inverse};
                    float distance[2] = {0.0f, 0.0f};
                    float contrast = 0.0f;
                    for (int channel = 0; channel < 3; ++channel) {
                        const float near_mean = side_sum[0][channel] / side_count[0];
                        const float far_mean = side_sum[1][channel] / side_count[1];
                        distance[0] += (fill[channel] - near_mean) * (fill[channel] - near_mean);
                        distance[1] += (fill[channel] - far_mean) * (fill[channel] - far_mean);
                        contrast += (near_mean - far_mean) * (near_mean - far_mean);
                    }
                    // Where both sides look alike (a fold, a dark coat on a dark
                    // shadow) "closer to the near side" is noise, not a leak.
                    if (contrast > kGhostMinContrast && distance[0] < distance[1]) {
                        ++stats->ghost_samples;
                    }
                }
            }
            const float z = sample.z;
            const float cell_x_m = z * cell_x_m_factor;
            const float cell_y_m = z * cell_y_m_factor;
            const float sigma_x =
                std::max(kFillFootprintCells * kFillBlockCells * cell_x_m, 1e-4f);
            const float sigma_y =
                std::max(kFillFootprintCells * kFillBlockCells * cell_y_m, 1e-4f);
            const float sigma_z = std::max(0.25f * std::min(cell_x_m, cell_y_m), 1e-4f);
            append_gaussian(&fills, (sample.px - cx) * z / fx, (sample.py - cy) * z / fy, z,
                            sigma_x, sigma_y, sigma_z, r * inverse, g * inverse, b * inverse,
                            config.base_opacity);
        }
    }

    // Keep at most `target` Gaussians in total.
    if (surface.size() > target) surface.resize(target);

    SplatScene scene;
    scene.resize(surface.size() + fills.size());
    size_t index = 0;
    auto copy = [&](const Gaussian& splat) {
        std::memcpy(&scene.position[index * 3], splat.position, sizeof(float) * 3);
        std::memcpy(&scene.log_scale[index * 3], splat.log_scale, sizeof(float) * 3);
        scene.rotation[index * 4 + 0] = 1.0f;  // w, x, y, z: identity
        scene.rotation[index * 4 + 1] = 0.0f;
        scene.rotation[index * 4 + 2] = 0.0f;
        scene.rotation[index * 4 + 3] = 0.0f;
        std::memcpy(&scene.sh_dc[index * 3], splat.sh_dc, sizeof(float) * 3);
        scene.opacity_logit[index] = splat.opacity_logit;
        ++index;
    };
    for (const Gaussian& splat : surface) copy(splat);
    for (const Gaussian& splat : fills) copy(splat);
    scene.resize(index);
    return ResultT::success(std::move(scene));
}

FallbackReconstructor::FallbackReconstructor(FallbackConfig config) : config_(config) {}

FallbackReconstructor::~FallbackReconstructor() { unload(); }

BackendInfo FallbackReconstructor::info() const {
    BackendInfo backend;
    backend.name = "depth-anything-v2-small";
    backend.license = "Apache-2.0";  // model weights and this backend's pipeline
    // 24.8 M parameters: ~50 MB fp16 weights plus activations for a 924x518
    // input; the spec budget is 1 GiB including the scene.
    backend.vram_bytes = 512ull << 20;
    return backend;
}

Status FallbackReconstructor::load(const Path& gguf) {
    unload();
    splat::Result<std::unique_ptr<DepthAnythingV2>> model =
        DepthAnythingV2::create(gguf, config_.prefer_vulkan, config_.nodes_per_submit);
    if (!model.ok()) return Status::error(model.message());
    model_ = model.take();
    return Status::success();
}

void FallbackReconstructor::unload() { model_.reset(); }

Result<SplatScene> FallbackReconstructor::run(const ReconstructionInput& input, Progress& progress) {
    using ResultT = Result<SplatScene>;
    if (model_ == nullptr) return ResultT::failure("fallback backend is not loaded");
    const Image& image = input.rgb;
    if (image.empty()) return ResultT::failure("fallback backend got an empty image");
    const int width = image.width;
    const int height = image.height;

    const auto started = std::chrono::steady_clock::now();
    progress.report(0.02f, "preprocess");
    splat::Result<std::vector<float>> chw = da2_preprocess(image.rgb.data(), width, height,
                                                          &last_disparity_width_,
                                                          &last_disparity_height_,
                                                          config_.input_size);
    if (!chw.ok()) return ResultT::failure(chw.message());

    progress.report(0.08f, "depth");
    Progress depth_progress([&progress](float fraction, const std::string& stage) {
        progress.report(0.08f + 0.7f * fraction, stage.empty() ? "depth" : stage);
    });
    splat::Result<DepthAnythingResult> depth =
        model_->run(chw.value().data(), last_disparity_width_, last_disparity_height_, {},
                    &depth_progress);
    if (!depth.ok()) return ResultT::failure(depth.message());

    // Back to the photo's resolution, exactly like DepthAnythingV2.infer_image.
    progress.report(0.8f, "splat grid");
    last_disparity_ = resize_bilinear_align_corners(depth.value().depth, depth.value().width,
                                                    depth.value().height, width, height);

    // Metric mapping: median relative inverse depth becomes the assumed scene
    // depth, so a slider that scales `assumed_scene_depth_m` scales the scene.
    // Full resolution: the discontinuity mask is sampled here, not on the
    // emission grid, so a silhouette keeps its sub-cell position.
    const std::vector<float> depth_px =
        fallback_metric_depth_from_disparity(last_disparity_, config_);

    // Intrinsics: EXIF focal (the C2 input) or SHARP's 30 mm default.
    float focal = input.focal_px.value_or(0.0f);
    if (focal <= 0.0f) {
        const splat::Result<float> fallback = focal_px_from_exif(ExifFocal{}, width, height);
        if (!fallback.ok()) return ResultT::failure(fallback.message());
        focal = fallback.value();
    }
    Result<SplatScene> scene =
        fallback_scene_from_depth(depth_px, image, focal, config_,
                                  kFallbackDisocclusionHeadMotionM, progress,
                                  &last_fill_stats_);
    last_run_seconds_ =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    progress.report(1.0f, "done");
    return scene;
}

}  // namespace splat
