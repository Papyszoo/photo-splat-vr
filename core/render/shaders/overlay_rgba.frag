#version 450

// Overlay quad fragment for a straight-alpha RGBA8 texture: a whole panel image
// (the CPU-drawn UiCanvas) rather than overlay.frag's single-channel mask. The
// texel's own RGB is the colour and its alpha the coverage, so a photo
// thumbnail keeps every one of its colours; the quad's tint alpha fades the
// panel and its RGB is unused.
//
// The texture is VK_FORMAT_R8G8B8A8_SRGB, so the sampler hands back linear
// light. An sRGB target re-encodes it; the gamma-space (UNORM) target needs the
// shader to encode, so the panel bytes are the same in both modes.

layout(push_constant) uniform Camera {
    mat4 view;
    vec4 intrinsics;
    vec4 screen;
    vec4 eye;
    vec4 encode;  // x = 1: this is a UNORM target
} camera;

layout(binding = 0) uniform sampler2D panel_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;

layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), c));
}

void main() {
    const vec4 texel = texture(panel_texture, v_uv);
    vec3 rgb = texel.rgb;
    if (camera.encode.x > 0.5) rgb = linear_to_srgb(rgb);
    out_color = vec4(rgb, texel.a * v_tint.a);
}
