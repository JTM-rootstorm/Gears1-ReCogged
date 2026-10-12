// Native mouse look (NATIVE_PORT.md seam 1, stage 2).
//
// The game's look acceleration and smoothing live in UnrealScript
// (WarPlayerInput, run from the PlayerTick event), so feeding the mouse in as
// a right stick keeps them. Instead, raw mouse motion is added straight to
// the player controller's view rotation after APlayerController::Tick
// (823CB580) has run PlayerTick (and with it UpdateRotation), so the view of
// the same frame already uses it and the script never sees the mouse.
//
// Evidence (Ghidra project, NATIVE_PORT.md):
// - 823CB580 calls the PlayerTick event (FName global 0x82BFB940) through
//   FindFunctionChecked 822AA9B0 + UObject::ProcessEvent (vtable +0xA4); it
//   only runs the full tick when its TickType (r5) is 2 (LEVELTICK_All).
// - AActor::Rotation is at +0xD8 (Pitch, Yaw, Roll; int32): ULevel::MoveActor
//   8239E3F8, reached from AActor::SetRotation 8236BAC8 / execSetRotation,
//   compares the new rotation with +0xD8 and stores it there.
// The script clamps pitch again in the next UpdateRotation; the clamp here
// only keeps a single frame from flipping over the poles.
//
// F7 toggles mouse capture (relative mode). Mouse buttons stay mapped to the
// triggers by src/native_input.cpp.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc.h>

#include "probes.h"

REXCVAR_DEFINE_BOOL(gears1_mouse_look, true, "Gears1",
                    "Turn the camera with the mouse, added to the view rotation without the "
                    "controller's look acceleration (F7 toggles mouse capture)");
REXCVAR_DEFINE_DOUBLE(gears1_mouse_sensitivity, 12.0, "Gears1",
                      "Mouse look: rotation units per mouse count (65536 = 360 degrees)");
REXCVAR_DEFINE_BOOL(gears1_mouse_invert_y, false, "Gears1", "Mouse look: invert vertical look");

namespace {

constexpr uint32_t kTickAll = 2;            // ELevelTick LEVELTICK_All
constexpr uint32_t kRotationPitch = 0xD8;   // AActor::Rotation
constexpr uint32_t kRotationYaw = 0xDC;
constexpr int32_t kPitchLimit = 16000;      // just under 90 degrees (16384)

int32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return int32_t(__builtin_bswap32(v));
}
void Store32(uint8_t* p, int32_t value) {
  const uint32_t v = __builtin_bswap32(uint32_t(value));
  std::memcpy(p, &v, 4);
}

// Mouse motion collected by an SDL event watch on the thread pumping events.
std::atomic<float> g_dx{0.0f}, g_dy{0.0f};
std::atomic<bool> g_capture_wanted{true};
std::atomic<bool> g_watch_added{false};
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
  const bool wanted = g_capture_wanted.load() && REXCVAR_GET(gears1_mouse_look);
  if (SDL_GetWindowRelativeMouseMode(window) != wanted) {
    SDL_SetWindowRelativeMouseMode(window, wanted);
    REXLOG_INFO("Mouse look: capture {}", wanted ? "on" : "off");
  }
}

// F7 edge (guest thread) and capture upkeep.
void UpdateCapture() {
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  const bool down =
      keys && SDL_SCANCODE_F7 < count && keys[SDL_SCANCODE_F7] && Gears1WindowFocused();
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
  if (!g_watch_added.exchange(true)) {
    SDL_AddEventWatch(OnEvent, nullptr);
    REXLOG_INFO("Mouse look: on (F7 toggles capture)");
  }
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

  uint8_t* rotation = base + entry.r3;
  Store32(rotation + kRotationYaw, Load32(rotation + kRotationYaw) + yaw_delta);
  // Pitch is kept in [0, 65536) or signed by the script; clamp as signed and
  // write it back in the same form.
  const int32_t pitch = Load32(rotation + kRotationPitch);
  const bool unsigned_form = pitch >= 0 && pitch < 65536;
  int32_t signed_pitch = int32_t(int16_t(uint16_t(pitch)));
  signed_pitch = std::clamp(signed_pitch + pitch_delta, -kPitchLimit, kPitchLimit);
  Store32(rotation + kRotationPitch,
          unsigned_form ? int32_t(uint16_t(signed_pitch)) : signed_pitch);
}
