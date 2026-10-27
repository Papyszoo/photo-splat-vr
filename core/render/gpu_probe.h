#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace splat {

// Runs the fill.compute shader (core/render/shaders/fill.comp) over `count`
// uints on the first Vulkan device with a compute queue. Each element becomes
// i * 3 + 7. Returns false and fills `error` on any Vulkan or IO failure.
bool run_fill_probe(const std::string& spirv_path, uint32_t count,
                    std::vector<uint32_t>& out, std::string& error);

}  // namespace splat
