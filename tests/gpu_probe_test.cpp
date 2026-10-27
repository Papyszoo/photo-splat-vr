#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "render/gpu_probe.h"
#include "render/vulkan_context.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

TEST_CASE("fill.comp runs through Vulkan and writes the expected values") {
    // CI runs on lavapipe, whose subgroup size does not meet the renderer's
    // device requirements, so there is no device to probe there. Skip in that
    // case; every other probe failure still fails the test.
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    context.value().reset();

    const std::string path = std::string(SPLAT_SHADER_DIR) + "/fill.comp.spv";
    constexpr uint32_t count = 256;

    std::vector<uint32_t> values;
    std::string error;
    const bool ok = splat::run_fill_probe(path, count, values, error);
    REQUIRE_MESSAGE(ok, error);
    REQUIRE(values.size() == count);

    for (uint32_t i = 0; i < count; ++i) {
        REQUIRE_MESSAGE(values[i] == i * 3u + 7u,
                        "value[" << i << "] = " << values[i]);
    }
}
