// Native mouse look (NATIVE_PORT.md seam 1, stage 2; evidence in its
// "Status"): raw mouse motion is added to the player controller's rotation
// after APlayerController::Tick has run the script's UpdateRotation, so the
// script's look acceleration and smoothing never see the mouse. Mouse buttons
// stay mapped to the triggers by src/native_input.cpp.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "native_renderer.h"
#include "probes.h"

REXCVAR_DEFINE_BOOL(gears1_mouse_look, true, "Gears1",
                    "Turn the camera with the mouse (F7 toggles mouse capture)");
REXCVAR_DEFINE_DOUBLE(gears1_mouse_sensitivity, 12.0, "Gears1",
                      "Mouse look: rotation units per mouse count (65536 = 360 degrees)")
    .range(0.1, 1000.0);
REXCVAR_DEFINE_BOOL(gears1_mouse_invert_y, false, "Gears1", "Mouse look: invert vertical look");

namespace {

constexpr uint32_t kTickAll = 2;            // ELevelTick LEVELTICK_All
constexpr uint32_t kRotationPitch = 0xD8;   // AActor::Rotation
constexpr uint32_t kRotationYaw = 0xDC;
constexpr int32_t kPitchLimit = 16000;      // just under 90 degrees (16384)

// Mouse motion collected by an SDL event watch on the thread pumping events.
std::atomic<float> g_dx{0.0f}, g_dy{0.0f};
std::atomic<bool> g_capture_wanted{true};
std::once_flag g_watch_once;
// Sub-unit remainders, so slow movement isn't truncated away.
double g_yaw_rest = 0.0, g_pitch_rest = 0.0;
bool g_toggle_down = false;

bool OnEvent(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_MOUSE_MOTION) {
    SDL_Window* window = SDL_GetWindowFromID(event->motion.windowID);
    if (window && SDL_GetWindowRelativeMouseMode(window)) {
      g_dx.fetch_add(event->motion.xrel, std::memory_order_relaxed);
      g_dy.fetch_add(event->motion.yrel, std::memory_order_relaxed);
    }
  }
  return true;
}

// Window calls must run on the main thread.
void ApplyCapture(void*) {
  SDL_Window* window = SDL_GetKeyboardFocus();
  if (!window) return;
  // Never capture the native renderer's window: its overlay needs the cursor.
  const bool wanted = g_capture_wanted.load() && REXCVAR_GET(gears1_mouse_look) &&
                      window != NativeRendererWindow();
  if (SDL_GetWindowRelativeMouseMode(window) != wanted) {
    SDL_SetWindowRelativeMouseMode(window, wanted);
    REXLOG_INFO("Mouse look: capture {}", wanted ? "on" : "off");
  }
}

// F7 edge (guest thread) and capture upkeep.
void UpdateCapture() {
  const bool down = Gears1KeyDown(SDL_SCANCODE_F7);
  const bool toggled = down && !g_toggle_down;
  if (toggled) g_capture_wanted = !g_capture_wanted.load();
  g_toggle_down = down;
  // Re-apply now and then too: focus changes and the first frames with a
  // window need it.
  static uint32_t ticks = 0;
  if (toggled || ticks++ % 60 == 0) SDL_RunOnMainThread(ApplyCapture, nullptr, false);
}

}  // namespace

// Post hook of APlayerController::Tick (tools/hook_table.py).
void Gears1MouseLookAfterTick(const Gears1HookEntry& entry, PPCContext&, uint8_t* base) {
  if (!REXCVAR_GET(gears1_mouse_look)) {
    g_dx = 0.0f;
    g_dy = 0.0f;
    return;
  }
  std::call_once(g_watch_once, [] {
    SDL_AddEventWatch(OnEvent, nullptr);
    REXLOG_INFO("Mouse look: on (F7 toggles capture)");
  });
  UpdateCapture();
  const double dx = g_dx.exchange(0.0f, std::memory_order_relaxed);
  const double dy = g_dy.exchange(0.0f, std::memory_order_relaxed);
  if (entry.r5 != kTickAll || !entry.r3 || (dx == 0.0 && dy == 0.0)) return;

  const double sensitivity = REXCVAR_GET(gears1_mouse_sensitivity);
  g_yaw_rest += dx * sensitivity;
  // Mouse down is +y; pitch up is +pitch.
  g_pitch_rest += (REXCVAR_GET(gears1_mouse_invert_y) ? dy : -dy) * sensitivity;
  const int32_t yaw_delta = int32_t(std::trunc(g_yaw_rest));
  const int32_t pitch_delta = int32_t(std::trunc(g_pitch_rest));
  g_yaw_rest -= yaw_delta;
  g_pitch_rest -= pitch_delta;

  using rex::memory::load_and_swap;
  using rex::memory::store_and_swap;
  uint8_t* actor = rex::memory::GuestPtr(base, entry.r3);
  store_and_swap<int32_t>(actor + kRotationYaw,
                          load_and_swap<int32_t>(actor + kRotationYaw) + yaw_delta);
  // Pitch is kept in [0, 65536) or signed by the script; clamp as signed and
  // write it back in the same form. The script clamps it again in the next
  // UpdateRotation; this only keeps one frame from flipping over the poles.
  const int32_t pitch = load_and_swap<int32_t>(actor + kRotationPitch);
  const bool unsigned_form = pitch >= 0 && pitch < 65536;
  int32_t signed_pitch = int32_t(int16_t(uint16_t(pitch)));
  signed_pitch = std::clamp(signed_pitch + pitch_delta, -kPitchLimit, kPitchLimit);
  store_and_swap<int32_t>(actor + kRotationPitch,
                          unsigned_form ? int32_t(uint16_t(signed_pitch)) : signed_pitch);
}
