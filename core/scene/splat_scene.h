#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace splat {

struct Bounds {
    std::array<float, 3> min{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> max{{0.0f, 0.0f, 0.0f}};
    bool valid = false;
};

// In-memory scene, struct-of-arrays in fp32. The attribute conventions are the
// standard 3DGS ones and match the SHARP .ply export:
//   position      xyz metres, OpenCV axes (x right, y down, z forward)
//   log_scale     log(sigma) per axis
//   rotation      quaternion w, x, y, z
//   sh_dc         SH degree-0 RGB of the sRGB colour, i.e. (rgb - 0.5) / 0.2820948
//   opacity_logit logit(alpha)
// 14 values per Gaussian.
struct SplatScene {
    static constexpr uint32_t kValuesPerSplat = 14;
    static constexpr uint32_t kLayout = 1;  // pos3, log_scale3, rot4(wxyz), sh_dc3, opacity1

    std::vector<float> position;      // 3 * count
    std::vector<float> log_scale;     // 3 * count
    std::vector<float> rotation;      // 4 * count
    std::vector<float> sh_dc;         // 3 * count
    std::vector<float> opacity_logit; // 1 * count

    size_t count() const { return opacity_logit.size(); }
    bool empty() const { return opacity_logit.empty(); }
    bool consistent() const {
        return position.size() == count() * 3 && log_scale.size() == count() * 3 &&
               rotation.size() == count() * 4 && sh_dc.size() == count() * 3;
    }

    void clear();
    void resize(size_t count);
    Bounds bounds() const;
};

}  // namespace splat
