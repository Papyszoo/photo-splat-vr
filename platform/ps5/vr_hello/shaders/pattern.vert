// One full-screen triangle for the PSVR2 test pattern: no vertex buffer, no
// attributes, three vertices at (-1,-1), (3,-1), (-1,3).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
