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

    // The eye is shifted along camera +x, as in splat.vert; the overlay is a
    // world-space object, so this is the same parallax the splats get.
    const vec3 eye_p = vec3(p.x - camera.eye.x, p.y, p.z);

    // The pinhole projection written in homogeneous clip coordinates, w = the
    // view depth. pixels = fx * x / z + cx' (cx' = cx + the eye's shift) and
    // ndc = pixels * 2 / width - 1 are both divisions by z, so the clip
    // position is the affine map below and the GPU's own divide by w gives
    // exactly that pixel.
    //
    // w must be the depth, not 1: the rasteriser interpolates v_uv with the
    // perspective-correct barycentrics 1/w gives it. With w = 1 the texture
    // coordinates are interpolated linearly in screen space across the quad's
    // two triangles, which is exact only for a quad facing the eye; a window
    // that is world-fixed and seen at an angle (the head turned or moved away
    // from where it opened) then shows its content skewed along the diagonal
    // while its four corners are still right.
    //
    // A corner behind the eye needs no special case: the clip volume's
    // 0 <= z <= w (z = depth - near, so w - z = near > 0) drops the part of the
    // triangle nearer than `near` and keeps the rest, which a vertex pushed to
    // a fixed point outside the volume would have mangled.
    const float near_plane = 0.01;
    const vec2 cxy = camera.intrinsics.zw + vec2(camera.eye.y, 0.0);
    gl_Position = vec4(
        (2.0 / camera.screen.xy) * (camera.intrinsics.xy * eye_p.xy + cxy * eye_p.z) - eye_p.z,
        eye_p.z - near_plane, eye_p.z);
}
