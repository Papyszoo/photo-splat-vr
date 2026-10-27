#version 450

// Overlay quad fragment: a single-channel mask times the quad's straight-alpha
// tint. Light text on a dark plate is two draws of this shader with different
// textures/tints (plate first, text over it).
//
// The tint is linear light. An sRGB attachment re-encodes the linear output on
// store; for the gamma-space (UNORM) target the shader encodes it itself, so
// the panel keeps the same bytes in both blend modes.

layout(push_constant) uniform Camera {
    mat4 view;
    vec4 intrinsics;
    vec4 screen;
    vec4 eye;
    vec4 encode;  // x = 1: this is a UNORM target
} camera;

layout(binding = 0) uniform sampler2D mask_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;

layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), c));
}

void main() {
    const float coverage = texture(mask_texture, v_uv).r * v_tint.a;
    vec3 rgb = v_tint.rgb;
    if (camera.encode.x > 0.5) rgb = linear_to_srgb(rgb);
    out_color = vec4(rgb, coverage);
}
