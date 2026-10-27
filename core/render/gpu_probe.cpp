#include "render/gpu_probe.h"

#include <vulkan/vulkan.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "render/vulkan_context.h"

namespace splat {
namespace {

constexpr uint32_t kLocalSizeX = 64;

std::string vk_error(const char* what, VkResult result) {
    return std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) +
           ")";
}

bool read_file(const std::string& path, std::vector<char>& out, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open shader: " + path;
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        error = "bad SPIR-V size: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(out.data(), size)) {
        error = "cannot read shader: " + path;
        return false;
    }
    return true;
}

// The handful of handles this probe needs on top of VulkanContext. Everything
// device-side (instance, device, queue, command pool) comes from the context.
struct ProbePipeline {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    void destroy(VkDevice device) {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    }
};

bool create_pipeline(VkDevice device, ProbePipeline& p, const std::vector<char>& spirv,
                     VkBuffer storage, std::string& error) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    VkResult result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &p.set_layout);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateDescriptorSetLayout", result);
        return false;
    }

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &p.set_layout;
    result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &p.pipeline_layout);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreatePipelineLayout", result);
        return false;
    }

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &p.pool);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateDescriptorPool", result);
        return false;
    }

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = p.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &p.set_layout;
    result = vkAllocateDescriptorSets(device, &allocate, &p.set);
    if (result != VK_SUCCESS) {
        error = vk_error("vkAllocateDescriptorSets", result);
        return false;
    }

    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = storage;
    buffer_info.offset = 0;
    buffer_info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = p.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = spirv.size();
    shader_info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    result = vkCreateShaderModule(device, &shader_info, nullptr, &p.shader);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateShaderModule", result);
        return false;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = p.shader;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = p.pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                      &p.pipeline);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateComputePipelines", result);
        return false;
    }
    return true;
}

}  // namespace

bool run_fill_probe(const std::string& spirv_path, uint32_t count,
                    std::vector<uint32_t>& out, std::string& error) {
    if (count == 0) {
        error = "count must be non-zero";
        return false;
    }

    std::vector<char> spirv;
    if (!read_file(spirv_path, spirv, error)) return false;

    auto created = VulkanContext::create(VulkanContextOptions{});
    if (!created.ok()) {
        error = created.message();
        return false;
    }
    VulkanContext& context = *created.value();
    VkDevice device = context.device();

    auto storage = create_vulkan_buffer(context, VkDeviceSize(count) * sizeof(uint32_t),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    ProbePipeline pipeline;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = storage.ok();
    if (!ok) error = storage.message();
    if (ok) ok = create_pipeline(device, pipeline, spirv, storage.value().buffer, error);

    if (ok) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = context.command_pool();
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkResult result = vkAllocateCommandBuffers(device, &allocate, &command_buffer);
        if (result != VK_SUCCESS) {
            ok = false;
            error = vk_error("vkAllocateCommandBuffers", result);
        }
    }
    if (ok) {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        const VkResult result = vkCreateFence(device, &fence_info, nullptr, &fence);
        if (result != VK_SUCCESS) {
            ok = false;
            error = vk_error("vkCreateFence", result);
        }
    }

    if (ok) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult result = vkBeginCommandBuffer(command_buffer, &begin);
        if (result != VK_SUCCESS) {
            ok = false;
            error = vk_error("vkBeginCommandBuffer", result);
        }
        if (ok) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipeline.pipeline_layout, 0, 1, &pipeline.set, 0, nullptr);
            const uint32_t groups = (count + kLocalSizeX - 1) / kLocalSizeX;
            vkCmdDispatch(command_buffer, groups, 1, 1);
            result = vkEndCommandBuffer(command_buffer);
            if (result != VK_SUCCESS) {
                ok = false;
                error = vk_error("vkEndCommandBuffer", result);
            }
        }
        if (ok) {
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffer;
            result = vkQueueSubmit(context.queue(), 1, &submit, fence);
            if (result != VK_SUCCESS) {
                ok = false;
                error = vk_error("vkQueueSubmit", result);
            }
        }
        if (ok) {
            result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
            if (result != VK_SUCCESS) {
                ok = false;
                error = vk_error("vkWaitForFences", result);
            }
        }
        if (ok) {
            out.resize(count);
            std::memcpy(out.data(), storage.value().mapped, out.size() * sizeof(uint32_t));
        }
    }

    if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
    if (command_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, context.command_pool(), 1, &command_buffer);
    }
    pipeline.destroy(device);
    if (storage.ok()) destroy_vulkan_buffer(context, storage.value());
    return ok;
}

}  // namespace splat
