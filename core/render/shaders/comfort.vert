#version 450

// Full-target triangle for the C7 comfort blend. No vertex buffer: the three
// corners cover the viewport with one triangle.

void main() {
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
