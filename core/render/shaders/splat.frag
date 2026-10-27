#version 450

layout(std140, set = 0, binding = 0) uniform Camera {
    mat4 view;
    vec4 intrinsics;
    vec4 screen;
    vec4 params;    // scene_scale, half IPD, srgb_target, comfort_fade
    vec4 backdrop;  // neutral backdrop, linear RGB
    uvec4 counts;
} camera;

layout(location = 0) in vec4 color;   // premultiply-ready rgba, a = opacity
layout(location = 1) in vec2 position;

layout(location = 0) out vec4 out_color;

// sRGB -> linear, the transfer function ml-sharp applies to the .ply colours
// before alpha blending (ref/ml-sharp/src/sharp/utils/color_space.py). The SH
// DC values in a .ply/.psplat are sRGB-encoded, and 3DGS composites in linear
// light, so the blend has to happen in linear space.
vec3 srgb_to_linear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    const float alpha = clamp(color.a * exp(-0.5 * dot(position, position)), 0.0, 1.0);
    if (alpha < 1.0 / 512.0) discard;
    vec3 rgb = color.rgb;
    if (camera.params.z > 0.5) {
        // An sRGB target decodes on load and re-encodes on store, so the
        // fixed-function blend runs in linear light: feed it linear values.
        rgb = srgb_to_linear(rgb);
    }
    out_color = vec4(rgb * alpha, alpha);
}
