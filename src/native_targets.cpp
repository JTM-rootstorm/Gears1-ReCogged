// Native renderer, phase 2: render targets as SDL_GPU textures, with D3D Clear
// and Resolve done on them.
//
// On the console, render targets live in EDRAM and only reach memory through
// Resolve, which copies a rectangle of one into a texture (and can clear it).
// Here each guest surface is a host texture of its own, keyed by its EDRAM
// base, format and size, and each Resolve destination is a host texture keyed
// by the destination texture's address, format and size; textures the game
// samples later are looked up there before being decoded from guest memory
// (src/native_textures.cpp). Targets are drawn at 1x: SDL_GPU can neither
// resolve depth nor read single samples (NATIVE_RENDERER.md), and the tiling
// patch (gears1_black_shading_fix) already has the game render without MSAA.
//
// The hooks (pre hooks of D3D Clear and Resolve, tools/hook_table.py) record
// the operations of each guest frame from the bound surfaces; the main thread
// replays every recorded frame in order and presents the front buffer that
// D3D Swap names.
//
// Guest surface object (D3DSurface): +0x18 RB_SURFACE_INFO (pitch, MSAA),
// +0x1C RB_COLOR_INFO or RB_DEPTH_INFO (EDRAM base, format, exponent bias),
// +0x24 size (width - 1 in bits 18-31, height - 1 in bits 3-17), as D3D Clear
// reads it for its default rectangle.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>

#include "clear.frag.h"
#include "clear_depth.frag.h"
#include "copy.frag.h"
#include "native_renderer.h"
#include "rect.vert.h"

REXCVAR_DEFINE_STRING(gears1_native_dump_dir, "", "Gears1",
                      "Directory the native renderer writes its front buffer to as a BMP "
                      "every 300 replayed frames (front_<frame>.bmp); empty: off");

namespace {

using rex::graphics::xenos::ColorRenderTargetFormat;
using rex::graphics::xenos::DepthRenderTargetFormat;
using rex::graphics::xenos::TextureFormat;
using rex::memory::load_and_swap;

// Device fields (NATIVE_RENDERER.md "D3D API map").
constexpr uint32_t kDeviceRenderTargets = 0x2F88;  // RT0-3
constexpr uint32_t kDeviceDepthStencil = 0x2F98;
constexpr uint32_t kDeviceViewport = 0x3058;     // X, Y, Width, Height
constexpr uint32_t kDeviceScissorRect = 0x3070;  // left, top, right, bottom
constexpr uint32_t kDeviceScissorEnable = 0x2D40;
constexpr uint32_t kTextureFetchConstantOffset = 0x1C;

// D3D Clear flags.
constexpr uint32_t kClearTargets = 0xF, kClearDepth = 0x10, kClearStencil = 0x20;
// D3D Resolve flags (the RB_COPY_CONTROL bits D3D passes through, plus its own).
constexpr uint32_t kResolveSourceMask = 7, kResolveDepthSource = 4;
constexpr uint32_t kResolveClearColor = 0x100, kResolveClearDepthStencil = 0x200;
constexpr uint32_t kResolveNoCopy = 0x2000;  // copy command "null": clears only
constexpr uint32_t kResolveExpBiasShift = 26;

struct TargetKey {
  uint32_t depth;   // 0 colour, 1 depth/stencil
  uint32_t base;    // EDRAM tiles
  uint32_t format;  // ColorRenderTargetFormat or DepthRenderTargetFormat
  uint32_t width, height;
  auto operator<=>(const TargetKey&) const = default;
};

// A Resolve destination: its texture's address (as in the fetch constant),
// format and base level size.
struct TextureKey {
  uint32_t address;
  uint32_t format;  // TextureFormat
  uint32_t width, height;
  auto operator<=>(const TextureKey&) const = default;
};

struct Rect {
  int32_t left, top, right, bottom;
  bool Empty() const { return left >= right || top >= bottom; }
};

struct ClearOp {
  TargetKey target;
  Rect rect;
  float color[4];
  bool depth, stencil;  // which parts of a depth/stencil target
  float z;
  uint32_t stencil_value;
};

struct CopyOp {
  TargetKey source;
  Rect rect;
  TextureKey dest;
  int32_t dest_x, dest_y;
  bool swap;  // red and blue, for a destination fetched with them swapped
  float scale;
};

using Op = std::variant<ClearOp, CopyOp>;

struct Frame {
  std::vector<Op> ops;
  bool has_front = false;
  TextureKey front;
  bool front_swap = false;
};

struct Stats {
  uint32_t clears = 0, resolves = 0;
  std::map<std::string, uint32_t> problems;  // reason -> count
};

// Guest threads, under g_mutex.
std::mutex g_mutex;
Frame g_frame;
Stats g_stats;
std::set<TextureKey> g_resolve_dests;
std::set<TargetKey> g_logged_targets;
// Published at the end of each guest frame.
std::vector<Frame> g_pending;
Stats g_last_stats;

// Main thread only.
struct HostTexture {
  SDL_GPUTexture* texture = nullptr;
  SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
};
std::map<TargetKey, HostTexture> g_targets;
std::map<TextureKey, HostTexture> g_resolved;
SDL_GPUShader* g_rect_shader = nullptr;
SDL_GPUShader* g_clear_shader = nullptr;
SDL_GPUShader* g_clear_depth_shader = nullptr;
SDL_GPUShader* g_copy_shader = nullptr;
SDL_GPUSampler* g_sampler = nullptr;
std::map<uint32_t, SDL_GPUGraphicsPipeline*> g_pipelines;  // PipelineKey()
Frame g_shown;  // the last frame replayed, for the browser
uint64_t g_replayed_frames = 0;
std::vector<std::string> g_host_problems;
// A front buffer download in flight (gears1_native_dump_dir).
SDL_GPUTransferBuffer* g_dump = nullptr;
uint32_t g_dump_width = 0, g_dump_height = 0;
bool g_dump_swap = false;

uint32_t Load32(uint8_t* base, uint32_t address) {
  return load_and_swap<uint32_t>(rex::memory::GuestPtr(base, address));
}

bool ReadSurface(uint8_t* base, uint32_t object, bool depth, TargetKey& key) {
  if (!object) {
    return false;
  }
  const uint32_t info = Load32(base, object + 0x1C);
  const uint32_t size = Load32(base, object + 0x24);
  key.depth = depth;
  key.base = info & 0xFFF;
  key.format = depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
  key.width = (size >> 18) + 1;
  key.height = ((size >> 3) & 0x7FFF) + 1;
  if (g_logged_targets.insert(key).second) {
    REXLOG_INFO("Native targets: {} surface {:08X}: EDRAM base {}, format {}, {}x{}, info {:08X}",
                depth ? "depth" : "colour", object, key.base, key.format, key.width, key.height,
                Load32(base, object + 0x18));
  }
  return true;
}

Rect ReadRect(uint8_t* base, uint32_t address) {
  return {int32_t(Load32(base, address)), int32_t(Load32(base, address + 4)),
          int32_t(Load32(base, address + 8)), int32_t(Load32(base, address + 12))};
}

Rect Intersect(Rect a, const Rect& b) {
  a.left = std::max(a.left, b.left);
  a.top = std::max(a.top, b.top);
  a.right = std::min(a.right, b.right);
  a.bottom = std::min(a.bottom, b.bottom);
  return a;
}

Rect Whole(const TargetKey& key) {
  return {0, 0, int32_t(key.width), int32_t(key.height)};
}

void ReadColor(uint8_t* base, uint32_t address, float color[4]) {
  for (int i = 0; i < 4; ++i) {
    color[i] = address ? load_and_swap<float>(rex::memory::GuestPtr(base, address + 4 * i)) : 0.0f;
  }
}

TextureKey ReadTextureKey(uint8_t* base, uint32_t texture, uint32_t& dword_3) {
  const uint32_t dword_1 = Load32(base, texture + kTextureFetchConstantOffset + 4);
  const uint32_t dword_2 = Load32(base, texture + kTextureFetchConstantOffset + 8);
  dword_3 = Load32(base, texture + kTextureFetchConstantOffset + 12);
  return {dword_1 & 0xFFFFF000, dword_1 & 0x3F, (dword_2 & 0x1FFF) + 1,
          ((dword_2 >> 13) & 0x1FFF) + 1};
}

// Main thread: host formats.

SDL_GPUTextureFormat ColorFormat(ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case ColorRenderTargetFormat::k_2_10_10_10:
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case ColorRenderTargetFormat::k_16_16_16_16:
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case ColorRenderTargetFormat::k_16_16:
    case ColorRenderTargetFormat::k_16_16_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case ColorRenderTargetFormat::k_32_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
    default:
      return SDL_GPU_TEXTUREFORMAT_INVALID;
  }
}

SDL_GPUTextureFormat DepthFormat(SDL_GPUDevice* device, DepthRenderTargetFormat format) {
  // D24S8 keeps its exact unorm depth where the GPU has it (Apple GPUs don't).
  if (format == DepthRenderTargetFormat::kD24S8 &&
      SDL_GPUTextureSupportsFormat(
          device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
    return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
  }
  return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
}

// The host format of a Resolve destination: colour as the render target wrote
// it, depth as one float channel.
SDL_GPUTextureFormat ResolveFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::k_8:
      return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case TextureFormat::k_8_8:
      return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    case TextureFormat::k_8_8_8_8:
    case TextureFormat::k_8_8_8_8_A:
      return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case TextureFormat::k_2_10_10_10:
      return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    case TextureFormat::k_16_16:
    case TextureFormat::k_16_16_EDRAM:
    case TextureFormat::k_16_16_EXPAND:
    case TextureFormat::k_16_16_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case TextureFormat::k_16_16_16_16:
    case TextureFormat::k_16_16_16_16_EDRAM:
    case TextureFormat::k_16_16_16_16_EXPAND:
    case TextureFormat::k_16_16_16_16_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::k_24_8:
    case TextureFormat::k_24_8_FLOAT:
    case TextureFormat::k_32_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case TextureFormat::k_32_32_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
    default:
      return SDL_GPU_TEXTUREFORMAT_INVALID;
  }
}

void HostProblem(std::string problem) {
  if (std::find(g_host_problems.begin(), g_host_problems.end(), problem) == g_host_problems.end()) {
    REXLOG_WARN("Native targets: {}", problem);
    g_host_problems.push_back(std::move(problem));
  }
}

bool CreateShaders(SDL_GPUDevice* device) {
  if (g_rect_shader) {
    return true;
  }
  SDL_GPUShaderCreateInfo info = {};
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.entrypoint = "main";
  info.code = reinterpret_cast<const Uint8*>(rect_vert);
  info.code_size = sizeof(rect_vert);
  info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  g_rect_shader = SDL_CreateGPUShader(device, &info);
  info.code = reinterpret_cast<const Uint8*>(clear_frag);
  info.code_size = sizeof(clear_frag);
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_uniform_buffers = 1;
  g_clear_shader = SDL_CreateGPUShader(device, &info);
  info.code = reinterpret_cast<const Uint8*>(clear_depth_frag);
  info.code_size = sizeof(clear_depth_frag);
  g_clear_depth_shader = SDL_CreateGPUShader(device, &info);
  info.code = reinterpret_cast<const Uint8*>(copy_frag);
  info.code_size = sizeof(copy_frag);
  info.num_samplers = 1;
  g_copy_shader = SDL_CreateGPUShader(device, &info);
  SDL_GPUSamplerCreateInfo sampler = {};
  sampler.min_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  g_sampler = SDL_CreateGPUSampler(device, &sampler);
  if (!g_rect_shader || !g_clear_shader || !g_clear_depth_shader || !g_copy_shader || !g_sampler) {
    REXLOG_ERROR("Native targets: shaders not created: {}", SDL_GetError());
    return false;
  }
  return true;
}

enum class PipelineKind : uint32_t { kClearColor, kClearDepthStencil, kCopy };

// Clear depth/stencil: bit 0 writes depth, bit 1 stencil.
SDL_GPUGraphicsPipeline* Pipeline(SDL_GPUDevice* device, PipelineKind kind,
                                  SDL_GPUTextureFormat format, uint32_t depth_stencil = 0) {
  const uint32_t key = uint32_t(kind) << 24 | depth_stencil << 16 | uint32_t(format);
  auto [it, inserted] = g_pipelines.try_emplace(key, nullptr);
  if (!inserted) {
    return it->second;
  }
  SDL_GPUColorTargetDescription target = {};
  target.format = format;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = g_rect_shader;
  info.fragment_shader = kind == PipelineKind::kCopy                ? g_copy_shader
                         : kind == PipelineKind::kClearDepthStencil ? g_clear_depth_shader
                                                                    : g_clear_shader;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  if (kind == PipelineKind::kClearDepthStencil) {
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = format;
    SDL_GPUDepthStencilState& state = info.depth_stencil_state;
    state.enable_depth_test = true;  // Vulkan writes depth only with the test on
    state.enable_depth_write = depth_stencil & 1;
    state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    state.enable_stencil_test = (depth_stencil & 2) != 0;
    state.front_stencil_state = {SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_REPLACE,
                                 SDL_GPU_STENCILOP_REPLACE, SDL_GPU_COMPAREOP_ALWAYS};
    state.back_stencil_state = state.front_stencil_state;
    state.compare_mask = 0xFF;
    state.write_mask = 0xFF;
  } else {
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
  }
  it->second = SDL_CreateGPUGraphicsPipeline(device, &info);
  if (!it->second) {
    HostProblem("pipeline for format " + std::to_string(format) + " failed: " + SDL_GetError());
  }
  return it->second;
}

// A new texture starts cleared: colour to zero, depth to 1, stencil to 0.
SDL_GPUTexture* CreateTexture(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer,
                              SDL_GPUTextureFormat format, bool depth, uint32_t width,
                              uint32_t height) {
  SDL_GPUTextureCreateInfo info = {};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
                                                     : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
  if (!texture) {
    HostProblem(std::to_string(width) + "x" + std::to_string(height) + " format " +
                std::to_string(format) + " not created: " + SDL_GetError());
    return nullptr;
  }
  SDL_GPURenderPass* pass;
  if (depth) {
    SDL_GPUDepthStencilTargetInfo target = {};
    target.texture = texture;
    target.clear_depth = 1.0f;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    target.stencil_store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(command_buffer, nullptr, 0, &target);
  } else {
    SDL_GPUColorTargetInfo target = {};
    target.texture = texture;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(command_buffer, &target, 1, nullptr);
  }
  SDL_EndGPURenderPass(pass);
  return texture;
}

HostTexture* Target(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer,
                    const TargetKey& key) {
  auto [it, inserted] = g_targets.try_emplace(key);
  if (inserted) {
    it->second.format = key.depth ? DepthFormat(device, DepthRenderTargetFormat(key.format))
                                  : ColorFormat(ColorRenderTargetFormat(key.format));
    if (it->second.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
      HostProblem("render target format " + std::to_string(key.format));
    } else {
      it->second.texture = CreateTexture(device, command_buffer, it->second.format, key.depth,
                                         key.width, key.height);
    }
  }
  return it->second.texture ? &it->second : nullptr;
}

HostTexture* Resolved(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer,
                      const TextureKey& key) {
  auto [it, inserted] = g_resolved.try_emplace(key);
  if (inserted) {
    it->second.format = ResolveFormat(TextureFormat(key.format));
    if (it->second.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
      HostProblem("resolve destination format " + std::to_string(key.format));
    } else {
      it->second.texture =
          CreateTexture(device, command_buffer, it->second.format, false, key.width, key.height);
    }
  }
  return it->second.texture ? &it->second : nullptr;
}

void SetViewport(SDL_GPURenderPass* pass, const Rect& rect) {
  const SDL_GPUViewport viewport = {float(rect.left),
                                    float(rect.top),
                                    float(rect.right - rect.left),
                                    float(rect.bottom - rect.top),
                                    0.0f,
                                    1.0f};
  SDL_SetGPUViewport(pass, &viewport);
}

struct ClearUniforms {
  float color[4];
  float depth;
  float padding[3];
};

struct CopyUniforms {
  float source_rect[4];
  float dest[4];
};

void Clear(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer, const ClearOp& op) {
  HostTexture* target = Target(device, command_buffer, op.target);
  const Rect rect = Intersect(op.rect, Whole(op.target));
  if (!target || rect.Empty()) {
    return;
  }
  const uint32_t depth_stencil = uint32_t(op.depth) | uint32_t(op.stencil) << 1;
  SDL_GPUGraphicsPipeline* pipeline = Pipeline(
      device, op.target.depth ? PipelineKind::kClearDepthStencil : PipelineKind::kClearColor,
      target->format, depth_stencil);
  if (!pipeline) {
    return;
  }
  SDL_GPURenderPass* pass;
  if (op.target.depth) {
    SDL_GPUDepthStencilTargetInfo info = {};
    info.texture = target->texture;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    info.stencil_load_op = SDL_GPU_LOADOP_LOAD;
    info.stencil_store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(command_buffer, nullptr, 0, &info);
    SDL_SetGPUStencilReference(pass, uint8_t(op.stencil_value));
  } else {
    SDL_GPUColorTargetInfo info = {};
    info.texture = target->texture;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(command_buffer, &info, 1, nullptr);
  }
  SDL_BindGPUGraphicsPipeline(pass, pipeline);
  SetViewport(pass, rect);
  ClearUniforms uniforms = {{op.color[0], op.color[1], op.color[2], op.color[3]}, op.z, {}};
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
  SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
}

// Draws source (a rectangle of it, scaled by source texels per destination
// pixel) into destination's rect.
void Copy(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer, SDL_GPUTexture* source,
          SDL_GPUTexture* destination, SDL_GPUTextureFormat format, const Rect& rect,
          const CopyUniforms& uniforms) {
  SDL_GPUGraphicsPipeline* pipeline = Pipeline(device, PipelineKind::kCopy, format);
  if (!pipeline) {
    return;
  }
  SDL_GPUColorTargetInfo info = {};
  info.texture = destination;
  info.load_op = SDL_GPU_LOADOP_LOAD;
  info.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer, &info, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, pipeline);
  SetViewport(pass, rect);
  const SDL_GPUTextureSamplerBinding binding = {source, g_sampler};
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
  SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
}

void Resolve(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer, const CopyOp& op) {
  HostTexture* source = Target(device, command_buffer, op.source);
  HostTexture* dest = Resolved(device, command_buffer, op.dest);
  if (!source || !dest) {
    return;
  }
  // The source rectangle, moved to the destination point and clipped to both.
  Rect rect = Intersect(op.rect, Whole(op.source));
  const int32_t dx = op.dest_x - op.rect.left, dy = op.dest_y - op.rect.top;
  Rect dest_rect = {rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy};
  dest_rect = Intersect(dest_rect, {0, 0, int32_t(op.dest.width), int32_t(op.dest.height)});
  if (dest_rect.Empty()) {
    return;
  }
  const CopyUniforms uniforms = {
      {float(dest_rect.left - dx), float(dest_rect.top - dy), 1.0f, 1.0f},
      {float(dest_rect.left), float(dest_rect.top), op.swap ? 1.0f : 0.0f, op.scale}};
  Copy(device, command_buffer, source->texture, dest->texture, dest->format, dest_rect, uniforms);
}

void DrawThumbnail(SDL_GPUTexture* texture, uint32_t width, uint32_t height, const char* label) {
  const float scale = 160.0f / float(std::max(width, height));
  ImGui::BeginGroup();
  ImGui::Image(ImTextureRef(ImTextureID(texture)), ImVec2(width * scale, height * scale));
  ImGui::TextUnformatted(label);
  ImGui::EndGroup();
}

}  // namespace

// Pre hook of D3D Clear (8222CC48): f1 = Z, r3 = device, r4 = flags, r5 = one
// rectangle or 0 (the first bound surface's size), r6 = the colour (four
// floats) or 0, r7 = stencil. D3D clips the rectangle to the viewport and,
// when enabled, the scissor rectangle.
void Gears1NativeTargetsOnClear(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  const uint32_t device = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_stats.clears;
  TargetKey targets[4], depth;
  bool bound[4];
  for (uint32_t i = 0; i < 4; ++i) {
    bound[i] =
        ReadSurface(base, Load32(base, device + kDeviceRenderTargets + 4 * i), false, targets[i]);
  }
  const bool has_depth = ReadSurface(base, Load32(base, device + kDeviceDepthStencil), true, depth);
  if (!has_depth) {
    flags &= ~(kClearDepth | kClearStencil);
  }
  Rect rect;
  if (ctx.r5.u32) {
    rect = ReadRect(base, ctx.r5.u32);
  } else if (bound[0] || has_depth) {
    rect = Whole(bound[0] ? targets[0] : depth);
  } else {
    ++g_stats.problems["clear with no surface"];
    return;
  }
  const int32_t x = int32_t(Load32(base, device + kDeviceViewport));
  const int32_t y = int32_t(Load32(base, device + kDeviceViewport + 4));
  rect = Intersect(rect, {x, y, x + int32_t(Load32(base, device + kDeviceViewport + 8)),
                          y + int32_t(Load32(base, device + kDeviceViewport + 12))});
  if (Load32(base, device + kDeviceScissorEnable)) {
    rect = Intersect(rect, ReadRect(base, device + kDeviceScissorRect));
  }
  ClearOp op = {};
  op.rect = rect;
  ReadColor(base, ctx.r6.u32, op.color);
  for (uint32_t i = 0; i < 4; ++i) {
    if ((flags & kClearTargets & (1u << i)) && bound[i]) {
      op.target = targets[i];
      g_frame.ops.push_back(op);
    }
  }
  if (flags & (kClearDepth | kClearStencil)) {
    op.target = depth;
    op.depth = flags & kClearDepth;
    op.stencil = flags & kClearStencil;
    op.z = float(ctx.f1.f64);
    op.stencil_value = ctx.r7.u32 & 0xFF;
    g_frame.ops.push_back(op);
  }
}

// Pre hook of D3D Resolve (82235528): f1 = clear Z, r3 = device, r4 = flags,
// r5 = source rectangle or 0 (the destination's size), r6 = destination
// texture, r7 = destination point or 0, r8 = level, r9 = slice, r10 = clear
// colour (four floats) or 0, clear stencil in the tenth argument slot. The
// right and bottom edges are rounded up to 8 pixels, as D3D does.
void Gears1NativeTargetsOnResolve(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  const uint32_t device = ctx.r3.u32, flags = ctx.r4.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_stats.resolves;
  const uint32_t source = flags & kResolveSourceMask;
  const bool from_depth = source == kResolveDepthSource;
  TargetKey key;
  const uint32_t object =
      Load32(base, device + (from_depth ? kDeviceDepthStencil : kDeviceRenderTargets + 4 * source));
  if (source > kResolveDepthSource || !ReadSurface(base, object, from_depth, key)) {
    ++g_stats.problems["resolve from no surface"];
    return;
  }
  if (!ctx.r6.u32) {
    ++g_stats.problems["resolve to no texture"];
    return;
  }
  uint32_t dword_3;
  const TextureKey dest = ReadTextureKey(base, ctx.r6.u32, dword_3);
  Rect rect = ctx.r5.u32 ? ReadRect(base, ctx.r5.u32)
                         : Rect{0, 0, int32_t(dest.width), int32_t(dest.height)};
  rect.right = (rect.right + 7) & ~7;
  rect.bottom = (rect.bottom + 7) & ~7;
  if (!(flags & kResolveNoCopy)) {
    CopyOp op;
    op.source = key;
    op.rect = rect;
    op.dest = dest;
    op.dest_x = ctx.r7.u32 ? int32_t(Load32(base, ctx.r7.u32)) : 0;
    op.dest_y = ctx.r7.u32 ? int32_t(Load32(base, ctx.r7.u32 + 4)) : 0;
    // Colour only: D3D sets the copy's red/blue swap when the destination's
    // swizzle doesn't take X from the first channel, and passes the exponent
    // bias (6-bit signed) from the flags.
    op.swap = !from_depth && (dword_3 & 0xE);
    op.scale = from_depth ? 1.0f : std::ldexp(1.0f, int32_t(flags) >> kResolveExpBiasShift);
    if (ctx.r9.u32) {
      ++g_stats.problems["resolve to a slice"];
    }
    g_frame.ops.push_back(op);
    g_resolve_dests.insert(dest);
  }
  ClearOp clear = {};
  clear.rect = rect;
  if ((flags & kResolveClearColor) && !from_depth) {
    clear.target = key;
    ReadColor(base, ctx.r10.u32, clear.color);
    g_frame.ops.push_back(clear);
  }
  TargetKey depth;
  if ((flags & kResolveClearDepthStencil) &&
      ReadSurface(base, Load32(base, device + kDeviceDepthStencil), true, depth)) {
    clear.target = depth;
    clear.depth = clear.stencil = true;
    clear.z = float(ctx.f1.f64);
    clear.stencil_value = Load32(base, ctx.r1.u32 + 0x5C) & 0xFF;
    g_frame.ops.push_back(clear);
  }
}

bool NativeTargetsIsResolved(uint32_t address, uint32_t format, uint32_t width, uint32_t height) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_resolve_dests.count({address, format, width, height}) != 0;
}

void NativeTargetsEndFrame(uint32_t front_buffer, uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (front_buffer) {
    uint32_t dword_3;
    g_frame.front = ReadTextureKey(base, front_buffer, dword_3);
    g_frame.front_swap = (dword_3 & 0xE) != 0;
    g_frame.has_front = true;
  }
  g_pending.push_back(std::move(g_frame));
  g_frame = {};
  g_last_stats = std::move(g_stats);
  g_stats = {};
}

void NativeTargetsExecute(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer) {
  std::vector<Frame> frames;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    frames.swap(g_pending);
  }
  if (frames.empty() || !CreateShaders(device)) {
    return;
  }
  for (const Frame& frame : frames) {
    ++g_replayed_frames;
    for (const Op& op : frame.ops) {
      if (const ClearOp* clear = std::get_if<ClearOp>(&op)) {
        Clear(device, command_buffer, *clear);
      } else {
        Resolve(device, command_buffer, std::get<CopyOp>(op));
      }
    }
  }
  g_shown = std::move(frames.back());
  if (g_replayed_frames % 600 < frames.size()) {
    REXLOG_INFO("Native targets: frame {}: {} operations, {} targets, {} resolved textures",
                g_replayed_frames, g_shown.ops.size(), g_targets.size(), g_resolved.size());
  }
}

bool NativeTargetsPresent(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer,
                          SDL_GPUTexture* swapchain, SDL_GPUTextureFormat format, uint32_t width,
                          uint32_t height) {
  if (!g_shown.has_front || !g_rect_shader) {
    return false;
  }
  auto it = g_resolved.find(g_shown.front);
  if (it == g_resolved.end() || !it->second.texture) {
    return false;
  }
  const TextureKey& front = it->first;
  const CopyUniforms uniforms = {
      {0.0f, 0.0f, float(front.width) / float(width), float(front.height) / float(height)},
      {0.0f, 0.0f, g_shown.front_swap ? 1.0f : 0.0f, 1.0f}};
  Copy(device, command_buffer, it->second.texture, swapchain, format,
       {0, 0, int32_t(width), int32_t(height)}, uniforms);
  return true;
}

bool NativeTargetsQueueDump(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer) {
  static uint64_t dumped_frame = 0;
  if (REXCVAR_GET(gears1_native_dump_dir).empty() || !g_shown.has_front ||
      g_replayed_frames / 300 == dumped_frame / 300) {
    return false;
  }
  auto it = g_resolved.find(g_shown.front);
  if (it == g_resolved.end() || it->second.format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
    return false;
  }
  dumped_frame = g_replayed_frames;
  g_dump_width = it->first.width;
  g_dump_height = it->first.height;
  g_dump_swap = g_shown.front_swap;
  SDL_GPUTransferBufferCreateInfo info = {};
  info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  info.size = g_dump_width * g_dump_height * 4;
  g_dump = SDL_CreateGPUTransferBuffer(device, &info);
  if (!g_dump) {
    return false;
  }
  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command_buffer);
  SDL_GPUTextureRegion source = {};
  source.texture = it->second.texture;
  source.w = g_dump_width;
  source.h = g_dump_height;
  source.d = 1;
  const SDL_GPUTextureTransferInfo destination = {g_dump, 0, g_dump_width, g_dump_height};
  SDL_DownloadFromGPUTexture(pass, &source, &destination);
  SDL_EndGPUCopyPass(pass);
  return true;
}

void NativeTargetsFinishDump(SDL_GPUDevice* device) {
  if (!g_dump) {
    return;
  }
  void* pixels = SDL_MapGPUTransferBuffer(device, g_dump, false);
  // Bytes in memory order: R, G, B, A, or B, G, R, A when the copy swapped them.
  SDL_Surface* surface = SDL_CreateSurfaceFrom(
      int(g_dump_width), int(g_dump_height),
      g_dump_swap ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, pixels, int(g_dump_width * 4));
  const std::string path =
      REXCVAR_GET(gears1_native_dump_dir) + "/front_" + std::to_string(g_replayed_frames) + ".bmp";
  if (!surface || !SDL_SaveBMP(surface, path.c_str())) {
    REXLOG_WARN("Native targets: {} not written: {}", path, SDL_GetError());
  } else {
    REXLOG_INFO("Native targets: wrote {}", path);
  }
  SDL_DestroySurface(surface);
  SDL_UnmapGPUTransferBuffer(device, g_dump);
  SDL_ReleaseGPUTransferBuffer(device, g_dump);
  g_dump = nullptr;
}

void NativeTargetsDrawBrowser() {
  ImGui::SetNextWindowPos(ImVec2(10.0f, 690.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(820.0f, 420.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Render targets");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ImGui::Text("Last guest frame: %u clears, %u resolves", g_last_stats.clears,
                g_last_stats.resolves);
    for (const auto& [problem, count] : g_last_stats.problems) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s: %u", problem.c_str(), count);
    }
  }
  for (const std::string& problem : g_host_problems) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", problem.c_str());
  }
  if (g_shown.has_front) {
    ImGui::Text("Front buffer %08X, %ux%u, format %u", g_shown.front.address, g_shown.front.width,
                g_shown.front.height, g_shown.front.format);
  }
  if (ImGui::CollapsingHeader("Targets", ImGuiTreeNodeFlags_DefaultOpen)) {
    int column = 0;
    for (const auto& [key, host] : g_targets) {
      if (!host.texture) {
        continue;
      }
      if (column++ % 4) {
        ImGui::SameLine();
      }
      char label[64];
      std::snprintf(label, sizeof(label), "%s %u f%u %ux%u", key.depth ? "D" : "C", key.base,
                    key.format, key.width, key.height);
      DrawThumbnail(host.texture, key.width, key.height, label);
    }
  }
  if (ImGui::CollapsingHeader("Resolved textures", ImGuiTreeNodeFlags_DefaultOpen)) {
    int column = 0;
    for (const auto& [key, host] : g_resolved) {
      if (!host.texture) {
        continue;
      }
      if (column++ % 4) {
        ImGui::SameLine();
      }
      char label[64];
      std::snprintf(label, sizeof(label), "%08X f%u %ux%u", key.address, key.format, key.width,
                    key.height);
      DrawThumbnail(host.texture, key.width, key.height, label);
    }
  }
  if (ImGui::CollapsingHeader("Operations of the last frame shown")) {
    for (const Op& op : g_shown.ops) {
      if (const ClearOp* clear = std::get_if<ClearOp>(&op)) {
        ImGui::Text("Clear %s %u (%d,%d)-(%d,%d) colour %.2f %.2f %.2f %.2f%s%s z %.3f s %u",
                    clear->target.depth ? "D" : "C", clear->target.base, clear->rect.left,
                    clear->rect.top, clear->rect.right, clear->rect.bottom, clear->color[0],
                    clear->color[1], clear->color[2], clear->color[3], clear->depth ? " depth" : "",
                    clear->stencil ? " stencil" : "", clear->z, clear->stencil_value);
      } else {
        const CopyOp& copy = std::get<CopyOp>(op);
        ImGui::Text("Resolve %s %u (%d,%d)-(%d,%d) -> %08X f%u at (%d,%d)%s x%.2f",
                    copy.source.depth ? "D" : "C", copy.source.base, copy.rect.left, copy.rect.top,
                    copy.rect.right, copy.rect.bottom, copy.dest.address, copy.dest.format,
                    copy.dest_x, copy.dest_y, copy.swap ? " swap" : "", copy.scale);
      }
    }
  }
  ImGui::End();
}
