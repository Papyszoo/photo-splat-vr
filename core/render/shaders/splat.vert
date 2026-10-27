#version 450

// One instanced quad per sorted splat. The sort order is shared by both eyes;
// only the eye's camera x offset changes between the two draws.

layout(std140, set = 0, binding = 0) uniform Camera {
    mat4 view;
    vec4 intrinsics;  // fx, fy, cx, cy
    vec4 screen;      // width, height, near, far
    vec4 params;      // scene_scale, half IPD, srgb target, comfort fade
    vec4 backdrop;    // neutral backdrop, linear RGB
    uvec4 counts;
} camera;

layout(std430, set = 0, binding = 6) readonly buffer Instances {
    vec4 instances[];
};

layout(std430, set = 0, binding = 8) readonly buffer Values {
    uint values[];
};

layout(push_constant) uniform PushConstants {
    float eye_offset;  // metres along camera +x
    float cx_shift;    // pixels: this eye's principal-point x offset
} push;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_position;

void main() {
    const uint slot = values[gl_InstanceIndex];
    const vec4 view_pos = instances[slot * 3u + 0u];
    const vec4 axes = instances[slot * 3u + 1u];
    out_color = instances[slot * 3u + 2u];

    // The eye is shifted along camera +x, so the splat is shifted along -x.
    // cx_shift is the eye's own principal point: on a headset the frustum is
    // asymmetric (temporal side wider than nasal), which is a screen shift of
    // the projection centre, not a camera translation.
    const vec3 p = vec3(view_pos.x - push.eye_offset, view_pos.y, view_pos.z);
    const vec2 pixels =
        camera.intrinsics.xy * p.xy / p.z + camera.intrinsics.zw + vec2(push.cx_shift, 0.0);
    const vec2 ndc = pixels * (2.0 / camera.screen.xy) - 1.0;
    const float z_ndc = clamp((p.z - camera.screen.z) / (camera.screen.w - camera.screen.z),
                              0.0, 1.0);

    const int index = gl_VertexIndex;
    const vec2 corner = vec2((index & 1) == 0 ? -1.0 : 1.0, (index & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(ndc + (axes.xy * corner.x + axes.zw * corner.y) * 3.0, z_ndc, 1.0);
    out_position = corner * 3.0;
}
