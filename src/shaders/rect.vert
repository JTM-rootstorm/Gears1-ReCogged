// Native render targets (src/native_targets.cpp): the whole viewport as a
// 4-vertex triangle strip; the viewport is the rectangle to fill.
#version 450

void main() {
  const vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
