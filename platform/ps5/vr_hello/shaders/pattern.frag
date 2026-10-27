// The PSVR2 test pattern, drawn by the GPU into one side-by-side RGBA8 image.
//
// Left eye: green background, a big orange "L". Right eye: blue background, a
// big cyan "R". Both: a 128-pixel grid, a white centre cross, four white corner
// squares, a coloured top band, and a yellow bar that follows head yaw. The
// picture is a flat 2D pattern, so it is head-locked by construction; the bar
// moving with the head is what shows the pose path is live, and the colours
// and letters are what show the eye order and the per-eye half.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Params {
    uint tw;       // side-by-side width in pixels (2 * eye_w)
    uint eye_w;    // width of one eye in pixels
    uint eye_h;    // height in pixels
    float marker;  // 0..1 head yaw, left to right
} pc;

bool in_rect(vec2 p, vec2 lo, vec2 hi) {
    return p.x >= lo.x && p.x < hi.x && p.y >= lo.y && p.y < hi.y;
}

void main() {
    vec2 pixel = floor(gl_FragCoord.xy);
    uint x = uint(pixel.x);
    uint y = uint(pixel.y);
    if (x >= pc.tw || y >= pc.eye_h) {
        out_colour = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    bool right = x >= pc.eye_w;
    vec2 p = vec2(float(x) - (right ? float(pc.eye_w) : 0.0), pixel.y);
    float ew = float(pc.eye_w);
    float eh = float(pc.eye_h);
    vec2 q = vec2(p.x / ew, p.y / eh);

    // Eye background: green on the left, blue on the right.
    vec3 colour = right ? vec3(0.02, 0.04, 0.24) : vec3(0.02, 0.18, 0.06);

    // 128-pixel grid.
    if (mod(p.x, 128.0) < 2.0 || mod(p.y, 128.0) < 2.0)
        colour = right ? vec3(0.10, 0.18, 0.55) : vec3(0.0, 0.36, 0.12);

    // Top band: the eye's identity colour.
    if (p.y < 8.0)
        colour = right ? vec3(0.10, 0.55, 1.0) : vec3(0.20, 1.0, 0.30);

    // Thin border: it must survive the lens distortion on every edge.
    if (p.x < 3.0 || p.x >= ew - 3.0 || p.y < 3.0 || p.y >= eh - 3.0)
        colour = vec3(0.55);

    // Corner squares: orientation and overscan check, one in each corner.
    bool corner = in_rect(p, vec2(32.0, 32.0), vec2(72.0, 72.0)) ||
                  in_rect(p, vec2(ew - 72.0, 32.0), vec2(ew - 32.0, 72.0)) ||
                  in_rect(p, vec2(32.0, eh - 72.0), vec2(72.0, eh - 32.0)) ||
                  in_rect(p, vec2(ew - 72.0, eh - 72.0), vec2(ew - 32.0, eh - 32.0));
    if (corner)
        colour = vec3(0.92);

    // Big blocky letter: L on the left eye, R on the right eye.
    vec3 glyph = right ? vec3(0.10, 0.85, 1.0) : vec3(1.0, 0.55, 0.05);
    bool on = false;
    if (!right) {
        on = in_rect(q, vec2(0.335, 0.315), vec2(0.400, 0.685)) ||   // stem
             in_rect(q, vec2(0.335, 0.620), vec2(0.600, 0.685));     // foot
    } else {
        on = in_rect(q, vec2(0.400, 0.315), vec2(0.460, 0.685)) ||   // stem
             in_rect(q, vec2(0.460, 0.315), vec2(0.615, 0.375)) ||   // top bar
             in_rect(q, vec2(0.560, 0.315), vec2(0.615, 0.500)) ||   // upper bowl
             in_rect(q, vec2(0.460, 0.440), vec2(0.615, 0.500)) ||   // waist
             in_rect(q, vec2(0.470, 0.500), vec2(0.560, 0.640)) ||   // leg
             in_rect(q, vec2(0.520, 0.615), vec2(0.660, 0.685));     // foot
    }
    if (on)
        colour = glyph;

    // Centre cross: the eye's principal point.
    bool cross =
        in_rect(p, vec2(ew * 0.5 - 90.0, eh * 0.5 - 3.0), vec2(ew * 0.5 + 90.0, eh * 0.5 + 3.0)) ||
        in_rect(p, vec2(ew * 0.5 - 3.0, eh * 0.5 - 90.0), vec2(ew * 0.5 + 3.0, eh * 0.5 + 90.0));
    if (cross)
        colour = vec3(1.0);

    // Yaw marker: a bar plus a knob at the head's yaw, so a dead pose pipeline
    // (bar stuck at the centre) is as visible as a live one.
    float mx = clamp(pc.marker, 0.0, 1.0) * (ew - 12.0);
    bool bar = p.x >= mx && p.x < mx + 12.0;
    bool knob = in_rect(p, vec2(mx, eh * 0.16), vec2(mx + 72.0, eh * 0.16 + 72.0));
    if (bar || knob)
        colour = vec3(1.0, 0.86, 0.0);

    out_colour = vec4(colour, 1.0);
}
