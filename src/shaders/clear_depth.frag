// Native render targets (src/native_targets.cpp): the depth/stencil clears of
// D3D Clear and Resolve (the stencil value is the pipeline's reference).
#version 450

layout(set = 3, binding = 0) uniform Clear {
  vec4 clear_color;
  float clear_depth;
};

void main() {
  gl_FragDepth = clear_depth;
}
