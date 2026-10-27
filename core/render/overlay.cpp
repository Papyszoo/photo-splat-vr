#include "render/overlay.h"

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "render/splat_renderer.h"

namespace splat {
namespace {

// Push constants of overlay.vert: the camera block the splat pass already
// computed for the eye, not a second projection.
struct OverlayPush {
    float view[16];
    float intrinsics[4];  // fx, fy, cx, cy
    float screen[4];      // width, height, unused, unused
    float eye[4];         // eye_offset metres, cx_shift pixels
    float encode[4];      // x = 1 when the UNORM target needs sRGB encoding
};
static_assert(sizeof(OverlayPush) == 128, "overlay push constant layout");

struct OverlayVertex {
    float position[3];
    float uv[2];
    float tint[4];
};

std::string vk_error(const char* what, VkResult result) {
    return std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) +
           ")";
}

bool is_srgb_format(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
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

Result<VkShaderModule> load_shader_module(VulkanContext& context, const std::string& path) {
    std::vector<char> spirv;
    std::string error;
    if (!read_file(path, spirv, error)) return Result<VkShaderModule>::failure(error);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size();
    info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(context.device(), &info, nullptr, &module);
    if (result != VK_SUCCESS) {
        return Result<VkShaderModule>::failure(vk_error("vkCreateShaderModule", result));
    }
    return Result<VkShaderModule>::success(module);
}

void image_barrier(VkCommandBuffer command_buffer, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access, uint32_t base_mip = 0, uint32_t mip_count = 1) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = base_mip;
    barrier.subresourceRange.levelCount = mip_count;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

// Fills levels 1 .. mip_levels-1 by a linear blit of the level above. Level 0
// is already in TRANSFER_DST (the upload barrier) and receives the staging
// copy; this barrier moves it to TRANSFER_SRC, then each blit's destination is
// moved to TRANSFER_SRC before the next level reads it. The caller finishes
// with one barrier for the whole chain to SHADER_READ_ONLY.
void generate_mips(VkCommandBuffer command_buffer, VkImage image, uint32_t width,
                   uint32_t height, uint32_t mip_levels) {
    if (mip_levels <= 1) return;
    image_barrier(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
    for (uint32_t level = 1; level < mip_levels; ++level) {
        const VkOffset3D src_extent{static_cast<int32_t>(std::max(1u, width >> (level - 1))),
                                    static_cast<int32_t>(std::max(1u, height >> (level - 1))), 1};
        const VkOffset3D dst_extent{static_cast<int32_t>(std::max(1u, width >> level)),
                                    static_cast<int32_t>(std::max(1u, height >> level)), 1};
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = src_extent;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = dst_extent;
        vkCmdBlitImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        image_barrier(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, level, 1);
    }
}

}  // namespace

OverlayRenderer::~OverlayRenderer() { destroy(); }

void OverlayRenderer::destroy() {
    if (context_ == nullptr) return;
    VkDevice device = context_->device();
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (Texture& texture : textures_) destroy_vulkan_image(*context_, texture.image);
        textures_.clear();
        destroy_vulkan_buffer(*context_, vertices_);
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
        if (rgba_pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, rgba_pipeline_, nullptr);
        }
        if (pipeline_alt_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_alt_, nullptr);
        if (rgba_pipeline_alt_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, rgba_pipeline_alt_, nullptr);
        }
        if (fragment_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, fragment_module_, nullptr);
        }
        if (rgba_fragment_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, rgba_fragment_module_, nullptr);
        }
        if (vertex_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, vertex_module_, nullptr);
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        }
        if (set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
        }
        if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device, sampler_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    rgba_pipeline_ = VK_NULL_HANDLE;
    pipeline_alt_ = VK_NULL_HANDLE;
    rgba_pipeline_alt_ = VK_NULL_HANDLE;
    fragment_module_ = VK_NULL_HANDLE;
    rgba_fragment_module_ = VK_NULL_HANDLE;
    vertex_module_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    context_ = nullptr;
}

Result<std::unique_ptr<OverlayRenderer>> OverlayRenderer::create(VulkanContext& context,
                                                                 const std::string& shader_dir,
                                                                 VkFormat target_format) {
    std::unique_ptr<OverlayRenderer> overlay(new OverlayRenderer());
    overlay->context_ = &context;
    overlay->target_format_ = target_format;

    auto fail = [](const std::string& message) {
        return Result<std::unique_ptr<OverlayRenderer>>::failure(message);
    };

    const std::string dir = shader_dir;
    auto vertex_module = load_shader_module(context, dir + "/overlay.vert.spv");
    if (!vertex_module.ok()) return fail(vertex_module.message());
    overlay->vertex_module_ = vertex_module.take();
    auto fragment_module = load_shader_module(context, dir + "/overlay.frag.spv");
    if (!fragment_module.ok()) return fail(fragment_module.message());
    overlay->fragment_module_ = fragment_module.take();
    auto rgba_fragment_module = load_shader_module(context, dir + "/overlay_rgba.frag.spv");
    if (!rgba_fragment_module.ok()) return fail(rgba_fragment_module.message());
    overlay->rgba_fragment_module_ = rgba_fragment_module.take();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorCount = 1;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    VkResult result = vkCreateDescriptorSetLayout(context.device(), &set_layout, nullptr,
                                                  &overlay->set_layout_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateDescriptorSetLayout", result));

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(OverlayPush);
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &overlay->set_layout_;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(context.device(), &pipeline_layout, nullptr,
                                    &overlay->pipeline_layout_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreatePipelineLayout", result));

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 64;
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 64;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(context.device(), &pool, nullptr, &overlay->descriptor_pool_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateDescriptorPool", result));

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    // VR9: the panel and the label are minified in the headset (their textures
    // are 1.3-2x the projected pixels), so sample the generated mip chain
    // trilinearly instead of level 0 (the flicker of headset test 6).
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (context.sampler_anisotropy()) {
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy = std::min(8.0f, context.max_sampler_anisotropy());
    }
    result = vkCreateSampler(context.device(), &sampler_info, nullptr, &overlay->sampler_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateSampler", result));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = overlay->vertex_module_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = overlay->fragment_module_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = sizeof(OverlayVertex);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(OverlayVertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(OverlayVertex, uv);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[2].offset = offsetof(OverlayVertex, tint);
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Straight alpha: (tint.rgb, tint.a * mask), over the frame.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    // No depth test against the splats: the label stays readable even where a
    // splat was nearer.
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.stencilTestEnable = VK_FALSE;
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &target_format;

    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    graphics.pNext = &rendering;
    graphics.stageCount = 2;
    graphics.pStages = stages;
    graphics.pVertexInputState = &vertex_input;
    graphics.pInputAssemblyState = &input_assembly;
    graphics.pViewportState = &viewport_state;
    graphics.pRasterizationState = &raster;
    graphics.pMultisampleState = &multisample;
    graphics.pColorBlendState = &color_blend;
    graphics.pDepthStencilState = &depth;
    graphics.pDynamicState = &dynamic;
    graphics.layout = overlay->pipeline_layout_;
    result = vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1, &graphics, nullptr,
                                       &overlay->pipeline_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateGraphicsPipelines (overlay)", result));
    // The panel pipeline differs only in the fragment stage (the texel's own
    // colour instead of mask * tint); vertex, blend and depth state are the
    // same, so both kinds of quad stay one draw path.
    stages[1].module = overlay->rgba_fragment_module_;
    result = vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1, &graphics, nullptr,
                                       &overlay->rgba_pipeline_);
    if (result != VK_SUCCESS) {
        return fail(vk_error("vkCreateGraphicsPipelines (overlay rgba)", result));
    }
    // The other blend-space attachment format (sRGB <-> UNORM). Same shaders,
    // same state; only the pipeline's colour attachment format changes, so the
    // caller can pass either target image.
    overlay->alt_format_ = blend_alt_format(target_format);
    if (overlay->alt_format_ != target_format) {
        rendering.pColorAttachmentFormats = &overlay->alt_format_;
        stages[1].module = overlay->fragment_module_;
        result = vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1, &graphics,
                                           nullptr, &overlay->pipeline_alt_);
        if (result != VK_SUCCESS) {
            return fail(vk_error("vkCreateGraphicsPipelines (overlay alt)", result));
        }
        stages[1].module = overlay->rgba_fragment_module_;
        result = vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1, &graphics,
                                           nullptr, &overlay->rgba_pipeline_alt_);
        if (result != VK_SUCCESS) {
            return fail(vk_error("vkCreateGraphicsPipelines (overlay rgba alt)", result));
        }
    }
    return Result<std::unique_ptr<OverlayRenderer>>::success(std::move(overlay));
}

Status OverlayRenderer::upload_texture(Texture& texture, VkFormat format, uint32_t width,
                                       uint32_t height, const uint8_t* pixels) {
    if (width == 0 || height == 0 || pixels == nullptr) {
        return Status::error("overlay texture has no pixels");
    }
    const bool same_size = texture.image.image != VK_NULL_HANDLE &&
                           texture.image.extent.width == width &&
                           texture.image.extent.height == height &&
                           texture.image.format == format;
    VkImageLayout old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!same_size) {
        if (texture.image.image != VK_NULL_HANDLE) destroy_vulkan_image(*context_, texture.image);
        const uint32_t mips = mip_level_count(width, height);
        auto image = create_vulkan_color_image(
            *context_, format, VkExtent2D{width, height},
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, mips);
        if (!image.ok()) return Status::error(image.message());
        texture.image = image.take();
        texture.format = format;
        old_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkDescriptorImageInfo info{};
        info.sampler = sampler_;
        info.imageView = texture.image.view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = texture.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
    }
    const uint32_t mip_levels = texture.image.mip_levels;

    // One staging copy: 1 byte per pixel for an R8 mask, 4 for an RGBA panel.
    const VkDeviceSize bytes_per_pixel = format == VK_FORMAT_R8G8B8A8_SRGB ? 4u : 1u;
    const VkDeviceSize bytes = bytes_per_pixel * width * height;
    auto staging = create_vulkan_buffer(*context_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels);
    if (!staging.ok()) return Status::error(staging.message());

    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = context_->command_pool();
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result =
        vkAllocateCommandBuffers(context_->device(), &allocate, &command_buffer);
    if (result != VK_SUCCESS) {
        destroy_vulkan_buffer(*context_, staging.value());
        return Status::error(vk_error("vkAllocateCommandBuffers (overlay)", result));
    }
    // A fence for this upload alone, the pattern gpu_probe.cpp and
    // SplatRenderer::render use. vkQueueWaitIdle() waited for every earlier
    // submission in the queue too, and this runs from the frame loop
    // (UiHost::frame -> upload_canvas whenever the panel content or the Sense
    // cursor changed): with the headset's frame already queued, every panel
    // update stalled the whole queue, not just the mip blits.
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(context_->device(), &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) {
        vkFreeCommandBuffers(context_->device(), context_->command_pool(), 1, &command_buffer);
        VulkanBuffer buffer = staging.take();
        destroy_vulkan_buffer(*context_, buffer);
        return Status::error(vk_error("vkCreateFence (overlay)", result));
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &begin);
    if (result == VK_SUCCESS) {
        // Every level goes to TRANSFER_DST: level 0 takes the staging copy,
        // the blits then fill 1 .. mip_levels-1.
        image_barrier(command_buffer, texture.image.image, old_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE
                                                              : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                          ? VK_ACCESS_2_NONE
                          : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0,
                      mip_levels);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(command_buffer, staging.value().buffer, texture.image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // The mip chain, then the sampler's layout for every level.
        if (mip_levels > 1) {
            generate_mips(command_buffer, texture.image.image, width, height, mip_levels);
        } else {
            image_barrier(command_buffer, texture.image.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
        }
        image_barrier(command_buffer, texture.image.image,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, mip_levels);
        result = vkEndCommandBuffer(command_buffer);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        result = vkQueueSubmit(context_->queue(), 1, &submit, fence);
        // Only this upload: the staging buffer and the command buffer are freed
        // right after, and no other submission has to finish first.
        if (result == VK_SUCCESS) {
            result = vkWaitForFences(context_->device(), 1, &fence, VK_TRUE, UINT64_MAX);
        }
    }
    vkDestroyFence(context_->device(), fence, nullptr);
    vkFreeCommandBuffers(context_->device(), context_->command_pool(), 1, &command_buffer);
    VulkanBuffer buffer = staging.take();
    destroy_vulkan_buffer(*context_, buffer);
    if (result != VK_SUCCESS) return Status::error(vk_error("overlay texture upload", result));
    return Status::success();
}

Result<uint32_t> OverlayRenderer::allocate_texture(VkFormat format, uint32_t width,
                                                   uint32_t height, const uint8_t* pixels) {
    if (context_ == nullptr) {
        return Result<uint32_t>::failure("the overlay renderer has no Vulkan context");
    }
    if (textures_.size() >= 64) {
        return Result<uint32_t>::failure("the overlay texture limit (64) is reached");
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    Texture texture;
    const VkResult result =
        vkAllocateDescriptorSets(context_->device(), &allocate, &texture.set);
    if (result != VK_SUCCESS) {
        return Result<uint32_t>::failure(vk_error("vkAllocateDescriptorSets (overlay)", result));
    }
    const Status uploaded = upload_texture(texture, format, width, height, pixels);
    if (!uploaded.ok()) {
        if (texture.image.image != VK_NULL_HANDLE) destroy_vulkan_image(*context_, texture.image);
        vkFreeDescriptorSets(context_->device(), descriptor_pool_, 1, &texture.set);
        return Result<uint32_t>::failure(uploaded.message());
    }
    textures_.push_back(texture);
    return Result<uint32_t>::success(static_cast<uint32_t>(textures_.size() - 1));
}

Result<uint32_t> OverlayRenderer::add_texture(uint32_t width, uint32_t height,
                                              const uint8_t* pixels) {
    return allocate_texture(VK_FORMAT_R8_UNORM, width, height, pixels);
}

Result<uint32_t> OverlayRenderer::add_rgba_texture(uint32_t width, uint32_t height,
                                                   const uint8_t* rgba_pixels) {
    return allocate_texture(VK_FORMAT_R8G8B8A8_SRGB, width, height, rgba_pixels);
}

Status OverlayRenderer::update_texture(uint32_t index, uint32_t width, uint32_t height,
                                       const uint8_t* pixels) {
    if (context_ == nullptr) return Status::error("the overlay renderer has no Vulkan context");
    if (index >= textures_.size()) return Status::error("overlay texture index is out of range");
    return upload_texture(textures_[index], VK_FORMAT_R8_UNORM, width, height, pixels);
}

Status OverlayRenderer::update_rgba_texture(uint32_t index, uint32_t width, uint32_t height,
                                            const uint8_t* rgba_pixels) {
    if (context_ == nullptr) return Status::error("the overlay renderer has no Vulkan context");
    if (index >= textures_.size()) return Status::error("overlay texture index is out of range");
    return upload_texture(textures_[index], VK_FORMAT_R8G8B8A8_SRGB, width, height,
                          rgba_pixels);
}

Status OverlayRenderer::set_quads(std::vector<OverlayQuad> quads) {
    if (context_ == nullptr) return Status::error("the overlay renderer has no Vulkan context");
    for (const OverlayQuad& quad : quads) {
        if (quad.texture >= textures_.size()) {
            return Status::error("overlay quad uses an unknown texture index " +
                                 std::to_string(quad.texture));
        }
    }
    quads_ = std::move(quads);
    if (quads_.empty()) {
        destroy_vulkan_buffer(*context_, vertices_);
        return Status::success();
    }

    std::vector<OverlayVertex> vertices;
    vertices.reserve(quads_.size() * 6);
    // Two triangles per quad, corners in +u/+v space: top-left, bottom-left,
    // bottom-right, top-left, bottom-right, top-right. v = 0 is the top edge.
    const float corners[6][2] = {{-0.5f, 0.5f},  {-0.5f, -0.5f}, {0.5f, -0.5f},
                                 {-0.5f, 0.5f}, {0.5f, -0.5f},  {0.5f, 0.5f}};
    for (const OverlayQuad& quad : quads_) {
        for (const auto& corner : corners) {
            OverlayVertex vertex{};
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = quad.center[axis] +
                                        quad.right[axis] * (corner[0] * quad.width) +
                                        quad.up[axis] * (corner[1] * quad.height);
            }
            vertex.uv[0] = corner[0] + 0.5f;
            vertex.uv[1] = 0.5f - corner[1];
            std::memcpy(vertex.tint, quad.tint, sizeof(vertex.tint));
            vertices.push_back(vertex);
        }
    }
    const VkDeviceSize bytes = vertices.size() * sizeof(OverlayVertex);
    // The HUD moves every frame while a gesture runs, and the vertex buffer is
    // host-visible and stays mapped: rewriting the same-size buffer in place is
    // safe between frames (SplatRenderer::render waits for its fence) and keeps
    // ACO/Vulkan allocation out of the frame. A different size recreates it.
    if (vertices_.mapped != nullptr && vertices_.size == bytes) {
        std::memcpy(vertices_.mapped, vertices.data(), static_cast<size_t>(bytes));
        return Status::success();
    }
    destroy_vulkan_buffer(*context_, vertices_);
    auto buffer = create_vulkan_buffer(*context_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       vertices.data());
    if (!buffer.ok()) {
        quads_.clear();
        return Status::error(buffer.message());
    }
    vertices_ = buffer.take();
    return Status::success();
}

void OverlayRenderer::record(VkCommandBuffer command_buffer, const OverlayCamera& camera,
                             bool srgb_target) {
    if (quads_.empty() || vertices_.buffer == VK_NULL_HANDLE) return;
    const bool alt = alt_format_ != target_format_ && srgb_target != is_srgb_format(target_format_);
    const VkPipeline mask_pipeline = alt ? pipeline_alt_ : pipeline_;
    const VkPipeline rgba_pipeline = alt ? rgba_pipeline_alt_ : rgba_pipeline_;
    OverlayPush push{};
    std::memcpy(push.view, camera.view, sizeof(push.view));
    push.intrinsics[0] = camera.intrinsics.fx;
    push.intrinsics[1] = camera.intrinsics.fy;
    push.intrinsics[2] = camera.intrinsics.cx;
    push.intrinsics[3] = camera.intrinsics.cy;
    push.screen[0] = static_cast<float>(camera.width);
    push.screen[1] = static_cast<float>(camera.height);
    push.screen[2] = 0.0f;
    push.screen[3] = 0.0f;
    push.eye[0] = camera.eye_offset;
    push.eye[1] = camera.cx_shift;
    push.eye[2] = 0.0f;
    push.eye[3] = 0.0f;
    // A UNORM attachment stores what the shader writes, so the shaders encode
    // the linear tint/texel themselves; an sRGB attachment does it on store.
    push.encode[0] = srgb_target ? 0.0f : 1.0f;
    push.encode[1] = 0.0f;
    push.encode[2] = 0.0f;
    push.encode[3] = 0.0f;

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mask_pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertices_.buffer, &offset);
    vkCmdPushConstants(command_buffer, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    VkPipeline bound = mask_pipeline;
    for (size_t i = 0; i < quads_.size(); ++i) {
        const Texture& texture = textures_[quads_[i].texture];
        // An RGBA panel texture draws its own colours; an R8 mask is tinted.
        const VkPipeline wanted =
            texture.format == VK_FORMAT_R8G8B8A8_SRGB ? rgba_pipeline : mask_pipeline;
        if (wanted != bound) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
            bound = wanted;
        }
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &texture.set, 0, nullptr);
        vkCmdDraw(command_buffer, 6, 1, static_cast<uint32_t>(6 * i), 0);
    }
}

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
