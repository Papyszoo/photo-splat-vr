#version 450

// C7 comfort boundary final blend. Drawn as one full-target triangle after the
// splat pass, with premultiplied alpha blending (ONE, ONE_MINUS_SRC_ALPHA):
//
//   dst' = backdrop * (1 - fade) + dst * fade
//
// This is a real final blend, so it works no matter how dense the splat
// coverage is. Scaling each splat's alpha instead leaves a dense scene opaque
// because the accumulated coverage saturates. Splats are never dropped.
//
// Two fades multiply:
//  - the radial comfort fade from the head's distance to the capture point
//    (camera.params.w), and
//  - a per-pixel, per-eye capture-frustum fade. The splat pass stores the
//    nearest splat depth per eye; this pass adds that eye's own camera offset
//    to the pixel's view ray, reconstructs the scene point at that depth,
//    projects it through the capture camera and fades where it falls outside
//    the original photo. Using the eye's own centre (not the midpoint) puts
//    the fade edge on the scene surface, so it carries the same disparity as
//    the geometry behind it instead of sitting at a fixed screen x.

layout(std140, set = 0, binding = 0) uniform Camera {
    mat4 view;
    vec4 intrinsics;
    vec4 screen;         // width, height, near, far
    vec4 params;         // scene_scale, half IPD, srgb_target, comfort_fade
    vec4 backdrop;       // neutral backdrop, linear RGB
    uvec4 counts;
    vec4 capture;        // capture fx, fy, cx, cy (x = 0 disables the fade)
    vec4 capture_size;   // width, height, outer margin, inner margin (fractions)
    vec4 camera_position;  // midpoint eye position; w = half IPD in stereo
    vec4 eye_shift;        // per-eye principal-point x offset in pixels
    vec4 aa;               // low-pass eps, compensate flag, near-fade end, vignette
} camera;

// Nearest splat depth (NDC z) written by the splat pass.
layout(set = 0, binding = 11) uniform sampler2D depth_image;

layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), c));
}

// 1 while the pixel's content is inside the capture image, then a smoothstep
// down to 0 across the band that starts `capture_size.w` inside the border and
// ends `capture_size.z` outside it. Both are fractions of the image size.
// The ramp starts INSIDE the photo because the capture's own edge Gaussians
// are weak and washed out: left at full opacity they sit as a speckled strip
// against the backdrop. `eye_offset` is this eye's shift along the camera x
// axis, so the reconstructed hit point is the one this eye actually sees.
float capture_frustum_fade(vec3 ray, float eye_offset) {
    if (camera.capture.x <= 0.0 || camera.capture_size.x <= 0.0 ||
        camera.capture_size.y <= 0.0) {
        return 1.0;
    }

    const float ndc_z = texelFetch(depth_image, ivec2(gl_FragCoord.xy), 0).r;
    // 1.0 is the cleared depth: no splat covers this pixel, so there is no
    // geometry to judge (the pixel is backdrop already).
    if (ndc_z >= 1.0) return 1.0;

    // View-space z of the nearest splat, then the same point in scene space,
    // seen from this eye. The ray has z = 1, so the point at that depth is
    // ray * z; the eye offset is a camera-space x shift (the eyes only differ
    // along camera right). Depth is unaffected by that shift.
    const float view_z = camera.screen.z + ndc_z * (camera.screen.w - camera.screen.z);
    const vec3 scene = transpose(mat3(camera.view)) *
                           (ray * view_z + vec3(eye_offset, 0.0, 0.0)) +
                       camera.camera_position.xyz;

    // The capture camera sits at the scene origin looking down +z with the
    // original photo's intrinsics. Project the content point through it and
    // measure how far outside the image it lands, in image-size units.
    if (!(scene.z > 0.0)) return 0.0;
    const vec2 capture_px = camera.capture.xy * scene.xy / scene.z + camera.capture.zw;
    const vec2 uv = capture_px / camera.capture_size.xy;
    const float outside = max(max(-uv.x, uv.x - 1.0), max(-uv.y, uv.y - 1.0));
    return 1.0 - smoothstep(-camera.capture_size.w, camera.capture_size.z, outside);
}

void main() {
    // The comfort pass covers the whole side-by-side target; recover the
    // pixel's position inside its eye before building the ray, and which eye
    // it belongs to (mono runs as one eye at the midpoint).
    const float eye_width = camera.screen.x;
    const float eye = floor(gl_FragCoord.x / eye_width);
    const float local_x = gl_FragCoord.x - eye_width * eye;
    const vec2 pixel = vec2(local_x, gl_FragCoord.y);
    // This eye's principal point: the shared cx plus the eye's own offset (a
    // headset's asymmetric frustum). Zero everywhere on a symmetric renderer.
    const float eye_cx = eye >= 0.5 ? camera.eye_shift.y : camera.eye_shift.x;
    const vec3 ray =
        vec3((pixel - camera.intrinsics.zw - vec2(eye_cx, 0.0)) / camera.intrinsics.xy, 1.0);
    // camera_position.w is the half IPD for a stereo frame and 0 for mono, so
    // the left eye is at -half IPD and the right at +half IPD along camera x.
    const float eye_offset = (eye >= 0.5 ? 1.0 : -1.0) * camera.camera_position.w;

    const float fade =
        clamp(camera.params.w, 0.0, 1.0) * capture_frustum_fade(ray, eye_offset);
    vec3 backdrop = camera.backdrop.rgb;
    if (camera.params.z < 0.5) {
        // A UNORM target blends encoded values, matching the splat path.
        backdrop = linear_to_srgb(backdrop);
    }
    // A light peripheral dimming while a scale press is active (VR8): the dolly
    // term of the pivot transform is optic flow, so a faint tunnel makes the
    // press easier to read. Radial from the eye's principal point; 0 disables.
    const float vignette = clamp(camera.aa.w, 0.0, 1.0);
    const vec2 half_size = max(camera.screen.xy * 0.5, vec2(1.0));
    const vec2 radial = (pixel - camera.intrinsics.zw - vec2(eye_cx, 0.0)) / half_size;
    const float darken = vignette * smoothstep(0.45, 1.0, length(radial) * 0.70710678);
    // Premultiplied over the frame: backdrop * (1-fade) then dimmed by darken.
    const float reveal = (1.0 - fade) * (1.0 - darken);
    const float alpha = 1.0 - fade * (1.0 - darken);
    out_color = vec4(backdrop * reveal, alpha);
}
