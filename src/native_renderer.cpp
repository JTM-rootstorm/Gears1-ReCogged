// Native renderer, phase 1 skeleton (NATIVE_RENDERER.md): an SDL_GPU device
// and window next to the emulated renderer, which still consumes the game's
// command buffers (D3D waits on it). Each guest frame (D3D Swap) clears the
// window and draws an ImGui overlay; later phases render the game's own D3D
// calls here and compare against the emulated window.
//
// Window and swapchain calls must run on the main thread, so the hooks only
// record state and post work there (SDL_RunOnMainThread). ImGui is the
// runtime's (librexruntime.so exports it) in a context of our own; the
// SDL_GPU backend is third_party/imgui.

#include <atomic>
#include <cmath>
#include <cstdint>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "imgui_impl_sdlgpu3.h"
#include "native_renderer.h"
#include "probes.h"

REXCVAR_DEFINE_BOOL(gears1_native_renderer, false, "Gears1",
                    "Open the native renderer's window (SDL_GPU) next to the emulated one")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Set by the device init hook (guest thread), read on the main thread.
std::atomic<uint32_t> g_guest_device{0};
std::atomic<uint32_t> g_back_buffer_width{0}, g_back_buffer_height{0};
std::atomic<uint64_t> g_guest_frames{0};
// A present is queued on the main thread and not finished yet.
std::atomic<bool> g_present_pending{false};

// Main thread only.
SDL_Window* g_window = nullptr;
SDL_GPUDevice* g_device = nullptr;
ImGuiContext* g_imgui = nullptr;
uint64_t g_presented_frames = 0;
uint64_t g_last_present_ns = 0;
double g_frame_ms = 0.0;

// Makes our ImGui context current for the scope, restoring the runtime's.
class ImGuiScope {
 public:
  ImGuiScope() : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(g_imgui); }
  ~ImGuiScope() { ImGui::SetCurrentContext(previous_); }

 private:
  ImGuiContext* previous_;
};

void Create(void*) {
  const int width = int(g_back_buffer_width.load()), height = int(g_back_buffer_height.load());
  g_window = SDL_CreateWindow("Gears of War - native renderer", width ? width : 1280,
                              height ? height : 720, SDL_WINDOW_RESIZABLE);
  g_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false,
                                 nullptr);
  if (!g_window || !g_device || !SDL_ClaimWindowForGPUDevice(g_device, g_window)) {
    REXLOG_ERROR("Native renderer: no SDL_GPU window: {}", SDL_GetError());
    return;
  }
  g_imgui = ImGui::CreateContext();
  ImGuiScope scope;
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplSDLGPU3_InitInfo init;
  init.Device = g_device;
  init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(g_device, g_window);
  ImGui_ImplSDLGPU3_Init(&init);
  REXLOG_INFO("Native renderer: {} window {}x{}", SDL_GetGPUDeviceDriver(g_device), width, height);
}

void Present(void*) {
  g_present_pending = false;
  if (!g_imgui) {
    return;
  }
  SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(g_device);
  SDL_GPUTexture* swapchain = nullptr;
  Uint32 width = 0, height = 0;
  if (!command_buffer ||
      !SDL_AcquireGPUSwapchainTexture(command_buffer, g_window, &swapchain, &width, &height)) {
    if (command_buffer) {
      SDL_CancelGPUCommandBuffer(command_buffer);
    }
    return;
  }
  if (!swapchain) {
    // Minimized or not ready; nothing to draw into.
    SDL_SubmitGPUCommandBuffer(command_buffer);
    return;
  }

  NativeTexturesUpload(g_device, command_buffer);

  const uint64_t now = SDL_GetTicksNS();
  if (g_last_present_ns) {
    g_frame_ms = (now - g_last_present_ns) / 1e6;
  }
  g_last_present_ns = now;
  ++g_presented_frames;

  ImGuiScope scope;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(width), float(height));
  io.DeltaTime = g_frame_ms > 0.0 ? float(g_frame_ms / 1000.0) : 1.0f / 60.0f;
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
  ImGui::Begin("Native renderer", nullptr,
               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::Text("Phase 1 skeleton: clear + overlay");
  ImGui::Text("Guest D3D device %08X, back buffer %ux%u", g_guest_device.load(),
              g_back_buffer_width.load(), g_back_buffer_height.load());
  ImGui::Text("Guest frames %llu, presented %llu", (unsigned long long)g_guest_frames.load(),
              (unsigned long long)g_presented_frames);
  ImGui::Text("%.2f ms", g_frame_ms);
  ImGui::End();
  NativeTexturesDrawBrowser();
  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();
  ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

  // Slowly cycling hue, so a stalled window is obvious.
  const float hue = float(g_guest_frames.load() % 600) / 600.0f * 6.2831853f;
  SDL_GPUColorTargetInfo target = {};
  target.texture = swapchain;
  target.load_op = SDL_GPU_LOADOP_CLEAR;
  target.store_op = SDL_GPU_STOREOP_STORE;
  target.clear_color = {0.25f + 0.15f * std::sin(hue), 0.25f + 0.15f * std::sin(hue + 2.094f),
                        0.25f + 0.15f * std::sin(hue + 4.189f), 1.0f};
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer, &target, 1, nullptr);
  ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, pass);
  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(command_buffer);
}

}  // namespace

// Post hook of the D3D device init (8223A840, tools/hook_table.py): r3 = the
// device, r4 = D3DPRESENT_PARAMETERS (BackBufferWidth, BackBufferHeight
// first).
void Gears1NativeRendererDeviceInit(const Gears1HookEntry& entry, PPCContext&, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  const uint8_t* parameters = rex::memory::GuestPtr(base, entry.r4);
  g_back_buffer_width = rex::memory::load_and_swap<uint32_t>(parameters);
  g_back_buffer_height = rex::memory::load_and_swap<uint32_t>(parameters + 4);
  g_guest_device = entry.r3;
  SDL_RunOnMainThread(Create, nullptr, false);
}

// Post hook of D3D Swap: one per guest frame.
void Gears1NativeRendererFrame(const Gears1HookEntry&, PPCContext&, uint8_t*) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  ++g_guest_frames;
  if (!g_present_pending.exchange(true)) {
    SDL_RunOnMainThread(Present, nullptr, false);
  }
}
