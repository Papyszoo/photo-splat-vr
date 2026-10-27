#version 450

// One textured quad of the overlay pass (scene label, tooltips, menus). The
// camera block is the one the splat pass uses for the same eye, so a
// world-locked quad lands exactly where the splats put the scene. Scene axes
// are OpenCV (x right, y down, z forward); the splat projection below is
// project.comp / splat.vert's, including the per-eye principal-point shift.

layout(push_constant) uniform Camera {
    mat4 view;        // world -> camera, column-major
    vec4 intrinsics;  // fx, fy, cx, cy in eye pixels
    vec4 screen;      // width, height, unused, unused
    vec4 eye;         // eye offset metres, cx shift pixels
    vec4 encode;      // x = 1: the fragment shaders encode for a UNORM target
} camera;

layout(location = 0) in vec3 in_position;  // world position, metres
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_tint;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;

void main() {
    const vec3 p = (camera.view * vec4(in_position, 1.0)).xyz;
    v_uv = in_uv;
    v_tint = in_tint;

    // Behind the eye: push it outside the clip volume so it is discarded
    // without making the whole triangle degenerate.
    if (p.z <= 1.0e-4) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    // The eye is shifted along camera +x, as in splat.vert; the overlay is a
    // world-space object, so this is the same parallax the splats get.
    const vec3 eye_p = vec3(p.x - camera.eye.x, p.y, p.z);
    const vec2 pixels = camera.intrinsics.xy * eye_p.xy / eye_p.z + camera.intrinsics.zw +
                        vec2(camera.eye.y, 0.0);
    const vec2 ndc = pixels * (2.0 / camera.screen.xy) - 1.0;
    gl_Position = vec4(ndc, 0.5, 1.0);
}
