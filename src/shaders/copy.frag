// Native render targets (src/native_targets.cpp): Resolve's copy from a render
// target into a texture, and the front buffer's presentation. Each destination
// pixel reads the nearest source texel.
#version 450

layout(set = 2, binding = 0) uniform sampler2D source;

layout(set = 3, binding = 0) uniform Copy {
  // xy: the source texel at the destination origin; zw: source texels per
  // destination pixel.
  vec4 source_rect;
  // xy: the destination origin (the viewport's) in pixels; z: nonzero to swap
  // red and blue; w: scale (Resolve's exponent bias).
  vec4 dest;
};

layout(location = 0) out vec4 color;

void main() {
  const vec2 texel = (gl_FragCoord.xy - dest.xy) * source_rect.zw + source_rect.xy;
  vec4 value = texelFetch(source, ivec2(texel), 0);
  if (dest.z != 0.0) {
    value = value.bgra;
  }
  color = value * dest.w;
}
