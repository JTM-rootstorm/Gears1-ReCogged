// Native render targets (src/native_targets.cpp): D3D Clear and the clears of
// Resolve, for a rectangle of a colour or depth/stencil target.
#version 450

layout(set = 3, binding = 0) uniform Clear {
  vec4 clear_color;
  float clear_depth;
};

layout(location = 0) out vec4 color;

void main() {
  color = clear_color;
  gl_FragDepth = clear_depth;
}
