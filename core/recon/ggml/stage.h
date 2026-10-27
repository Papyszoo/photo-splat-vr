#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace splat {

// One tensor read back from a network stage for a parity test. `shape` is the
// ggml ne order, so a (W, H, C, 1) channels-last map has shape {W, H, C, 1} and
// the values are row-major with x fastest — the same linear element order as the
// reference's NCHW .npy dumps (tools/parity/npy.h). Both the DA2 (C4) driver and
// the SHARP (C3) stages report through this type, so a test helper can take
// either backend's dump.
struct StageDump {
    std::string name;
    std::vector<int64_t> shape;
    std::vector<float> values;
};

using StageCallback = std::function<void(const StageDump&)>;

}  // namespace splat
