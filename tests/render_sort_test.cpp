#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "render/radix_sort.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

namespace {

struct SortResult {
    std::vector<uint32_t> keys;
    std::vector<uint32_t> values;
};

// Runs one indirect key-value sort on the GPU. Returns false and fills error
// on any Vulkan failure.
bool gpu_sort(uint32_t count, const std::vector<uint32_t>& keys, SortResult& out,
              std::string& error) {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        error = context.message();
        return false;
    }
    splat::VulkanContext& device = *context.value();

    // VulkanBuffer has no destructor, so every buffer is released by hand at
    // the end of the call (and on the early returns below).
    splat::VulkanBuffer keys_buffer;
    splat::VulkanBuffer values_buffer;
    splat::VulkanBuffer count_buffer;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    auto release = [&]() {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device.device(), fence, nullptr);
        if (command_buffer != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device.device(), device.command_pool(), 1, &command_buffer);
        }
        splat::destroy_vulkan_buffer(device, count_buffer);
        splat::destroy_vulkan_buffer(device, values_buffer);
        splat::destroy_vulkan_buffer(device, keys_buffer);
    };
    auto fail = [&](const std::string& message) {
        error = message;
        release();
        return false;
    };

    std::vector<uint32_t> values(count);
    std::iota(values.begin(), values.end(), 0u);

    auto created_keys = splat::create_vulkan_buffer(
        device, static_cast<VkDeviceSize>(count) * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, keys.data());
    if (!created_keys.ok()) return fail(created_keys.message());
    keys_buffer = created_keys.take();
    auto created_values = splat::create_vulkan_buffer(
        device, static_cast<VkDeviceSize>(count) * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, values.data());
    if (!created_values.ok()) return fail(created_values.message());
    values_buffer = created_values.take();
    auto created_count = splat::create_vulkan_buffer(
        device, sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &count);
    if (!created_count.ok()) return fail(created_count.message());
    count_buffer = created_count.take();

    auto sorter = splat::RadixSort::create(device, count);
    if (!sorter.ok()) return fail(sorter.message());

    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = device.command_pool();
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkResult result = vkAllocateCommandBuffers(device.device(), &allocate, &command_buffer);
    if (result != VK_SUCCESS) return fail("vkAllocateCommandBuffers failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin);

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask =
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);

    sorter.value()->record(command_buffer, keys_buffer.buffer, values_buffer.buffer,
                           count_buffer.buffer);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
    vkEndCommandBuffer(command_buffer);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(device.device(), &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) return fail("vkCreateFence failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    result = vkQueueSubmit(device.queue(), 1, &submit, fence);
    if (result != VK_SUCCESS) return fail("vkQueueSubmit failed");
    result = vkWaitForFences(device.device(), 1, &fence, VK_TRUE, 60ull * 1000 * 1000 * 1000);
    if (result != VK_SUCCESS) return fail("vkWaitForFences failed");

    out.keys.resize(count);
    out.values.resize(count);
    std::memcpy(out.keys.data(), keys_buffer.mapped, count * sizeof(uint32_t));
    std::memcpy(out.values.data(), values_buffer.mapped, count * sizeof(uint32_t));
    release();
    return true;
}

void check_sort(uint32_t count) {
    std::mt19937 rng(1234 + count);
    std::vector<uint32_t> keys(count);
    for (uint32_t i = 0; i < count; ++i) {
        // A small range forces many ties; every 8th key is full range.
        keys[i] = (i % 8 == 0) ? rng() : (rng() % 1024);
    }

    std::vector<uint32_t> expected = keys;
    std::stable_sort(expected.begin(), expected.end());

    SortResult result;
    std::string error;
    REQUIRE_MESSAGE(gpu_sort(count, keys, result, error), error);
    REQUIRE(result.keys.size() == count);
    REQUIRE(static_cast<uint32_t>(result.values.size()) == count);

    // Sorted key sequence must match std::stable_sort exactly.
    REQUIRE(std::equal(expected.begin(), expected.end(), result.keys.begin()));

    // The sorter is not stable across partitions, but it must be deterministic
    // for identical input: the renderer feeds it a deterministic order so that
    // equal depth keys (and therefore whole frames) repeat exactly.
    {
        SortResult again;
        std::string again_error;
        REQUIRE_MESSAGE(gpu_sort(count, keys, again, again_error), again_error);
        const bool same = again.keys == result.keys && again.values == result.values;
        CHECK_MESSAGE(same, "repeat sort of identical input gives identical output");
    }

    // Ties may be ordered differently from std::stable_sort, but values must be
    // a permutation and each (key, value) pair must survive the sort.
    std::vector<uint32_t> permutation = result.values;
    std::sort(permutation.begin(), permutation.end());
    for (uint32_t i = 0; i < count; ++i) {
        REQUIRE(permutation[i] == i);
        REQUIRE(keys[result.values[i]] == result.keys[i]);
    }
}

}  // namespace

TEST_CASE("GPU radix sort matches std::stable_sort, including ties") {
    // CI runs on lavapipe, whose subgroup size does not meet the renderer's
    // device requirements; skip there instead of failing the suite.
    auto probe = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!probe.ok()) {
        MESSAGE("skipping: " << probe.message());
        return;
    }
    for (uint32_t count : {1u, 1000u, 1200000u}) {
        CAPTURE(count);
        check_sort(count);
    }
}

namespace {

// One splat cloud on a coarse lattice: all of it in front of the camera and
// inside the frame, and no two splats at the same depth, so the sort order is
// decided and the CPU's pre-rotated copy cannot tie with it.
splat::SplatScene lattice_scene() {
    constexpr int kSide = 4;   // 4 x 4 x 3 = 48 splats
    constexpr int kDepth = 3;
    splat::SplatScene scene;
    scene.resize(static_cast<size_t>(kSide * kSide * kDepth));
    const float white_dc = 0.5f / 0.28209479177387814f;
    const float log_sigma = std::log(0.02f);
    size_t index = 0;
    for (int z = 0; z < kDepth; ++z) {
        for (int y = 0; y < kSide; ++y) {
            for (int x = 0; x < kSide; ++x, ++index) {
                // z spans 0.80 .. 1.10 m in 0.15 m steps, x and y +-0.15 m.
                scene.position[index * 3 + 0] = 0.1f * static_cast<float>(x - 1);
                scene.position[index * 3 + 1] = 0.1f * static_cast<float>(y - 1);
                scene.position[index * 3 + 2] = 0.8f + 0.15f * static_cast<float>(z);
                for (int axis = 0; axis < 3; ++axis) {
                    scene.log_scale[index * 3 + axis] = log_sigma;
                }
                scene.rotation[index * 4 + 0] = 1.0f;   // w, x, y, z: identity
                scene.rotation[index * 4 + 1] = 0.0f;
                scene.rotation[index * 4 + 2] = 0.0f;
                scene.rotation[index * 4 + 3] = 0.0f;
                for (int channel = 0; channel < 3; ++channel) {
                    scene.sh_dc[index * 3 + channel] = white_dc;
                }
                scene.opacity_logit[index] = 12.0f;
            }
        }
    }
    return scene;
}

// Renders the scene once (no readback) and returns the frame's draw order: the
// splat indices as the sort left them, which is what a rotation must not
// change. Empty with `error` set on failure.
std::vector<uint32_t> draw_order(const splat::SplatScene& scene,
                                 const splat::RendererOptions& options,
                                 const splat::FrameView& view, splat::VulkanContext& device,
                                 std::string& error) {
    auto renderer = splat::SplatRenderer::create(device, options, scene);
    if (!renderer.ok()) {
        error = renderer.message();
        return {};
    }
    auto image = splat::create_vulkan_color_image(
        device, options.target_format, renderer.value()->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image.ok()) {
        error = image.message();
        return {};
    }
    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = renderer.value()->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const splat::Status status = renderer.value()->render(view, target, false);
    std::vector<uint32_t> order;
    if (!status.ok()) {
        error = status.message();
    } else {
        order = renderer.value()->last_draw_order();
        if (order.empty()) error = "the renderer returned no draw order";
    }
    splat::destroy_vulkan_image(device, image.value());
    return order;
}

}  // namespace

#else

TEST_CASE("GPU radix sort is skipped without Vulkan") { CHECK(true); }

#endif
