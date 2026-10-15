// Draw viewer wireframe (src/native_buffers.cpp): positions only.
#version 450

layout(location = 0) in vec4 position;

layout(set = 1, binding = 0) uniform Transform {
  mat4 clip_from_object;
};

void main() {
  gl_Position = clip_from_object * vec4(position.xyz, 1.0);
}
