#include "scene/splat_scene.h"

#include <limits>

namespace splat {

void SplatScene::clear() {
    position.clear();
    log_scale.clear();
    rotation.clear();
    sh_dc.clear();
    opacity_logit.clear();
}

void SplatScene::resize(size_t count) {
    position.resize(count * 3);
    log_scale.resize(count * 3);
    rotation.resize(count * 4);
    sh_dc.resize(count * 3);
    opacity_logit.resize(count);
}

Bounds SplatScene::bounds() const {
    Bounds result;
    const size_t n = count();
    if (n == 0 || position.size() < n * 3) return result;

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float min_z = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    float max_z = std::numeric_limits<float>::lowest();

    for (size_t i = 0; i < n; ++i) {
        const float x = position[i * 3 + 0];
        const float y = position[i * 3 + 1];
        const float z = position[i * 3 + 2];
        if (x < min_x) min_x = x;
        if (y < min_y) min_y = y;
        if (z < min_z) min_z = z;
        if (x > max_x) max_x = x;
        if (y > max_y) max_y = y;
        if (z > max_z) max_z = z;
    }

    result.min = {{min_x, min_y, min_z}};
    result.max = {{max_x, max_y, max_z}};
    result.valid = true;
    return result;
}

}  // namespace splat
