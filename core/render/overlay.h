#pragma once

#if defined(SPLAT_HAVE_VULKAN)

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/status.h"
#include "render/camera.h"
#include "render/overlay_quad.h"
#include "render/vulkan_context.h"

namespace splat {

// The camera state one eye's overlay quads are projected with. It is the same
// view matrix, intrinsics and per-eye shift the splat pass uses, so overlay
// content sits exactly where the splats are.
struct OverlayCamera {
    float view[16] = {};  // world -> camera, column-major (OpenCV axes)
    CameraIntrinsics intrinsics{};  // eye pixels
    uint32_t width = 0;             // one eye
    uint32_t height = 0;
    float eye_offset = 0.0f;  // metres along camera +x
    float cx_shift = 0.0f;    // pixels, this eye's principal point
};

// World-space textured quads for the first piece of the app UI: the scene
// label now, controller tooltips and menus later. It shares the SplatRenderer's
// VulkanContext, command buffer, target and camera uniforms; it creates no
// device and no command buffer of its own.
//
// Drawn after the splat and comfort passes with LOAD_OP_LOAD, alpha blending
// and no depth test, so a label is never hidden by a splat and is not washed
// out by the comfort fade. Destroying an image or the vertex buffer is safe
// between frames: SplatRenderer::render waits for its fence before returning.
class OverlayRenderer {
public:
    ~OverlayRenderer();
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    static Result<std::unique_ptr<OverlayRenderer>> create(VulkanContext& context,
                                                           const std::string& shader_dir,
                                                           VkFormat target_format);

    // R8 mask (255 = quad pixel), tinted by the quad's flat tint: the scene
    // label, HUD and tooltips. Each call allocates one texture slot and returns
    // its index for OverlayQuad::texture.
    Result<uint32_t> add_texture(uint32_t width, uint32_t height, const uint8_t* pixels);
    // Replaces a texture's pixels; same-size updates reuse the image, a size
    // change recreates it. This is the "once per text change" upload path.
    Status update_texture(uint32_t index, uint32_t width, uint32_t height,
                          const uint8_t* pixels);

    // RGBA8, straight alpha, top row first (the UI canvas pixel layout). The
    // texel's RGB is the colour, so a photographic panel keeps every colour;
    // the quad tint only fades it (tint alpha). Uploaded as an SRGB texture so
    // the bytes reach the sRGB target unchanged. Same slots, sampler, draw path
    // and blend state as the R8 masks.
    Result<uint32_t> add_rgba_texture(uint32_t width, uint32_t height,
                                      const uint8_t* rgba_pixels);
    // The panel upload: one staging copy of the whole canvas. Same slot rules
    // as update_texture().
    Status update_rgba_texture(uint32_t index, uint32_t width, uint32_t height,
                               const uint8_t* rgba_pixels);

    // Replaces the quad list (draw order: first quad is behind). Empty clears.
    Status set_quads(std::vector<OverlayQuad> quads);

    bool has_quads() const { return !quads_.empty(); }
    size_t quad_count() const { return quads_.size(); }
    size_t texture_count() const { return textures_.size(); }

    // Records all quads for one eye. The caller has the dynamic-rendering pass
    // bound and the eye's viewport/scissor set. `srgb_target` says whether the
    // bound colour attachment is an sRGB format: an sRGB attachment encodes on
    // store (the shaders output linear); a UNORM one needs the shader to
    // encode, which is what the "gamma-space blending" target is.
    void record(VkCommandBuffer command_buffer, const OverlayCamera& camera,
                bool srgb_target = true);

private:
    OverlayRenderer() = default;
    void destroy();

    struct Texture {
        VulkanImage image;
        VkDescriptorSet set = VK_NULL_HANDLE;
        // VK_FORMAT_R8_UNORM or VK_FORMAT_R8G8B8A8_SRGB: which pipeline and
        // which staging size the slot needs.
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    Status upload_texture(Texture& texture, VkFormat format, uint32_t width, uint32_t height,
                          const uint8_t* pixels);
    Result<uint32_t> allocate_texture(VkFormat format, uint32_t width, uint32_t height,
                                      const uint8_t* pixels);

    VulkanContext* context_ = nullptr;
    VkFormat target_format_ = VK_FORMAT_UNDEFINED;
    VkFormat alt_format_ = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkShaderModule vertex_module_ = VK_NULL_HANDLE;
    VkShaderModule fragment_module_ = VK_NULL_HANDLE;
    VkShaderModule rgba_fragment_module_ = VK_NULL_HANDLE;
    // One pipeline per fragment shader; both share layout, blend state and the
    // per-quad vertex buffer.
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline rgba_pipeline_ = VK_NULL_HANDLE;
    // The same two pipelines for the UNORM/sRGB counterpart of the target
    // format (the "Blending" setting's other mode).
    VkPipeline pipeline_alt_ = VK_NULL_HANDLE;
    VkPipeline rgba_pipeline_alt_ = VK_NULL_HANDLE;

    VulkanBuffer vertices_;
    std::vector<OverlayQuad> quads_;
    std::vector<Texture> textures_;
};

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
