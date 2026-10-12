// Probe hooks on the UE3 renderer (PROBES.md).
//
// The generated overrides (src/hooks_gen.cpp) call in here. While active,
// each thread keeps a stack of the UE3 passes/scopes it is in and the last
// drawing policy (DrawShared) it ran, so every D3D draw can be attributed.
//   gears1_probe            per-frame draws-per-pass summary
//   trigger file            full trace of the next N frames: one line per
//                           draw with a snapshot of the D3D device state
//   gears1_probe_skip       skip passes, scopes or the meshes of a policy
//   gears1_probe_depth_bias_scale / _negate
//                           change every guest depth bias at the D3D setter
// Addresses and layouts are from the Ghidra project (NATIVE_RENDERER.md).

#include "probes.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

REXCVAR_DEFINE_BOOL(gears1_probe, false, "Gears1",
                    "Track UE3 render passes and write the draws per pass of every frame to "
                    "gears1_probe_trace_file");
REXCVAR_DEFINE_STRING(gears1_probe_trace_file, "probe_trace.log", "Gears1",
                      "Output file for gears1_probe summaries and probe traces");
REXCVAR_DEFINE_STRING(gears1_probe_trigger_file, "probe_trigger", "Gears1",
                      "When this file exists, trace every draw of the next "
                      "gears1_probe_trace_frames frames, then delete it");
REXCVAR_DEFINE_INT32(gears1_probe_trace_frames, 1, "Gears1", "Frames traced per trigger");
REXCVAR_DEFINE_STRING(gears1_probe_skip, "", "Gears1",
                      "Comma-separated probe names (tools/hook_table.py) to skip: passes and "
                      "scopes return early, policies skip the meshes they draw");
REXCVAR_DEFINE_DOUBLE(gears1_probe_depth_bias_scale, 1.0, "Gears1",
                      "Multiply every guest depth bias and slope-scaled depth bias (D3D render "
                      "states, after UE3's reversed-Z negation)");
REXCVAR_DEFINE_BOOL(gears1_probe_depth_bias_negate, false, "Gears1",
                    "Negate every guest depth bias and slope-scaled depth bias");

namespace {

// Guest globals.
constexpr uint32_t kReversedZFlag = 0x82BFA554;      // set by RHISetRenderTarget
constexpr uint32_t kSceneColorSurface = 0x82C0DE54;  // surface refs: value = D3D surface
constexpr uint32_t kSceneDepthSurface = 0x82C0DE9C;

// D3D device fields (NATIVE_RENDERER.md "D3D API map").
constexpr uint32_t kDevCommandWrite = 0x28;
constexpr uint32_t kDevColorMask = 0x285C;      // RB_COLOR_MASK
constexpr uint32_t kDevDepthControl = 0x28B4;   // RB_DEPTHCONTROL
constexpr uint32_t kDevBlendControl0 = 0x28B8;  // RB_BLENDCONTROL0
constexpr uint32_t kDevColorControl = 0x28BC;   // RB_COLORCONTROL
constexpr uint32_t kDevModeControl = 0x28C8;    // PA_SU_SC_MODE_CNTL
constexpr uint32_t kDevPolyOffsetScale = 0x29D0;  // written by SetSlopeScaleDepthBias
constexpr uint32_t kDevPolyOffset = 0x29D4;       // written by SetDepthBias
constexpr uint32_t kDevRenderTarget0 = 0x2F88;
constexpr uint32_t kDevDepthStencil = 0x2F98;
constexpr uint32_t kDevPixelShader = 0x3080;
constexpr uint32_t kDevVertexShader = 0x3084;

// FMeshElement fields read by FMeshDrawingPolicy_SetMeshRenderState.
constexpr uint32_t kMeshDepthBias = 0xF0;
constexpr uint32_t kMeshSlopeScaleDepthBias = 0xF4;
constexpr uint32_t kMeshFlags = 0xF8;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}
float LoadFloat(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Load32(base, address));
}

// Settings, refreshed from the cvars once per frame (render thread).
std::atomic<bool> g_active{false};  // anything below needs the pass tracking
std::atomic<bool> g_summary{false};
std::atomic<bool> g_tracing{false};
std::atomic<float> g_bias_scale{1.0f};
std::atomic<bool> g_bias_negate{false};
std::unique_ptr<std::atomic<bool>[]> g_skip;
std::unique_ptr<std::atomic<uint32_t>[]> g_draws;  // per innermost pass; [count] = none
std::string g_skip_text;
uint32_t g_draw_mesh = UINT32_MAX;
int32_t g_trace_frames_left = 0;
uint64_t g_frame = 0;
std::once_flag g_init;
std::mutex g_frame_mutex;  // frame end can come from more than one thread

std::mutex g_out_mutex;
FILE* g_out = nullptr;

void Init() {
  std::call_once(g_init, [] {
    g_skip = std::make_unique<std::atomic<bool>[]>(kGears1ProbeCount);
    g_draws = std::make_unique<std::atomic<uint32_t>[]>(kGears1ProbeCount + 1);
    for (uint32_t i = 0; i < kGears1ProbeCount; ++i) {
      if (std::string_view(kGears1Probes[i].name) == "DrawMesh") g_draw_mesh = i;
    }
  });
}

// Writes one line; the caller formats it.
void Write(const char* text) {
  std::lock_guard<std::mutex> lock(g_out_mutex);
  if (!g_out) {
    const std::string path(REXCVAR_GET(gears1_probe_trace_file));
    // Append: earlier runs (and their traces) are kept.
    g_out = std::fopen(path.c_str(), "a");
    if (!g_out) {
      REXLOG_ERROR("Probe: cannot write {}", path);
      return;
    }
    std::fprintf(g_out, "# run start (pid %d)\n", static_cast<int>(getpid()));
    REXLOG_INFO("Probe: appending to {}", path);
  }
  std::fputs(text, g_out);
}
void Flush() {
  std::lock_guard<std::mutex> lock(g_out_mutex);
  if (g_out) std::fflush(g_out);
}

void UpdateSkip() {
  const std::string text(REXCVAR_GET(gears1_probe_skip));
  if (text == g_skip_text) return;
  g_skip_text = text;
  for (uint32_t i = 0; i < kGears1ProbeCount; ++i) g_skip[i] = false;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) end = text.size();
    const std::string_view name = std::string_view(text).substr(start, end - start);
    if (!name.empty()) {
      bool found = false;
      for (uint32_t i = 0; i < kGears1ProbeCount; ++i) {
        const Gears1ProbeKind kind = kGears1Probes[i].kind;
        if (name == kGears1Probes[i].name &&
            (kind == Gears1ProbeKind::kPass || kind == Gears1ProbeKind::kScope ||
             kind == Gears1ProbeKind::kPolicy)) {
          g_skip[i] = true;
          found = true;
        }
      }
      if (found) {
        REXLOG_INFO("Probe: skipping {}", name);
      } else {
        REXLOG_WARN("Probe: gears1_probe_skip names no pass, scope or policy '{}'", name);
      }
    }
    start = end + 1;
  }
}

struct ThreadState {
  uint32_t stack[32];
  uint32_t depth = 0;
  int32_t policy = -1;
  uint32_t mesh_element = 0;
};
thread_local ThreadState t_state;

// Innermost pass on the stack, or kGears1ProbeCount when outside every pass.
uint32_t CurrentPass(const ThreadState& state) {
  for (uint32_t i = state.depth; i-- > 0;) {
    if (kGears1Probes[state.stack[i]].kind == Gears1ProbeKind::kPass) return state.stack[i];
  }
  return kGears1ProbeCount;
}

// "Render>RenderEmissive>DrawMesh" and the policy, for trace lines.
std::string Where(const ThreadState& state) {
  std::string where;
  for (uint32_t i = 0; i < state.depth; ++i) {
    if (i) where += '>';
    where += kGears1Probes[state.stack[i]].name;
  }
  if (where.empty()) where = "-";
  where += " pol=";
  where += state.policy >= 0 ? kGears1Probes[state.policy].name : "-";
  return where;
}

std::string Surface(const uint8_t* base, uint32_t surface) {
  if (!surface) return "0";
  if (surface == Load32(base, kSceneColorSurface)) return "SceneColor";
  if (surface == Load32(base, kSceneDepthSurface)) return "SceneDepth";
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", surface);
  return text;
}

}  // namespace

bool Gears1ProbeEnter(uint32_t id, PPCContext& ctx, uint8_t*) {
  if (!g_active.load(std::memory_order_relaxed)) return false;
  ThreadState& state = t_state;
  if (g_skip[id].load(std::memory_order_relaxed)) return true;
  if (id == g_draw_mesh) {
    if (state.policy >= 0 && g_skip[state.policy].load(std::memory_order_relaxed)) return true;
    state.mesh_element = ctx.r5.u32;  // DrawMesh(this, device, mesh element, ...)
  }
  if (kGears1Probes[id].kind == Gears1ProbeKind::kPass) {
    state.policy = -1;
    if (g_tracing.load(std::memory_order_relaxed)) {
      char line[256];
      std::snprintf(line, sizeof(line), "F%llu ENTER %s r3=%08X r4=%08X r5=%08X in=%s\n",
                    static_cast<unsigned long long>(g_frame), kGears1Probes[id].name,
                    ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, Where(state).c_str());
      Write(line);
    }
  }
  if (state.depth < std::size(state.stack)) state.stack[state.depth] = id;
  ++state.depth;
  return false;
}

void Gears1ProbeLeave(uint32_t id) {
  ThreadState& state = t_state;
  // Pop through `id` even if probes were switched off in between.
  for (uint32_t i = std::min<uint32_t>(state.depth, std::size(state.stack)); i-- > 0;) {
    if (state.stack[i] == id) {
      state.depth = i;
      break;
    }
  }
  if (id == g_draw_mesh) state.mesh_element = 0;
  if (kGears1Probes[id].kind == Gears1ProbeKind::kPass) {
    state.policy = -1;
    if (g_tracing.load(std::memory_order_relaxed)) {
      char line[128];
      std::snprintf(line, sizeof(line), "F%llu LEAVE %s\n",
                    static_cast<unsigned long long>(g_frame), kGears1Probes[id].name);
      Write(line);
    }
  }
}

void Gears1ProbePolicy(uint32_t id) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  t_state.policy = static_cast<int32_t>(id);
}

void Gears1ProbeDraw(uint32_t id, PPCContext& ctx, uint8_t* base) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  const ThreadState& state = t_state;
  g_draws[CurrentPass(state)].fetch_add(1, std::memory_order_relaxed);
  if (!g_tracing.load(std::memory_order_relaxed)) return;

  const uint32_t dev = ctx.r3.u32;
  char mesh[96] = "-";
  if (state.mesh_element) {
    const uint32_t element = state.mesh_element;
    std::snprintf(mesh, sizeof(mesh), "%08X bias=%g,%g flags=%08X", element,
                  LoadFloat(base, element + kMeshDepthBias),
                  LoadFloat(base, element + kMeshSlopeScaleDepthBias),
                  Load32(base, element + kMeshFlags));
  }
  char line[768];
  std::snprintf(
      line, sizeof(line),
      "F%llu DRAW %s args=%08X,%08X,%08X,%08X in=%s vs=%016llx ps=%016llx rt=%s ds=%s "
      "depth=%08X blend=%08X color=%08X mode=%08X mask=%08X poly=%g,%g revz=%u mesh=%s "
      "cmd=%08X\n",
      static_cast<unsigned long long>(g_frame), kGears1Probes[id].name, ctx.r4.u32, ctx.r5.u32,
      ctx.r6.u32, ctx.r7.u32, Where(state).c_str(),
      static_cast<unsigned long long>(Gears1ShaderHash(Load32(base, dev + kDevVertexShader))),
      static_cast<unsigned long long>(Gears1ShaderHash(Load32(base, dev + kDevPixelShader))),
      Surface(base, Load32(base, dev + kDevRenderTarget0)).c_str(),
      Surface(base, Load32(base, dev + kDevDepthStencil)).c_str(),
      Load32(base, dev + kDevDepthControl), Load32(base, dev + kDevBlendControl0),
      Load32(base, dev + kDevColorControl), Load32(base, dev + kDevModeControl),
      Load32(base, dev + kDevColorMask), LoadFloat(base, dev + kDevPolyOffsetScale),
      LoadFloat(base, dev + kDevPolyOffset), Load32(base, kReversedZFlag), mesh,
      Load32(base, dev + kDevCommandWrite));
  Write(line);
}

void Gears1ProbeEvent(uint32_t id, PPCContext& ctx, uint8_t* base) {
  if (!g_tracing.load(std::memory_order_relaxed)) return;
  char line[512];
  std::snprintf(line, sizeof(line),
                "F%llu EVENT %s r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X f1=%g in=%s\n",
                static_cast<unsigned long long>(g_frame), kGears1Probes[id].name, ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.f1.f64,
                Where(t_state).c_str());
  Write(line);
  (void)base;
}

void Gears1ProbeDepthBiasPre(PPCContext& ctx, uint8_t*) {
  // The setters take the float's bits in r4 (D3D render state DWORD).
  const float scale = g_bias_scale.load(std::memory_order_relaxed);
  const bool negate = g_bias_negate.load(std::memory_order_relaxed);
  if (scale == 1.0f && !negate) return;
  float value = std::bit_cast<float>(ctx.r4.u32) * scale;
  if (negate) value = -value;
  ctx.r4.u64 = std::bit_cast<uint32_t>(value);
}

bool Gears1ProbeWantsShaderHashes() { return g_tracing.load(std::memory_order_relaxed); }

void Gears1ProbeFrameEnd(uint8_t*) {
  Init();
  std::lock_guard<std::mutex> frame_lock(g_frame_mutex);
  UpdateSkip();
  bool skip_any = false;
  for (uint32_t i = 0; i < kGears1ProbeCount; ++i) skip_any |= g_skip[i].load();

  const float bias_scale = static_cast<float>(REXCVAR_GET(gears1_probe_depth_bias_scale));
  const bool bias_negate = REXCVAR_GET(gears1_probe_depth_bias_negate);
  if (bias_scale != g_bias_scale.load() || bias_negate != g_bias_negate.load()) {
    REXLOG_INFO("Probe: guest depth bias x{}{}", bias_scale, bias_negate ? ", negated" : "");
  }
  g_bias_scale = bias_scale;
  g_bias_negate = bias_negate;

  const bool summary = REXCVAR_GET(gears1_probe);
  if (g_summary.load() && g_active.load()) {
    std::string line = "F" + std::to_string(g_frame) + " draws";
    for (uint32_t i = 0; i <= kGears1ProbeCount; ++i) {
      const uint32_t n = g_draws[i].exchange(0, std::memory_order_relaxed);
      if (!n) continue;
      line += ' ';
      line += i < kGears1ProbeCount ? kGears1Probes[i].name : "(outside)";
      line += ':' + std::to_string(n);
    }
    line += '\n';
    Write(line.c_str());
  } else {
    for (uint32_t i = 0; i <= kGears1ProbeCount; ++i) g_draws[i] = 0;
  }

  // Trace: count down, or start when the trigger file appears.
  if (g_trace_frames_left > 0 && --g_trace_frames_left == 0) {
    g_tracing = false;
    Write(("# trace end F" + std::to_string(g_frame) + "\n").c_str());
    REXLOG_INFO("Probe: trace finished at frame {}", g_frame);
  }
  if (g_trace_frames_left == 0) {
    const std::filesystem::path trigger{std::string(REXCVAR_GET(gears1_probe_trigger_file))};
    std::error_code ec;
    if (!trigger.empty() && std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      g_trace_frames_left = std::max(1, REXCVAR_GET(gears1_probe_trace_frames));
      g_tracing = true;
      Write(("# trace start F" + std::to_string(g_frame + 1) + " for " +
             std::to_string(g_trace_frames_left) + " frames\n")
                .c_str());
      REXLOG_INFO("Probe: tracing {} frames from frame {}", g_trace_frames_left, g_frame + 1);
    }
  }

  g_summary = summary;
  g_active = summary || skip_any || g_tracing.load();
  ++g_frame;
  Flush();
}
