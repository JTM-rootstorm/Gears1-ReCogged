// Native renderer (NATIVE_RENDERER.md): an SDL_GPU device and window next to
// the emulated renderer, which still consumes the game's command buffers (D3D
// waits on it). Each guest frame (D3D Swap) shows the front buffer the native
// render targets resolved into (src/native_targets.cpp), or a slowly cycling
// clear colour until there is one, under an ImGui overlay; later phases render
// the game's own draws here and compare against the emulated window.
//
// Window and swapchain calls must run on the main thread, so the hooks only
// record state and post work there (SDL_RunOnMainThread). ImGui is the
// runtime's (librexruntime.so exports it) in a context of our own; the
// SDL_GPU backend is third_party/imgui.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <vector>

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
REXCVAR_DEFINE_BOOL(gears1_native_gpu_debug, false, "Gears1",
                    "Create the native renderer's SDL_GPU device in debug mode (validation)")
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

// Mouse input for the overlay. Event watches run on whichever thread pushes
// the event, and ImGui's current context is process-wide, so the watch only
// queues the native window's mouse events; Present feeds them to ImGui.
std::atomic<SDL_WindowID> g_window_id{0};
std::mutex g_events_mutex;
std::vector<SDL_Event> g_events;

bool OnEvent(void*, SDL_Event* event) {
  SDL_WindowID window = 0;
  switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
      window = event->motion.windowID;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      window = event->button.windowID;
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      window = event->wheel.windowID;
      break;
    default:
      return true;
  }
  if (window && window == g_window_id.load()) {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    g_events.push_back(*event);
  }
  return true;
}

// Main thread, with our ImGui context current.
void FeedEvents() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    events.swap(g_events);
  }
  ImGuiIO& io = ImGui::GetIO();
  for (const SDL_Event& event : events) {
    switch (event.type) {
      case SDL_EVENT_MOUSE_MOTION:
        io.AddMousePosEvent(event.motion.x, event.motion.y);
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        // ImGui: 0 left, 1 right, 2 middle; SDL: 1 left, 2 middle, 3 right.
        constexpr int kImGuiButton[] = {-1, 0, 2, 1, 3, 4};
        const uint8_t button = event.button.button;
        if (button < std::size(kImGuiButton) && kImGuiButton[button] >= 0) {
          io.AddMouseButtonEvent(kImGuiButton[button], event.button.down);
        }
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        break;
      default:
        break;
    }
  }
}

void Create(void*) {
  const int width = int(g_back_buffer_width.load()), height = int(g_back_buffer_height.load());
  g_window = SDL_CreateWindow("Gears of War - native renderer", width ? width : 1280,
                              height ? height : 720, SDL_WINDOW_RESIZABLE);
  g_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL,
                                 REXCVAR_GET(gears1_native_gpu_debug), nullptr);
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
  g_window_id = SDL_GetWindowID(g_window);
  SDL_AddEventWatch(OnEvent, nullptr);
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
  NativeBuffersUpload(g_device, command_buffer);
  NativeTargetsExecute(g_device, command_buffer);

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
  FeedEvents();
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
  ImGui::Begin("Native renderer", nullptr,
               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::Text("Phase 2: resources (textures, buffers, render targets)");
  ImGui::Text("Guest D3D device %08X, back buffer %ux%u", g_guest_device.load(),
              g_back_buffer_width.load(), g_back_buffer_height.load());
  ImGui::Text("Guest frames %llu, presented %llu", (unsigned long long)g_guest_frames.load(),
              (unsigned long long)g_presented_frames);
  ImGui::Text("%.2f ms", g_frame_ms);
  ImGui::End();
  NativeTexturesDrawBrowser();
  NativeBuffersDrawBrowser();
  NativeTargetsDrawBrowser();
  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();
  ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
  NativeBuffersRenderPreview(g_device, command_buffer);

  const bool presented =
      NativeTargetsPresent(g_device, command_buffer, swapchain,
                           SDL_GetGPUSwapchainTextureFormat(g_device, g_window), width, height);
  // Until then a slowly cycling hue, so a stalled window is obvious.
  const float hue = float(g_guest_frames.load() % 600) / 600.0f * 6.2831853f;
  SDL_GPUColorTargetInfo target = {};
  target.texture = swapchain;
  target.load_op = presented ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
  target.store_op = SDL_GPU_STOREOP_STORE;
  target.clear_color = {0.25f + 0.15f * std::sin(hue), 0.25f + 0.15f * std::sin(hue + 2.094f),
                        0.25f + 0.15f * std::sin(hue + 4.189f), 1.0f};
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer, &target, 1, nullptr);
  ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, pass);
  SDL_EndGPURenderPass(pass);
  if (!NativeTargetsQueueDump(g_device, command_buffer)) {
    SDL_SubmitGPUCommandBuffer(command_buffer);
    return;
  }
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
  if (fence) {
    SDL_WaitForGPUFences(g_device, true, &fence, 1);
    SDL_ReleaseGPUFence(g_device, fence);
  }
  NativeTargetsFinishDump(g_device);
}

}  // namespace

SDL_Window* NativeRendererWindow() {
  return g_window;
}

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

// Post hook of D3D Swap: one per guest frame. r3 = device, r4 = the front
// buffer texture.
void Gears1NativeRendererFrame(const Gears1HookEntry& entry, PPCContext&, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  ++g_guest_frames;
  NativeBuffersEndFrame();
  NativeTargetsEndFrame(entry.r4, base);
  if (!g_present_pending.exchange(true)) {
    SDL_RunOnMainThread(Present, nullptr, false);
  }
}
