#include "render/splat_renderer.h"

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "common/half.h"
#include "render/overlay.h"

namespace splat {
namespace {

constexpr float kDefaultHorizontalFovDegrees = 60.0f;
// Anything nearer than this is inside the viewer's face. A splat at a few
// centimetres projects enormous even with a small covariance, so it is culled
// before it can fill the eye; the projection shader also caps the screen radius
// (kMaxScreenRadiusPx in project.comp) for the ones that pass.
constexpr float kNearPlane = 0.05f;
constexpr float kFarPlane = 1000.0f;
constexpr uint32_t kProjectLocalSize = 256;
constexpr uint32_t kTimestampCount = 4;

struct CameraUniforms {
    float view[16];         // world -> camera, column-major
    float intrinsics[4];    // fx, fy, cx, cy
    float screen[4];        // width, height, near, far
    float params[4];        // scene_scale, half ipd, srgb_target, comfort_fade
    float backdrop[4];      // neutral backdrop, linear RGB
    uint32_t counts[4];     // splat count
    // The blocks below are appended after `counts` so the offsets of the fields
    // the vertex/fragment shaders declare do not move. comfort.frag reads the
    // capture fade, project.comp reads `aa`.
    float capture[4];       // capture fx, fy, cx, cy (zero disables the fade)
    float capture_size[4];  // capture width, height, outer margin, inner margin
    float camera_position[4];  // midpoint eye position; w = half IPD in stereo
    // Per-eye principal-point x offset in pixels (left, right): the asymmetric
    // frustum of a headset whose optics are not centred on the panels.
    float eye_shift[4];
    // Anti-aliasing, see RendererOptions.
    float aa[4];  // low-pass eps px^2, compensate flag, near-fade end m, unused
};
static_assert(sizeof(CameraUniforms) == 224, "camera uniform layout");

// Linear -> sRGB-encoded. Only used for UNORM targets, which blend in encoded
// space; an sRGB target converts the clear colour itself.
float encode_srgb(float value) {
    if (value <= 0.0031308f) return 12.92f * value;
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

bool is_srgb_format(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

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

Result<VkShaderModule> load_shader_module(VulkanContext& context, const std::string& path) {
    std::vector<char> spirv;
    std::string error;
    if (!read_file(path, spirv, error)) {
        return Result<VkShaderModule>::failure(error);
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size();
    info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(context.device(), &info, nullptr, &module);
    if (result != VK_SUCCESS) {
        return Result<VkShaderModule>::failure(vk_error("vkCreateShaderModule", result));
    }
    return Result<VkShaderModule>::success(module);
}

void memory_barrier(VkCommandBuffer command_buffer, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

void image_barrier(VkCommandBuffer command_buffer, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access,
                   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
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
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

Result<VulkanBuffer> upload_half_array(VulkanContext& context, const std::vector<float>& source,
                                       size_t values) {
    std::vector<uint16_t> halves(values);
    for (size_t i = 0; i < values; ++i) halves[i] = float_to_half(source[i]);
    return create_vulkan_buffer(context, halves.size() * sizeof(uint16_t),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, halves.data());
}

CameraIntrinsics resolve_intrinsics(const RendererOptions& options) {
    CameraIntrinsics intrinsics = options.intrinsics;
    if (!intrinsics.valid()) {
        const float width = static_cast<float>(options.eye_width);
        const float height = static_cast<float>(options.eye_height);
        const float fx = 0.5f * width / std::tan(0.5f * kDefaultHorizontalFovDegrees *
                                                3.14159265358979323846f / 180.0f);
        intrinsics = {fx, fx, 0.5f * width, 0.5f * height};
    }
    return intrinsics;
}

}  // namespace

VkFormat blend_alt_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        default: return format;
    }
}

SplatRenderer::~SplatRenderer() { destroy(); }

void SplatRenderer::destroy() {
    if (context_ == nullptr) return;
    VkDevice device = context_->device();
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        // The overlay shares this context; destroy it while the device is
        // still valid.
        overlay_.reset();
        if (command_buffer_ != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, context_->command_pool(), 1, &command_buffer_);
            command_buffer_ = VK_NULL_HANDLE;
        }
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
        }
        if (depth_sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device, depth_sampler_, nullptr);
            depth_sampler_ = VK_NULL_HANDLE;
        }
        destroy_vulkan_image(*context_, depth_image_);
        if (query_pool_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, query_pool_, nullptr);
            query_pool_ = VK_NULL_HANDLE;
        }
        if (comfort_pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, comfort_pipeline_, nullptr);
            comfort_pipeline_ = VK_NULL_HANDLE;
        }
        if (comfort_pipeline_alt_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, comfort_pipeline_alt_, nullptr);
            comfort_pipeline_alt_ = VK_NULL_HANDLE;
        }
        if (graphics_pipeline_alt_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphics_pipeline_alt_, nullptr);
            graphics_pipeline_alt_ = VK_NULL_HANDLE;
        }
        if (graphics_pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphics_pipeline_, nullptr);
            graphics_pipeline_ = VK_NULL_HANDLE;
        }
        if (finalize_pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, finalize_pipeline_, nullptr);
            finalize_pipeline_ = VK_NULL_HANDLE;
        }
        if (project_pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, project_pipeline_, nullptr);
            project_pipeline_ = VK_NULL_HANDLE;
        }
        if (comfort_fragment_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, comfort_fragment_module_, nullptr);
            comfort_fragment_module_ = VK_NULL_HANDLE;
        }
        if (comfort_vertex_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, comfort_vertex_module_, nullptr);
            comfort_vertex_module_ = VK_NULL_HANDLE;
        }
        if (fragment_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, fragment_module_, nullptr);
            fragment_module_ = VK_NULL_HANDLE;
        }
        if (vertex_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, vertex_module_, nullptr);
            vertex_module_ = VK_NULL_HANDLE;
        }
        if (finalize_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, finalize_module_, nullptr);
            finalize_module_ = VK_NULL_HANDLE;
        }
        if (project_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, project_module_, nullptr);
            project_module_ = VK_NULL_HANDLE;
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
            pipeline_layout_ = VK_NULL_HANDLE;
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
            descriptor_pool_ = VK_NULL_HANDLE;
        }
        if (set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
            set_layout_ = VK_NULL_HANDLE;
        }
        sort_.reset();
        staged_sort_.reset();
        for (auto& buffer : scene_) destroy_vulkan_buffer(*context_, buffer);
        for (auto& buffer : staged_scene_) destroy_vulkan_buffer(*context_, buffer);
        destroy_vulkan_buffer(*context_, staged_instances_);
        destroy_vulkan_buffer(*context_, staged_keys_);
        destroy_vulkan_buffer(*context_, staged_values_);
        destroy_vulkan_buffer(*context_, instances_);
        destroy_vulkan_buffer(*context_, keys_);
        destroy_vulkan_buffer(*context_, values_);
        destroy_vulkan_buffer(*context_, counters_);
        destroy_vulkan_buffer(*context_, draw_);
        destroy_vulkan_buffer(*context_, camera_);
        destroy_vulkan_buffer(*context_, readback_);
    }
    context_ = nullptr;
}

VkExtent2D SplatRenderer::target_extent() const {
    return {options_.eye_width * (options_.stereo ? 2u : 1u), options_.eye_height};
}

void SplatRenderer::set_intrinsics(const CameraIntrinsics& eye, const CameraIntrinsics& capture,
                                   uint32_t capture_width, uint32_t capture_height) {
    // render() resolves and uploads these on every frame, so the next frame is
    // the first one to use them; no buffer or pipeline changes with them.
    options_.intrinsics = eye;
    options_.capture_intrinsics = capture;
    options_.capture_width = capture_width;
    options_.capture_height = capture_height;
}

Result<std::unique_ptr<SplatRenderer>> SplatRenderer::create(VulkanContext& context,
                                                             const RendererOptions& options,
                                                             const SplatScene& scene) {
    if (options.eye_width == 0 || options.eye_height == 0) {
        return Result<std::unique_ptr<SplatRenderer>>::failure("render size must be non-zero");
    }
    if (!scene.consistent()) {
        return Result<std::unique_ptr<SplatRenderer>>::failure("scene attribute arrays disagree");
    }

    std::unique_ptr<SplatRenderer> renderer(new SplatRenderer());
    renderer->context_ = &context;
    renderer->options_ = options;
    renderer->count_ = static_cast<uint32_t>(scene.count());
    renderer->capacity_ = std::max<uint32_t>(renderer->count_, 1);

    auto fail = [&](const std::string& message) {
        return Result<std::unique_ptr<SplatRenderer>>::failure(message);
    };

    // Scene: five fp16 struct-of-arrays storage buffers.
    {
        const Status status = renderer->upload_scene_attributes(scene, renderer->scene_);
        if (!status.ok()) return fail(status.message());
    }

    const uint32_t max_instances = renderer->capacity_;
    auto instances = create_vulkan_buffer(context, max_instances * 3 * sizeof(float) * 4,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!instances.ok()) return fail(instances.message());
    renderer->instances_ = instances.take();

    auto keys = create_vulkan_buffer(context, max_instances * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!keys.ok()) return fail(keys.message());
    renderer->keys_ = keys.take();

    auto values = create_vulkan_buffer(context, max_instances * sizeof(uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!values.ok()) return fail(values.message());
    renderer->values_ = values.take();

    auto counters = create_vulkan_buffer(
        context, 64,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (!counters.ok()) return fail(counters.message());
    renderer->counters_ = counters.take();

    auto draw = create_vulkan_buffer(
        context, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (!draw.ok()) return fail(draw.message());
    renderer->draw_ = draw.take();

    const CameraUniforms zero_uniforms{};
    auto camera = create_vulkan_buffer(context, sizeof(CameraUniforms),
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &zero_uniforms);
    if (!camera.ok()) return fail(camera.message());
    renderer->camera_ = camera.take();

    // Depth attachment for the comfort pass: the splat pass writes the nearest
    // splat depth per pixel and the comfort pass reads it back to test the
    // pixel's hit point against the capture image.
    auto depth = create_vulkan_depth_image(context, renderer->target_extent());
    if (!depth.ok()) return fail(depth.message());
    renderer->depth_image_ = depth.take();
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    const VkResult sampler_result = vkCreateSampler(context.device(), &sampler_info, nullptr,
                                                    &renderer->depth_sampler_);
    if (sampler_result != VK_SUCCESS) {
        return fail(vk_error("vkCreateSampler", sampler_result));
    }

    auto sort = RadixSort::create(context, max_instances);
    if (!sort.ok()) return fail(sort.message());
    renderer->sort_ = sort.take();

    // Descriptor set layout: camera at 0, splat arrays at 1..5, instances at 6,
    // keys/values at 7/8, counters at 9, indirect draw at 10, the depth image
    // sampled by the comfort pass at 11.
    VkDescriptorSetLayoutBinding bindings[12]{};
    for (uint32_t i = 0; i < 12; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType =
            i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 12;
    layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(context.device(), &layout_info, nullptr,
                                                  &renderer->set_layout_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateDescriptorSetLayout", result));

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    // Eye offset in metres and the eye's principal-point x shift in pixels
    // (splat.vert): two floats.
    push.size = 2 * sizeof(float);

    VkPipelineLayoutCreateInfo pipeline_layout{};
    pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &renderer->set_layout_;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(context.device(), &pipeline_layout, nullptr,
                                    &renderer->pipeline_layout_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreatePipelineLayout", result));

    VkDescriptorPoolSize pool_sizes[3]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 10;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[2].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(context.device(), &pool_info, nullptr,
                                    &renderer->descriptor_pool_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateDescriptorPool", result));

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = renderer->descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &renderer->set_layout_;
    result = vkAllocateDescriptorSets(context.device(), &allocate, &renderer->descriptor_set_);
    if (result != VK_SUCCESS) return fail(vk_error("vkAllocateDescriptorSets", result));

    VkDescriptorBufferInfo buffer_infos[11]{};
    buffer_infos[0] = {renderer->camera_.buffer, 0, sizeof(CameraUniforms)};
    for (int i = 0; i < 5; ++i) {
        buffer_infos[1 + i] = {renderer->scene_[i].buffer, 0, VK_WHOLE_SIZE};
    }
    buffer_infos[6] = {renderer->instances_.buffer, 0, VK_WHOLE_SIZE};
    buffer_infos[7] = {renderer->keys_.buffer, 0, VK_WHOLE_SIZE};
    buffer_infos[8] = {renderer->values_.buffer, 0, VK_WHOLE_SIZE};
    buffer_infos[9] = {renderer->counters_.buffer, 0, VK_WHOLE_SIZE};
    buffer_infos[10] = {renderer->draw_.buffer, 0, VK_WHOLE_SIZE};

    VkDescriptorImageInfo depth_info{};
    depth_info.sampler = renderer->depth_sampler_;
    depth_info.imageView = renderer->depth_image_.view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[12]{};
    for (uint32_t i = 0; i < 11; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = renderer->descriptor_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pBufferInfo = &buffer_infos[i];
    }
    writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[11].dstSet = renderer->descriptor_set_;
    writes[11].dstBinding = 11;
    writes[11].descriptorCount = 1;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[11].pImageInfo = &depth_info;
    vkUpdateDescriptorSets(context.device(), 12, writes, 0, nullptr);

    const std::string shader_dir = options.shader_dir;
    auto project_module = load_shader_module(context, shader_dir + "/project.comp.spv");
    if (!project_module.ok()) return fail(project_module.message());
    renderer->project_module_ = project_module.take();
    auto finalize_module = load_shader_module(context, shader_dir + "/finalize.comp.spv");
    if (!finalize_module.ok()) return fail(finalize_module.message());
    renderer->finalize_module_ = finalize_module.take();
    auto vertex_module = load_shader_module(context, shader_dir + "/splat.vert.spv");
    if (!vertex_module.ok()) return fail(vertex_module.message());
    renderer->vertex_module_ = vertex_module.take();
    auto fragment_module = load_shader_module(context, shader_dir + "/splat.frag.spv");
    if (!fragment_module.ok()) return fail(fragment_module.message());
    renderer->fragment_module_ = fragment_module.take();
    auto comfort_vertex_module = load_shader_module(context, shader_dir + "/comfort.vert.spv");
    if (!comfort_vertex_module.ok()) return fail(comfort_vertex_module.message());
    renderer->comfort_vertex_module_ = comfort_vertex_module.take();
    auto comfort_fragment_module = load_shader_module(context, shader_dir + "/comfort.frag.spv");
    if (!comfort_fragment_module.ok()) return fail(comfort_fragment_module.message());
    renderer->comfort_fragment_module_ = comfort_fragment_module.take();

    VkComputePipelineCreateInfo compute_info{};
    compute_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute_info.stage.pName = "main";
    compute_info.layout = renderer->pipeline_layout_;

    compute_info.stage.module = renderer->project_module_;
    result = vkCreateComputePipelines(context.device(), VK_NULL_HANDLE, 1, &compute_info,
                                      nullptr, &renderer->project_pipeline_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateComputePipelines (project)", result));

    compute_info.stage.module = renderer->finalize_module_;
    result = vkCreateComputePipelines(context.device(), VK_NULL_HANDLE, 1, &compute_info,
                                      nullptr, &renderer->finalize_pipeline_);
    if (result != VK_SUCCESS) {
        return fail(vk_error("vkCreateComputePipelines (finalize)", result));
    }

    // Graphics pipeline for instanced, alpha-blended quads into a dynamic
    // rendering target. Built for the requested format and for its
    // UNORM/sRGB counterpart, so RenderTarget::format can choose the blend
    // space at runtime (VR8's "Blending" setting). Format is the only
    // difference between the two pipelines.
    renderer->alt_format_ = blend_alt_format(options.target_format);
    auto make_graphics = [&](VkFormat format, VkPipeline* out) -> VkResult {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = renderer->vertex_module_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = renderer->fragment_module_;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
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
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo color_blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        color_blend.attachmentCount = 1;
        color_blend.pAttachments = &blend;
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        // The splat pass writes the nearest splat depth (free of z-fighting:
        // the test is ALWAYS and the splats are sorted far to near, so the last
        // write is the nearest). The colour pass never tests depth.
        VkPipelineDepthStencilStateCreateInfo depth_state{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth_state.depthTestEnable = VK_TRUE;
        depth_state.depthWriteEnable = VK_TRUE;
        depth_state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depth_state.depthBoundsTestEnable = VK_FALSE;
        depth_state.stencilTestEnable = VK_FALSE;

        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

        VkGraphicsPipelineCreateInfo graphics{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        graphics.pNext = &rendering;
        graphics.stageCount = 2;
        graphics.pStages = stages;
        graphics.pVertexInputState = &vertex_input;
        graphics.pInputAssemblyState = &input_assembly;
        graphics.pViewportState = &viewport_state;
        graphics.pRasterizationState = &raster;
        graphics.pMultisampleState = &multisample;
        graphics.pColorBlendState = &color_blend;
        graphics.pDepthStencilState = &depth_state;
        graphics.pDynamicState = &dynamic;
        graphics.layout = renderer->pipeline_layout_;
        return vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1, &graphics,
                                         nullptr, out);
    };
    result = make_graphics(options.target_format, &renderer->graphics_pipeline_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateGraphicsPipelines", result));

    // Full-target comfort blend: premultiplied backdrop over the splat image.
    auto make_comfort = [&](VkFormat format, VkPipeline* out) -> VkResult {
        VkPipelineShaderStageCreateInfo comfort_stages[2]{};
        comfort_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        comfort_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        comfort_stages[0].module = renderer->comfort_vertex_module_;
        comfort_stages[0].pName = "main";
        comfort_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        comfort_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        comfort_stages[1].module = renderer->comfort_fragment_module_;
        comfort_stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo comfort_vertex_input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo comfort_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        comfort_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo comfort_viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        comfort_viewport.viewportCount = 1;
        comfort_viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo comfort_raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        comfort_raster.polygonMode = VK_POLYGON_MODE_FILL;
        comfort_raster.cullMode = VK_CULL_MODE_NONE;
        comfort_raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        comfort_raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo comfort_multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        comfort_multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState comfort_blend{};
        comfort_blend.blendEnable = VK_TRUE;
        comfort_blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        comfort_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        comfort_blend.colorBlendOp = VK_BLEND_OP_ADD;
        comfort_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        comfort_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        comfort_blend.alphaBlendOp = VK_BLEND_OP_ADD;
        comfort_blend.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo comfort_color{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        comfort_color.attachmentCount = 1;
        comfort_color.pAttachments = &comfort_blend;
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo comfort_dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        comfort_dynamic.dynamicStateCount = 2;
        comfort_dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo comfort_rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        comfort_rendering.colorAttachmentCount = 1;
        comfort_rendering.pColorAttachmentFormats = &format;

        VkGraphicsPipelineCreateInfo comfort{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        comfort.pNext = &comfort_rendering;
        comfort.stageCount = 2;
        comfort.pStages = comfort_stages;
        comfort.pVertexInputState = &comfort_vertex_input;
        comfort.pInputAssemblyState = &comfort_assembly;
        comfort.pViewportState = &comfort_viewport;
        comfort.pRasterizationState = &comfort_raster;
        comfort.pMultisampleState = &comfort_multisample;
        comfort.pColorBlendState = &comfort_color;
        comfort.pDynamicState = &comfort_dynamic;
        comfort.layout = renderer->pipeline_layout_;
        return vkCreateGraphicsPipelines(context.device(), VK_NULL_HANDLE, 1, &comfort, nullptr,
                                         out);
    };
    result = make_comfort(options.target_format, &renderer->comfort_pipeline_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateGraphicsPipelines (comfort)", result));

    if (renderer->alt_format_ != options.target_format) {
        result = make_graphics(renderer->alt_format_, &renderer->graphics_pipeline_alt_);
        if (result != VK_SUCCESS) {
            return fail(vk_error("vkCreateGraphicsPipelines (alt format)", result));
        }
        result = make_comfort(renderer->alt_format_, &renderer->comfort_pipeline_alt_);
        if (result != VK_SUCCESS) {
            return fail(vk_error("vkCreateGraphicsPipelines (comfort alt)", result));
        }
    }

    if (context.timestamps_supported()) {
        VkQueryPoolCreateInfo query_info{};
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = kTimestampCount;
        result = vkCreateQueryPool(context.device(), &query_info, nullptr,
                                   &renderer->query_pool_);
        if (result != VK_SUCCESS) return fail(vk_error("vkCreateQueryPool", result));
    }

    VkCommandBufferAllocateInfo command_allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate.commandPool = context.command_pool();
    command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(context.device(), &command_allocate,
                                      &renderer->command_buffer_);
    if (result != VK_SUCCESS) return fail(vk_error("vkAllocateCommandBuffers", result));

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(context.device(), &fence_info, nullptr, &renderer->fence_);
    if (result != VK_SUCCESS) return fail(vk_error("vkCreateFence", result));

    return Result<std::unique_ptr<SplatRenderer>>::success(std::move(renderer));
}

Status SplatRenderer::upload_scene_attributes(const SplatScene& scene,
                                               VulkanBuffer attributes[5]) {
    if (!scene.consistent()) return Status::error("scene attribute arrays disagree");
    const size_t n = scene.count();
    const struct {
        const std::vector<float>* data;
        size_t values;
    } uploads[5] = {
        {&scene.position, n * 3},
        {&scene.log_scale, n * 3},
        {&scene.rotation, n * 4},
        {&scene.sh_dc, n * 3},
        {&scene.opacity_logit, n},
    };
    for (int i = 0; i < 5; ++i) {
        auto buffer = upload_half_array(*context_, *uploads[i].data, uploads[i].values);
        if (!buffer.ok()) {
            for (int j = 0; j < i; ++j) destroy_vulkan_buffer(*context_, attributes[j]);
            return Status::error(buffer.message());
        }
        attributes[i] = buffer.take();
    }
    return Status::success();
}

void SplatRenderer::destroy_attributes(VulkanBuffer attributes[5]) {
    for (int i = 0; i < 5; ++i) destroy_vulkan_buffer(*context_, attributes[i]);
}

void SplatRenderer::update_scene_descriptors() {
    VkDescriptorBufferInfo infos[8]{};
    for (int i = 0; i < 5; ++i) infos[i] = {scene_[i].buffer, 0, VK_WHOLE_SIZE};
    infos[5] = {instances_.buffer, 0, VK_WHOLE_SIZE};
    infos[6] = {keys_.buffer, 0, VK_WHOLE_SIZE};
    infos[7] = {values_.buffer, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_set_;
        writes[i].dstBinding = i + 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(context_->device(), 8, writes, 0, nullptr);
}

Status SplatRenderer::stage_scene(const SplatScene& scene) {
    if (context_ == nullptr) return Status::error("renderer has no context");
    if (has_staged_) return Status::error("a scene is already staged");
    if (!scene.consistent()) return Status::error("scene attribute arrays disagree");

    const Status status = upload_scene_attributes(scene, staged_scene_);
    if (!status.ok()) return status;

    staged_count_ = static_cast<uint32_t>(scene.count());
    staged_aux_ = false;
    if (staged_count_ > capacity_) {
        // The staged scene needs its own instance/key/value arrays and sorter;
        // they become live at commit together with the attribute buffers.
        const uint32_t instances = std::max<uint32_t>(staged_count_, 1);
        VulkanBuffer instance_buffer;
        VulkanBuffer key_buffer;
        VulkanBuffer value_buffer;
        std::unique_ptr<RadixSort> sorter;
        std::string message;

        auto instance_result = create_vulkan_buffer(
            *context_, instances * 3 * sizeof(float) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (instance_result.ok()) {
            instance_buffer = instance_result.take();
        } else {
            message = instance_result.message();
        }
        if (message.empty()) {
            auto key_result = create_vulkan_buffer(*context_, instances * sizeof(uint32_t),
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (key_result.ok()) {
                key_buffer = key_result.take();
            } else {
                message = key_result.message();
            }
        }
        if (message.empty()) {
            auto value_result = create_vulkan_buffer(*context_, instances * sizeof(uint32_t),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (value_result.ok()) {
                value_buffer = value_result.take();
            } else {
                message = value_result.message();
            }
        }
        if (message.empty()) {
            auto sorter_result = RadixSort::create(*context_, instances);
            if (sorter_result.ok()) {
                sorter = sorter_result.take();
            } else {
                message = sorter_result.message();
            }
        }
        if (message.empty()) {
            staged_instances_ = instance_buffer;
            staged_keys_ = key_buffer;
            staged_values_ = value_buffer;
            staged_sort_ = std::move(sorter);
            staged_aux_ = true;
        } else {
            destroy_vulkan_buffer(*context_, instance_buffer);
            destroy_vulkan_buffer(*context_, key_buffer);
            destroy_vulkan_buffer(*context_, value_buffer);
            destroy_attributes(staged_scene_);
            staged_count_ = 0;
            return Status::error(message);
        }
    }
    has_staged_ = true;
    return Status::success();
}

VkDeviceSize SplatRenderer::staged_bytes() const {
    if (!has_staged_) return 0;
    VkDeviceSize bytes = 0;
    for (const auto& buffer : staged_scene_) bytes += buffer.size;
    if (staged_aux_) {
        bytes += staged_instances_.size + staged_keys_.size + staged_values_.size;
    }
    return bytes;
}

Status SplatRenderer::commit_staged_scene() {
    if (context_ == nullptr) return Status::error("renderer has no context");
    if (!has_staged_) return Status::success();

    const auto start = std::chrono::steady_clock::now();
    const VkResult idle = vkDeviceWaitIdle(context_->device());
    if (idle != VK_SUCCESS) return Status::error(vk_error("vkDeviceWaitIdle", idle));

    destroy_attributes(scene_);
    for (int i = 0; i < 5; ++i) {
        scene_[i] = staged_scene_[i];
        staged_scene_[i] = VulkanBuffer{};
    }
    count_ = staged_count_;
    if (staged_aux_) {
        destroy_vulkan_buffer(*context_, instances_);
        destroy_vulkan_buffer(*context_, keys_);
        destroy_vulkan_buffer(*context_, values_);
        instances_ = staged_instances_;
        keys_ = staged_keys_;
        values_ = staged_values_;
        staged_instances_ = VulkanBuffer{};
        staged_keys_ = VulkanBuffer{};
        staged_values_ = VulkanBuffer{};
        sort_ = std::move(staged_sort_);
        capacity_ = std::max<uint32_t>(count_, 1);
        staged_aux_ = false;
    }
    update_scene_descriptors();
    has_staged_ = false;
    staged_count_ = 0;
    ++scene_generation_;
    last_commit_ms_ = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    return Status::success();
}

void SplatRenderer::discard_staged_scene() {
    if (context_ == nullptr || !has_staged_) return;
    vkDeviceWaitIdle(context_->device());
    destroy_attributes(staged_scene_);
    destroy_vulkan_buffer(*context_, staged_instances_);
    destroy_vulkan_buffer(*context_, staged_keys_);
    destroy_vulkan_buffer(*context_, staged_values_);
    staged_sort_.reset();
    has_staged_ = false;
    staged_count_ = 0;
    staged_aux_ = false;
}

Status SplatRenderer::ensure_overlay() {
    if (context_ == nullptr) return Status::error("renderer has no context");
    if (overlay_ != nullptr) return Status::success();
    auto overlay = OverlayRenderer::create(*context_, options_.shader_dir,
                                           options_.target_format);
    if (!overlay.ok()) return Status::error(overlay.message());
    overlay_ = overlay.take();
    return Status::success();
}

Result<uint32_t> SplatRenderer::add_overlay_texture(uint32_t width, uint32_t height,
                                                    const uint8_t* r8_pixels) {
    const Status status = ensure_overlay();
    if (!status.ok()) return Result<uint32_t>::failure(status.message());
    return overlay_->add_texture(width, height, r8_pixels);
}

Status SplatRenderer::update_overlay_texture(uint32_t index, uint32_t width, uint32_t height,
                                             const uint8_t* r8_pixels) {
    const Status status = ensure_overlay();
    if (!status.ok()) return status;
    return overlay_->update_texture(index, width, height, r8_pixels);
}

Result<uint32_t> SplatRenderer::add_overlay_rgba_texture(uint32_t width, uint32_t height,
                                                         const uint8_t* rgba_pixels) {
    const Status status = ensure_overlay();
    if (!status.ok()) return Result<uint32_t>::failure(status.message());
    return overlay_->add_rgba_texture(width, height, rgba_pixels);
}

Status SplatRenderer::update_overlay_rgba_texture(uint32_t index, uint32_t width,
                                                  uint32_t height,
                                                  const uint8_t* rgba_pixels) {
    const Status status = ensure_overlay();
    if (!status.ok()) return status;
    return overlay_->update_rgba_texture(index, width, height, rgba_pixels);
}

Status SplatRenderer::set_overlay_quads(std::vector<OverlayQuad> quads) {
    const Status status = ensure_overlay();
    if (!status.ok()) return status;
    return overlay_->set_quads(std::move(quads));
}

void SplatRenderer::clear_overlay_quads() {
    if (overlay_ != nullptr) overlay_->set_quads({});
}

bool SplatRenderer::has_overlay_quads() const {
    return overlay_ != nullptr && overlay_->has_quads();
}

Status SplatRenderer::render(const FrameView& view, const RenderTarget& target, bool readback) {
    if (context_ == nullptr) return Status::error("renderer has no context");
    const VkExtent2D expected = target_extent();
    if (target.extent.width != expected.width || target.extent.height != expected.height) {
        return Status::error("render target extent does not match the renderer");
    }
    // The caller's image format picks the blend-space pipeline: the renderer's
    // own target format or its UNORM/sRGB counterpart (the VR "Blending"
    // setting). VK_FORMAT_UNDEFINED keeps every pre-VR8 caller on the format
    // the renderer was created with.
    const VkFormat target_format =
        target.format != VK_FORMAT_UNDEFINED ? target.format : options_.target_format;
    if (target_format != options_.target_format && target_format != alt_format_) {
        return Status::error("render target format has no pipeline");
    }
    const bool blend_alt =
        alt_format_ != options_.target_format && target_format == alt_format_;
    const bool srgb_target = is_srgb_format(target_format);

    const CameraIntrinsics intrinsics = resolve_intrinsics(options_);
    CameraUniforms uniforms{};
    const float yaw = view.midpoint.yaw;
    const float pitch = view.midpoint.pitch;
    const float roll = view.midpoint.roll;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    // Forward, right and down axes in world space (OpenCV camera axes).
    const float f[3] = {sy * cp, sp, cy * cp};
    // s = normalize(cross(f, world up)) with world up = -y, and d = cross(f, s)
    // is the camera down axis: the level camera for this yaw and pitch.
    float s[3] = {cy, 0.0f, -sy};
    float d[3] = {f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2],
                  f[0] * s[1] - f[1] * s[0]};
    // Roll about the optical axis: the camera's right/down pair turns by `roll`
    // in the plane normal to f, which is exactly the head's own x and down axes
    // when the pose came from a tracked headset. Zero keeps the old view matrix.
    if (roll != 0.0f) {
        const float cr = std::cos(roll);
        const float sr = std::sin(roll);
        const float level_s[3] = {s[0], s[1], s[2]};
        const float level_d[3] = {d[0], d[1], d[2]};
        for (int axis = 0; axis < 3; ++axis) {
            s[axis] = cr * level_s[axis] + sr * level_d[axis];
            d[axis] = cr * level_d[axis] - sr * level_s[axis];
        }
    }
    const float* p = view.midpoint.position;
    uniforms.view[0] = s[0];
    uniforms.view[1] = d[0];
    uniforms.view[2] = f[0];
    uniforms.view[3] = 0.0f;
    uniforms.view[4] = s[1];
    uniforms.view[5] = d[1];
    uniforms.view[6] = f[1];
    uniforms.view[7] = 0.0f;
    uniforms.view[8] = s[2];
    uniforms.view[9] = d[2];
    uniforms.view[10] = f[2];
    uniforms.view[11] = 0.0f;
    uniforms.view[12] = -(s[0] * p[0] + s[1] * p[1] + s[2] * p[2]);
    uniforms.view[13] = -(d[0] * p[0] + d[1] * p[1] + d[2] * p[2]);
    uniforms.view[14] = -(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
    uniforms.view[15] = 1.0f;

    uniforms.intrinsics[0] = intrinsics.fx;
    uniforms.intrinsics[1] = intrinsics.fy;
    uniforms.intrinsics[2] = intrinsics.cx;
    uniforms.intrinsics[3] = intrinsics.cy;
    uniforms.screen[0] = static_cast<float>(options_.eye_width);
    uniforms.screen[1] = static_cast<float>(options_.eye_height);
    uniforms.screen[2] = kNearPlane;
    uniforms.screen[3] = kFarPlane;
    uniforms.params[0] = view.scene_scale;
    uniforms.params[1] = 0.5f * view.ipd;
    uniforms.params[2] = srgb_target ? 1.0f : 0.0f;
    uniforms.params[3] = std::clamp(view.comfort_fade, 0.0f, 1.0f);
    uniforms.backdrop[0] = options_.backdrop[0];
    uniforms.backdrop[1] = options_.backdrop[1];
    uniforms.backdrop[2] = options_.backdrop[2];
    uniforms.backdrop[3] = 1.0f;
    uniforms.counts[0] = count_;
    const bool capture_fade = options_.capture_intrinsics.valid() &&
                              options_.capture_width > 0 && options_.capture_height > 0;
    uniforms.capture[0] = capture_fade ? options_.capture_intrinsics.fx : 0.0f;
    uniforms.capture[1] = capture_fade ? options_.capture_intrinsics.fy : 0.0f;
    uniforms.capture[2] = capture_fade ? options_.capture_intrinsics.cx : 0.0f;
    uniforms.capture[3] = capture_fade ? options_.capture_intrinsics.cy : 0.0f;
    uniforms.capture_size[0] = capture_fade ? static_cast<float>(options_.capture_width) : 0.0f;
    uniforms.capture_size[1] = capture_fade ? static_cast<float>(options_.capture_height) : 0.0f;
    uniforms.capture_size[2] = std::max(1.0e-4f, options_.capture_margin);
    // The ramp starts this far inside the border so the capture's weak, washed
    // out border Gaussians fade as well. Capped at the outer margin: with
    // inner > outer the smoothstep edges would be reversed.
    uniforms.capture_size[3] =
        std::clamp(options_.capture_inner_margin, 0.0f, uniforms.capture_size[2]);
    uniforms.camera_position[0] = view.midpoint.position[0];
    uniforms.camera_position[1] = view.midpoint.position[1];
    uniforms.camera_position[2] = view.midpoint.position[2];
    // Half IPD on a stereo frame: the comfort pass shifts each eye's hit point
    // by this much along camera x, and project.comp widens its x cull by the
    // matching screen shift. Zero on mono, where the eye is the midpoint.
    uniforms.camera_position[3] = options_.stereo ? uniforms.params[1] : 0.0f;
    uniforms.eye_shift[0] = options_.stereo ? options_.eye_center_shift[0] : 0.0f;
    uniforms.eye_shift[1] = options_.stereo ? options_.eye_center_shift[1] : 0.0f;
    uniforms.eye_shift[2] = 0.0f;
    uniforms.eye_shift[3] = 0.0f;
    uniforms.aa[0] = std::max(0.0f, options_.low_pass_eps);
    uniforms.aa[1] = options_.low_pass_compensate ? 1.0f : 0.0f;
    uniforms.aa[2] = std::max(0.0f, options_.near_fade_end);
    uniforms.aa[3] = scale_vignette_;
    std::memcpy(camera_.mapped, &uniforms, sizeof(uniforms));

    // The sort orders the whole splat array (keys for culled splats are the
    // largest value), so the draw order is a deterministic function of the
    // scene and pose rather than of the atomic compaction order. counters[1]
    // is the element count for the indirect sort; counters[0] is reset below
    // and still becomes the draw instance count.
    if (counters_.mapped != nullptr) {
        static_cast<uint32_t*>(counters_.mapped)[1] = count_;
    }

    VkCommandBuffer command_buffer = command_buffer_;
    VkResult result = vkResetCommandBuffer(command_buffer, 0);
    if (result != VK_SUCCESS) return Status::error(vk_error("vkResetCommandBuffer", result));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &begin);
    if (result != VK_SUCCESS) return Status::error(vk_error("vkBeginCommandBuffer", result));

    if (query_pool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(command_buffer, query_pool_, 0, kTimestampCount);
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query_pool_, 0);
    }

    // Reset the visible counter, then cull + project.
    vkCmdFillBuffer(command_buffer, counters_.buffer, 0, sizeof(uint32_t), 0);
    memory_barrier(command_buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, project_pipeline_);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0,
                            1, &descriptor_set_, 0, nullptr);
    const uint32_t groups = (count_ + kProjectLocalSize - 1) / kProjectLocalSize;
    if (groups > 0) {
        vkCmdDispatch(command_buffer, groups, 1, 1);
    }

    memory_barrier(command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, finalize_pipeline_);
    vkCmdDispatch(command_buffer, 1, 1, 1);

    memory_barrier(command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    if (query_pool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, query_pool_,
                            1);
    }

    sort_->record(command_buffer, keys_.buffer, values_.buffer, counters_.buffer,
                  sizeof(uint32_t));

    memory_barrier(command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    if (query_pool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, query_pool_,
                            2);
    }

    // Draw both eyes into the target.
    const VkImageLayout final_layout =
        readback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : target.final_layout;
    const VkPipelineStageFlags2 src_stage =
        target.initial_layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_NONE
            : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkAccessFlags2 src_access = target.initial_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                          ? VK_ACCESS_2_NONE
                                          : VK_ACCESS_2_TRANSFER_READ_BIT;
    image_barrier(command_buffer, target.image, target.initial_layout,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, src_stage, src_access,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // The neutral backdrop the comfort fade blends toward. An sRGB attachment
    // converts the linear clear value itself; a UNORM target blends encoded
    // values, so encode the backdrop to match.
    color.clearValue.color = {{srgb_target ? options_.backdrop[0] : encode_srgb(options_.backdrop[0]),
                               srgb_target ? options_.backdrop[1] : encode_srgb(options_.backdrop[1]),
                               srgb_target ? options_.backdrop[2] : encode_srgb(options_.backdrop[2]),
                               1.0f}};
    // Depth is cleared, written by every splat (last write = nearest, since the
    // draw order is far to near) and then sampled by the comfort pass.
    image_barrier(command_buffer, depth_image_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                  VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depth_image_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0u};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(command_buffer, &rendering);

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      blend_alt ? graphics_pipeline_alt_ : graphics_pipeline_);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                            1, &descriptor_set_, 0, nullptr);

    const uint32_t eye_count = options_.stereo ? 2u : 1u;
    for (uint32_t eye = 0; eye < eye_count; ++eye) {
        VkViewport viewport{};
        viewport.x = static_cast<float>(eye * options_.eye_width);
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(options_.eye_width);
        viewport.height = static_cast<float>(options_.eye_height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = {static_cast<int32_t>(eye * options_.eye_width), 0};
        scissor.extent = {options_.eye_width, options_.eye_height};
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        // Left eye at midpoint - ipd/2, right eye at +ipd/2. The principal
        // point moves by that eye's own offset: a headset's per-eye frustum is
        // not centred on the panel, and the offset is a screen shift, not a
        // camera translation, so it belongs in the vertex shader's pixels.
        const float eye_offset = options_.stereo
                                     ? (eye == 0 ? -uniforms.params[1] : uniforms.params[1])
                                     : 0.0f;
        const struct {
            float eye_offset;
            float cx_shift;
        } push = {eye_offset,
                  options_.stereo ? options_.eye_center_shift[eye == 0 ? 0 : 1] : 0.0f};
        vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDrawIndirect(command_buffer, draw_.buffer, 0, 1,
                          sizeof(VkDrawIndirectCommand));
    }

    vkCmdEndRendering(command_buffer);
    image_barrier(command_buffer, depth_image_.image,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    image_barrier(command_buffer, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    // C7 comfort boundary: blend the whole splat image toward the neutral
    // backdrop across the full target. Doing it as a final full-screen pass
    // works for dense scenes where per-splat alpha fade would saturate.
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    rendering.pDepthAttachment = nullptr;
    vkCmdBeginRendering(command_buffer, &rendering);
    VkViewport comfort_viewport{};
    comfort_viewport.x = 0.0f;
    comfort_viewport.y = 0.0f;
    comfort_viewport.width = static_cast<float>(target.extent.width);
    comfort_viewport.height = static_cast<float>(target.extent.height);
    comfort_viewport.minDepth = 0.0f;
    comfort_viewport.maxDepth = 1.0f;
    VkRect2D comfort_scissor{};
    comfort_scissor.offset = {0, 0};
    comfort_scissor.extent = target.extent;
    vkCmdSetViewport(command_buffer, 0, 1, &comfort_viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &comfort_scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      blend_alt ? comfort_pipeline_alt_ : comfort_pipeline_);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                            1, &descriptor_set_, 0, nullptr);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRendering(command_buffer);

    if (query_pool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query_pool_, 3);
    }

    // Overlay UI (scene label, tooltips): world-space quads drawn after the
    // splats and after the comfort pass, so neither the splat depth nor the
    // fade can dim or hide them. LOAD keeps the finished frame; the quads
    // alpha-blend over it without a depth test.
    if (overlay_ != nullptr && overlay_->has_quads()) {
        image_barrier(command_buffer, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        rendering.pDepthAttachment = nullptr;
        vkCmdBeginRendering(command_buffer, &rendering);
        for (uint32_t eye = 0; eye < eye_count; ++eye) {
            VkViewport overlay_viewport{};
            overlay_viewport.x = static_cast<float>(eye * options_.eye_width);
            overlay_viewport.y = 0.0f;
            overlay_viewport.width = static_cast<float>(options_.eye_width);
            overlay_viewport.height = static_cast<float>(options_.eye_height);
            overlay_viewport.minDepth = 0.0f;
            overlay_viewport.maxDepth = 1.0f;
            VkRect2D overlay_scissor{};
            overlay_scissor.offset = {static_cast<int32_t>(eye * options_.eye_width), 0};
            overlay_scissor.extent = {options_.eye_width, options_.eye_height};
            vkCmdSetViewport(command_buffer, 0, 1, &overlay_viewport);
            vkCmdSetScissor(command_buffer, 0, 1, &overlay_scissor);

            OverlayCamera camera{};
            std::memcpy(camera.view, uniforms.view, sizeof(camera.view));
            camera.intrinsics = intrinsics;
            camera.width = options_.eye_width;
            camera.height = options_.eye_height;
            camera.eye_offset = options_.stereo
                                    ? (eye == 0 ? -uniforms.params[1] : uniforms.params[1])
                                    : 0.0f;
            camera.cx_shift =
                options_.stereo ? options_.eye_center_shift[eye == 0 ? 0 : 1] : 0.0f;
            overlay_->record(command_buffer, camera, srgb_target);
        }
        vkCmdEndRendering(command_buffer);
    }

    if (readback) {
        const VkDeviceSize bytes =
            VkDeviceSize(target.extent.width) * target.extent.height * 4;
        if (readback_.buffer == VK_NULL_HANDLE || readback_.size < bytes) {
            destroy_vulkan_buffer(*context_, readback_);
            auto buffer = create_vulkan_buffer(*context_, bytes,
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (!buffer.ok()) {
                return Status::error(buffer.message());
            }
            readback_ = buffer.take();
        }
        image_barrier(command_buffer, target.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {target.extent.width, target.extent.height, 1};
        vkCmdCopyImageToBuffer(command_buffer, target.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                               &region);
        memory_barrier(command_buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_HOST_READ_BIT);
    } else {
        image_barrier(command_buffer, target.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      final_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                          ? VK_PIPELINE_STAGE_2_NONE
                          : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      final_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                          ? VK_ACCESS_2_NONE
                          : VK_ACCESS_2_TRANSFER_READ_BIT);
    }

    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) return Status::error(vk_error("vkEndCommandBuffer", result));

    vkResetFences(context_->device(), 1, &fence_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    result = vkQueueSubmit(context_->queue(), 1, &submit, fence_);
    if (result != VK_SUCCESS) return Status::error(vk_error("vkQueueSubmit", result));
    result = vkWaitForFences(context_->device(), 1, &fence_, VK_TRUE,
                             10ull * 1000 * 1000 * 1000);
    if (result != VK_SUCCESS) return Status::error(vk_error("vkWaitForFences", result));

    timings_ = {};
    if (query_pool_ != VK_NULL_HANDLE) {
        uint64_t ticks[kTimestampCount] = {};
        result = vkGetQueryPoolResults(
            context_->device(), query_pool_, 0, kTimestampCount, sizeof(ticks), ticks,
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result == VK_SUCCESS) {
            const double ns = context_->timestamp_period();
            timings_.project_ms = static_cast<double>(ticks[1] - ticks[0]) * ns / 1.0e6;
            timings_.sort_ms = static_cast<double>(ticks[2] - ticks[1]) * ns / 1.0e6;
            timings_.draw_ms = static_cast<double>(ticks[3] - ticks[2]) * ns / 1.0e6;
        }
    }

    if (counters_.mapped != nullptr) {
        last_visible_count_ = *static_cast<const uint32_t*>(counters_.mapped);
    }
    has_readback_ = readback;
    return Status::success();
}

Result<Image> SplatRenderer::read_image() const {
    if (!has_readback_ || readback_.mapped == nullptr) {
        return Result<Image>::failure("the last render did not read the image back");
    }
    const VkExtent2D extent = target_extent();
    Image image;
    image.resize(static_cast<int>(extent.width), static_cast<int>(extent.height));
    const uint8_t* source = static_cast<const uint8_t*>(readback_.mapped);
    const bool bgr = options_.target_format == VK_FORMAT_B8G8R8A8_UNORM ||
                     options_.target_format == VK_FORMAT_B8G8R8A8_SRGB;
    const size_t pixels = static_cast<size_t>(extent.width) * extent.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* p = source + i * 4;
        uint8_t* out = image.rgb.data() + i * 3;
        out[0] = bgr ? p[2] : p[0];
        out[1] = p[1];
        out[2] = bgr ? p[0] : p[2];
    }
    return Result<Image>::success(std::move(image));
}

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
