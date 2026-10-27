#pragma once

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/image.h"
#include "common/status.h"
#include "render/camera.h"
#include "render/overlay_quad.h"
#include "render/radix_sort.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

namespace splat {

class OverlayRenderer;

struct RendererOptions {
    std::string shader_dir;
    uint32_t eye_width = 1280;
    uint32_t eye_height = 720;
    bool stereo = false;
    VkFormat target_format = VK_FORMAT_R8G8B8A8_UNORM;
    // Zero intrinsics means a 60 degree horizontal field of view.
    CameraIntrinsics intrinsics{};
    // Principal-point x offset per eye, in pixels of one eye image (left,
    // right), added to `intrinsics` for that eye only. A headset whose optics
    // are not centred on its panels has an asymmetric frustum per eye (PSVR2:
    // +-16 % of the eye width), which one shared `cx` cannot express. Zero on
    // a symmetric renderer, which is the desktop's stereo path.
    float eye_center_shift[2] = {0.0f, 0.0f};
    // Intrinsics and image size of the original capture camera. The comfort
    // pass fades view rays that fall outside this image, where the scene
    // extrapolates past what the photo saw. A zero size or invalid intrinsics
    // disables the capture-frustum fade.
    CameraIntrinsics capture_intrinsics{};
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    // Soft edge, as a fraction of the capture image size beyond its border.
    // 0.03125 is 40 px at a 1280-wide capture image (3.125 %), wide enough to
    // cover the speckled extrapolated band; the fraction keeps the band the
    // same relative size at any capture/render resolution.
    float capture_margin = 0.03125f;
    // Start of the ramp inside the same border, also a fraction of the capture
    // image size. The capture's own border Gaussians are weak and wash out, so
    // they must fade too instead of sitting at full opacity next to the
    // backdrop. 0.02 is 25.6 px at a 1280-wide capture image. The renderer caps
    // it at capture_margin, which keeps the smoothstep edges ordered.
    float capture_inner_margin = 0.02f;
    // Neutral backdrop the comfort fade blends toward, linear RGB. The clear
    // value is encoded when the target is UNORM (encoded-space blending).
    float backdrop[3] = {0.18f, 0.18f, 0.18f};
    // Screen-space anti-aliasing (Mip-Splatting's 2D filter): `low_pass_eps` is
    // added to the projected covariance diagonal (pixels^2) and, when
    // `low_pass_compensate` is set, the splat's alpha is scaled by
    // sqrt(det(Σ)/det(Σ+eps)) so its energy is unchanged. 0.3 is the published
    // 3DGS/Mip-Splatting value; see docs/render.md, "Anti-aliasing".
    float low_pass_eps = 0.3f;
    bool low_pass_compensate = true;
    // Alpha reaches full at this view depth, ramping up from the near plane
    // (0 = hard near cull): a splat crossing the near plane fades instead of
    // popping. 0.08 m is 3 cm of head travel past the 5 cm near plane.
    float near_fade_end = 0.08f;
};

// The UNORM/sRGB counterpart of an 8-bit colour format: sRGB -> UNORM,
// UNORM -> sRGB, anything else -> the same format. A renderer built for one
// builds its second pipeline set for the other, which is what lets the VR
// path switch "Blending: Linear" (sRGB attachment, linear-light blend) to
// "sRGB" (UNORM attachment, gamma-space blend like Spark) without recreating
// the renderer.
VkFormat blend_alt_format(VkFormat format);

struct PassTimings {
    double project_ms = 0.0;  // cull + project + indirect command
    double sort_ms = 0.0;     // radix sort
    double draw_ms = 0.0;     // both eyes
};

// Destination for a frame. The caller owns the image and view. With readback
// enabled the renderer forces final_layout to TRANSFER_SRC_OPTIMAL and copies
// the image into host memory for read_image().
struct RenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // The image's format. VK_FORMAT_UNDEFINED means "the renderer's
    // RendererOptions::target_format" (every caller before VR8). The renderer
    // also builds pipelines for the matching UNORM/sRGB counterpart
    // (blend_alt_format()), so the caller can pass either a linear-light (sRGB)
    // or a gamma-space (UNORM) target at runtime - the Display setting.
    VkFormat format = VK_FORMAT_UNDEFINED;
};

// C6 splat renderer: fp16 struct-of-arrays scene, one project+sort per frame,
// indirect instanced quads once per eye. One offscreen image of
// eye_width * (stereo ? 2 : 1) by eye_height.
//
// The renderer also owns a one-deep scene staging slot. A background thread
// reads the next scene file; the main thread calls stage_scene() to upload it
// into a second attribute buffer set while the current scene keeps rendering,
// then commit_staged_scene() to swap. The old buffers stay valid until the
// commit, so no frame ever sees a half-uploaded scene.
class SplatRenderer {
public:
    ~SplatRenderer();
    SplatRenderer(const SplatRenderer&) = delete;
    SplatRenderer& operator=(const SplatRenderer&) = delete;

    static Result<std::unique_ptr<SplatRenderer>> create(VulkanContext& context,
                                                         const RendererOptions& options,
                                                         const SplatScene& scene);

    // Records and submits one frame, then waits for it. With readback the
    // colour image is copied to host memory (read_image()).
    Status render(const FrameView& view, const RenderTarget& target, bool readback);

    // Uploads a scene into a second GPU buffer set without disturbing the
    // scene being rendered. Returns an error when a scene is already staged.
    Status stage_scene(const SplatScene& scene);
    bool has_staged_scene() const { return has_staged_; }
    uint32_t staged_count() const { return staged_count_; }
    // GPU bytes held by the staged scene: the five attribute buffers, plus new
    // instance/key/value buffers when the staged scene is larger than the
    // current capacity. This is the preload memory over the active scene.
    VkDeviceSize staged_bytes() const;
    // Waits for the device to idle, then swaps the staged scene in and rewrites
    // the descriptor set. The previous buffers are destroyed only after the
    // swap. No-op when nothing is staged.
    Status commit_staged_scene();
    void discard_staged_scene();

    uint64_t scene_generation() const { return scene_generation_; }
    uint32_t scene_count() const { return count_; }
    // Wall time of the last commit, including the device idle wait. A commit
    // is a descriptor rewrite and a buffer free, not a re-upload.
    double last_commit_ms() const { return last_commit_ms_; }

    // ---- overlay UI -----------------------------------------------------
    // World-space textured quads drawn after the splat and comfort passes,
    // alpha-blended and without a depth test. Resources are created on first
    // use from this renderer's Vulkan context and shader directory; there is
    // no second device. See render/overlay.h.
    Result<uint32_t> add_overlay_texture(uint32_t width, uint32_t height,
                                         const uint8_t* r8_pixels);
    Status update_overlay_texture(uint32_t index, uint32_t width, uint32_t height,
                                  const uint8_t* r8_pixels);
    // RGBA8 straight-alpha panel textures (an RGBA8 canvas, one upload per
    // change). Same draw path and blend state; see render/overlay.h.
    Result<uint32_t> add_overlay_rgba_texture(uint32_t width, uint32_t height,
                                              const uint8_t* rgba_pixels);
    Status update_overlay_rgba_texture(uint32_t index, uint32_t width, uint32_t height,
                                       const uint8_t* rgba_pixels);
    Status set_overlay_quads(std::vector<OverlayQuad> quads);
    void clear_overlay_quads();
    bool has_overlay_quads() const;

    // RGB8 top-down image of the last readback frame.
    Result<Image> read_image() const;

    const PassTimings& timings() const { return timings_; }
    uint32_t last_visible_count() const { return last_visible_count_; }
    uint32_t eye_width() const { return options_.eye_width; }
    uint32_t eye_height() const { return options_.eye_height; }
    bool stereo() const { return options_.stereo; }
    VkExtent2D target_extent() const;

    // Replaces the camera used from the next render(): the eye intrinsics (a
    // zero/invalid value means the 60 degree fallback) and the capture camera
    // the comfort fade uses (zero size or invalid intrinsics turns the fade
    // off). Called when a scene becomes the one on screen, so a converted scene
    // is drawn with its own meta.json camera and not the renderer's boot
    // intrinsics (W18 9.2). Does not touch the scene buffers or the overlay.
    void set_intrinsics(const CameraIntrinsics& eye, const CameraIntrinsics& capture,
                        uint32_t capture_width, uint32_t capture_height);

    // Screen-space AA low-pass (project.comp's aa.x), pixels^2: 0.3 is the
    // published Mip-Splatting value, 0.5 the "Soft" Settings option. A uniform
    // change only: no pipeline rebuild, no extra fragments beyond the slightly
    // wider 3-sigma quad.
    void set_low_pass_eps(float pixels_squared) {
        options_.low_pass_eps = pixels_squared > 0.0f ? pixels_squared : 0.0f;
    }
    float low_pass_eps() const { return options_.low_pass_eps; }

    // Light peripheral dimming while a scale press is active, 0..1 (VR8). The
    // comfort pass applies it; the value only reaches the frame's uniforms.
    void set_scale_vignette(float strength) {
        scale_vignette_ = strength > 0.0f ? std::min(strength, 1.0f) : 0.0f;
    }

private:
    SplatRenderer() = default;
    void destroy();
    Status ensure_overlay();

    Status upload_scene_attributes(const SplatScene& scene, VulkanBuffer attributes[5]);
    void destroy_attributes(VulkanBuffer attributes[5]);
    void update_scene_descriptors();

    VulkanContext* context_ = nullptr;
    RendererOptions options_;
    uint32_t count_ = 0;
    uint32_t capacity_ = 0;

    VulkanBuffer scene_[5];
    // Staged attribute buffers; `has_staged_` marks them live.
    VulkanBuffer staged_scene_[5];
    uint32_t staged_count_ = 0;
    bool has_staged_ = false;
    // Auxiliary buffers for a staged scene larger than the active capacity.
    VulkanBuffer staged_instances_;
    VulkanBuffer staged_keys_;
    VulkanBuffer staged_values_;
    std::unique_ptr<RadixSort> staged_sort_;
    bool staged_aux_ = false;

    VulkanBuffer instances_;
    VulkanBuffer keys_;
    VulkanBuffer values_;
    VulkanBuffer counters_;
    VulkanBuffer draw_;
    VulkanBuffer camera_;
    VulkanBuffer readback_;
    // D32 depth attachment, written by the splat pass and sampled by the
    // comfort pass to locate the nearest splat at each pixel.
    VulkanImage depth_image_;
    VkSampler depth_sampler_ = VK_NULL_HANDLE;

    std::unique_ptr<RadixSort> sort_;

    // Lazily created by the first add_overlay_texture()/set_overlay_quads().
    std::unique_ptr<OverlayRenderer> overlay_;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

    VkShaderModule project_module_ = VK_NULL_HANDLE;
    VkShaderModule finalize_module_ = VK_NULL_HANDLE;
    VkShaderModule vertex_module_ = VK_NULL_HANDLE;
    VkShaderModule fragment_module_ = VK_NULL_HANDLE;
    VkShaderModule comfort_vertex_module_ = VK_NULL_HANDLE;
    VkShaderModule comfort_fragment_module_ = VK_NULL_HANDLE;

    VkPipeline project_pipeline_ = VK_NULL_HANDLE;
    VkPipeline finalize_pipeline_ = VK_NULL_HANDLE;
    VkPipeline graphics_pipeline_ = VK_NULL_HANDLE;
    VkPipeline comfort_pipeline_ = VK_NULL_HANDLE;
    // The same two pipelines for options_.target_format's UNORM/sRGB
    // counterpart (blend_alt_format), selected by RenderTarget::format.
    VkFormat alt_format_ = VK_FORMAT_UNDEFINED;
    VkPipeline graphics_pipeline_alt_ = VK_NULL_HANDLE;
    VkPipeline comfort_pipeline_alt_ = VK_NULL_HANDLE;
    float scale_vignette_ = 0.0f;

    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    PassTimings timings_;
    uint32_t last_visible_count_ = 0;
    bool has_readback_ = false;
    uint64_t scene_generation_ = 0;
    double last_commit_ms_ = 0.0;
};

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
