// SPDX-License-Identifier: GPL-3.0-or-later

#include "splat_import/thinning.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace splat {
namespace {

float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

// Volume times coverage: a large, opaque Gaussian is seen far more than a
// small, transparent one. exp() of the sum of log scales is the product of the
// three sigmas without three multiplies per comparison.
float importance(const SplatScene& scene, size_t index) {
    const float log_volume = scene.log_scale[index * 3 + 0] + scene.log_scale[index * 3 + 1] +
                             scene.log_scale[index * 3 + 2];
    return std::exp(log_volume) * sigmoid(scene.opacity_logit[index]);
}

}  // namespace

void thin_scene(SplatScene& scene, uint32_t limit, ImportReport* report) {
    ImportReport local;
    ImportReport* out = report != nullptr ? report : &local;

    const size_t count = scene.count();
    out->source_count = static_cast<uint32_t>(count);
    out->kept_count = out->source_count;
    out->thinned = false;

    if (limit == 0 || count <= limit) return;

    std::vector<float> scores(count);
    for (size_t i = 0; i < count; ++i) scores[i] = importance(scene, i);

    std::vector<uint32_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0u);
    // Descending importance; equal scores keep the lower index first so the cut
    // is reproducible across runs and platforms. A non-finite score (a corrupt
    // source range) is treated as the least important: comparing NaNs makes the
    // "not equal then not greater" pair non-transitive, which is undefined
    // behaviour for nth_element and can keep an arbitrary set.
    const auto rank = [](float score) {
        return std::isfinite(score) ? score : -std::numeric_limits<float>::infinity();
    };
    std::nth_element(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(limit),
                     indices.end(), [&scores, &rank](uint32_t a, uint32_t b) {
                         const float left = rank(scores[a]);
                         const float right = rank(scores[b]);
                         if (left != right) return left > right;
                         return a < b;
                     });
    indices.resize(limit);
    // Output in the source's own order: an exporter's depth sort is worth
    // keeping, and a stable order makes the written .psplat reproducible.
    std::sort(indices.begin(), indices.end());

    SplatScene kept;
    kept.resize(indices.size());
    for (size_t out_index = 0; out_index < indices.size(); ++out_index) {
        const size_t from = indices[out_index];
        for (size_t axis = 0; axis < 3; ++axis) {
            kept.position[out_index * 3 + axis] = scene.position[from * 3 + axis];
            kept.log_scale[out_index * 3 + axis] = scene.log_scale[from * 3 + axis];
            kept.sh_dc[out_index * 3 + axis] = scene.sh_dc[from * 3 + axis];
        }
        for (size_t component = 0; component < 4; ++component) {
            kept.rotation[out_index * 4 + component] = scene.rotation[from * 4 + component];
        }
        kept.opacity_logit[out_index] = scene.opacity_logit[from];
    }
    scene = std::move(kept);

    out->kept_count = limit;
    out->thinned = true;
    out->note = "thinned " + count_label(count) + " to " + count_label(limit) + " splats";
}

}  // namespace splat
