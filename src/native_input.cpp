// Native input (NATIVE_PORT.md seam 1, stage 1): the game's XAM input
// imports implemented on SDL instead of ReXGlue's input system. How the
// import overrides and the shared SDL work: NATIVE_PORT.md "Status".
//
// User 0 gets the first SDL gamepad merged with the keyboard/mouse mapping
// below; users 1-3 get the next gamepads. Mouse look is src/mouse_look.cpp.

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging/macros.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include "probes.h"

REXCVAR_DEFINE_BOOL(gears1_native_input, true, "Gears1",
                    "Read pads (and keyboard/mouse) natively through SDL instead of ReXGlue's "
                    "input system");
REXCVAR_DEFINE_BOOL(gears1_native_input_keyboard, true, "Gears1",
                    "Map keyboard and mouse buttons onto player 1's pad (WASD left stick, arrows "
                    "right stick, mouse buttons triggers)");

using rex::X_RESULT;  // used by the X_ERROR_* macros

namespace {

using namespace rex::input;
using rex::memory::GuestPtr;

constexpr uint32_t kFlagAnyUser = 0x40000000;
constexpr uint32_t kUserCount = 4;

struct Pad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // XInput convention: +Y is up
  bool connected = false;
};

void WriteGamepad(X_INPUT_GAMEPAD& out, const Pad& pad) {
  out.buttons = pad.buttons;
  out.left_trigger = pad.left_trigger;
  out.right_trigger = pad.right_trigger;
  out.thumb_lx = pad.lx;
  out.thumb_ly = pad.ly;
  out.thumb_rx = pad.rx;
  out.thumb_ry = pad.ry;
}

int16_t InvertAxis(int16_t v) { return v == INT16_MIN ? INT16_MAX : int16_t(-v); }
uint8_t TriggerToByte(int16_t v) { return uint8_t(std::clamp(int(v), 0, 32767) * 255 / 32767); }
int16_t PickStronger(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

std::mutex g_mutex;
SDL_Gamepad* g_gamepads[kUserCount] = {};

// Focus and mouse buttons from SDL events, delivered on the thread that pumps
// them (the main thread): SDL's focus and mouse state getters must not be
// called from guest threads.
std::atomic<bool> g_focused{true};
std::atomic<uint32_t> g_mouse_buttons{0};
std::once_flag g_watch_once;

bool OnEvent(void*, SDL_Event* event) {
  switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      g_focused = true;
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      g_focused = false;
      g_mouse_buttons = 0;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      g_mouse_buttons.fetch_or(SDL_BUTTON_MASK(event->button.button));
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      g_mouse_buttons.fetch_and(~uint32_t(SDL_BUTTON_MASK(event->button.button)));
      break;
    default:
      break;
  }
  return true;
}

void EnsureEventWatch() {
  std::call_once(g_watch_once, [] { SDL_AddEventWatch(OnEvent, nullptr); });
}

bool KeyDown(SDL_Scancode scancode) {
  int key_count = 0;
  const bool* keys = SDL_GetKeyboardState(&key_count);
  return keys && int(scancode) < key_count && keys[scancode];
}

// Opens SDL gamepads in connection order, one per user.
void RefreshGamepads() {
  for (auto*& gamepad : g_gamepads) {
    if (gamepad && !SDL_GamepadConnected(gamepad)) {
      SDL_CloseGamepad(gamepad);
      gamepad = nullptr;
    }
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (!ids) return;
  for (int i = 0; i < count; ++i) {
    bool open = false;
    for (auto* gamepad : g_gamepads) open |= gamepad && SDL_GetGamepadID(gamepad) == ids[i];
    if (open) continue;
    for (auto*& gamepad : g_gamepads) {
      if (!gamepad) {
        gamepad = SDL_OpenGamepad(ids[i]);
        if (gamepad) REXLOG_INFO("Native input: pad {} = {}", &gamepad - g_gamepads,
                                 SDL_GetGamepadName(gamepad));
        break;
      }
    }
  }
  SDL_free(ids);
}

void ReadGamepad(SDL_Gamepad* gamepad, Pad& pad) {
  if (!gamepad) return;
  pad.connected = true;
  struct {
    SDL_GamepadButton button;
    uint16_t bit;
  } constexpr kMap[] = {
      {SDL_GAMEPAD_BUTTON_DPAD_UP, X_INPUT_GAMEPAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, X_INPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, X_INPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, X_INPUT_GAMEPAD_DPAD_RIGHT},
      {SDL_GAMEPAD_BUTTON_START, X_INPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_BACK, X_INPUT_GAMEPAD_BACK},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, X_INPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, X_INPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_SOUTH, X_INPUT_GAMEPAD_A},
      {SDL_GAMEPAD_BUTTON_EAST, X_INPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, X_INPUT_GAMEPAD_X},
      {SDL_GAMEPAD_BUTTON_NORTH, X_INPUT_GAMEPAD_Y},
  };
  for (const auto& m : kMap) {
    if (SDL_GetGamepadButton(gamepad, m.button)) pad.buttons |= m.bit;
  }
  pad.left_trigger = TriggerToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
  pad.right_trigger = TriggerToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
  pad.lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  pad.ly = InvertAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
  pad.rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
  pad.ry = InvertAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
}

// Keyboard/mouse onto the pad (merged with player 1's gamepad). Gears 1
// layout: A cover/run, B melee, X use, Y look at, RB reload, D-pad weapons.
void ReadKeyboard(Pad& pad) {
  if (!REXCVAR_GET(gears1_native_input_keyboard)) return;
  // Player 1 stays connected without a gamepad (and without focus), or the
  // game pauses for a disconnected controller.
  pad.connected = true;
  if (!g_focused.load(std::memory_order_relaxed)) return;
  struct {
    SDL_Scancode key;
    uint16_t bit;
  } constexpr kMap[] = {
      {SDL_SCANCODE_SPACE, X_INPUT_GAMEPAD_A},
      {SDL_SCANCODE_F, X_INPUT_GAMEPAD_B},
      {SDL_SCANCODE_E, X_INPUT_GAMEPAD_X},
      {SDL_SCANCODE_Q, X_INPUT_GAMEPAD_Y},
      {SDL_SCANCODE_R, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_SCANCODE_TAB, X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_SCANCODE_C, X_INPUT_GAMEPAD_LEFT_THUMB},
      {SDL_SCANCODE_1, X_INPUT_GAMEPAD_DPAD_UP},
      {SDL_SCANCODE_2, X_INPUT_GAMEPAD_DPAD_RIGHT},
      {SDL_SCANCODE_3, X_INPUT_GAMEPAD_DPAD_DOWN},
      {SDL_SCANCODE_4, X_INPUT_GAMEPAD_DPAD_LEFT},
      {SDL_SCANCODE_ESCAPE, X_INPUT_GAMEPAD_START},
      {SDL_SCANCODE_RETURN, X_INPUT_GAMEPAD_START},
      {SDL_SCANCODE_BACKSPACE, X_INPUT_GAMEPAD_BACK},
  };
  for (const auto& m : kMap) {
    if (KeyDown(m.key)) pad.buttons |= m.bit;
  }
  auto axis = [](SDL_Scancode negative, SDL_Scancode positive) {
    return int16_t((KeyDown(positive) ? 32767 : 0) - (KeyDown(negative) ? 32767 : 0));
  };
  pad.lx = PickStronger(pad.lx, axis(SDL_SCANCODE_A, SDL_SCANCODE_D));
  pad.ly = PickStronger(pad.ly, axis(SDL_SCANCODE_S, SDL_SCANCODE_W));
  pad.rx = PickStronger(pad.rx, axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT));
  pad.ry = PickStronger(pad.ry, axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP));

  const uint32_t mouse = g_mouse_buttons.load(std::memory_order_relaxed);
  if (mouse & SDL_BUTTON_RMASK) pad.left_trigger = 255;                      // aim
  if (mouse & SDL_BUTTON_LMASK) pad.right_trigger = 255;                     // fire
  if (mouse & SDL_BUTTON_MMASK) pad.buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;  // zoom
}

// Returns false when the user has no device.
bool ReadPad(uint32_t user, Pad& pad) {
  EnsureEventWatch();
  std::lock_guard<std::mutex> lock(g_mutex);
  // Enumerating allocates; hotplug only needs a check every half second.
  // (SDL_GetGamepads returns nothing if the gamepad subsystem isn't up.)
  static uint64_t last_refresh = 0;
  const uint64_t now = SDL_GetTicks();
  if (!last_refresh || now - last_refresh >= 500) {
    last_refresh = now;
    RefreshGamepads();
  }
  ReadGamepad(g_gamepads[user], pad);
  if (user == 0) ReadKeyboard(pad);
  return pad.connected;
}

PPCFunc* Original(const char* name) {
  auto* function = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
  if (!function) REXLOG_ERROR("Native input: no runtime {} to fall back to", name);
  return function;
}

uint32_t UserIndex(uint32_t user_index, uint32_t flags) {
  return ((user_index & 0xFF) == 0xFF || (flags & kFlagAnyUser)) ? 0 : user_index;
}

// Requests for another device type or a user we don't serve.
bool NoDevice(uint32_t user, uint32_t flags) {
  return ((flags & 0xFF) && !(flags & X_INPUT_FLAG_GAMEPAD)) || user >= kUserCount;
}

uint32_t g_packet[kUserCount] = {};
Pad g_last[kUserCount];

}  // namespace

bool Gears1KeyDown(int scancode) {
  EnsureEventWatch();
  return g_focused.load(std::memory_order_relaxed) && KeyDown(SDL_Scancode(scancode));
}

// XamInputGetState(user_index, flags, XINPUT_STATE* state)
extern "C" REX_FUNC(__imp__XamInputGetState) {
  if (!REXCVAR_GET(gears1_native_input)) {
    static PPCFunc* const original = Original("__imp__XamInputGetState");
    if (original) original(ctx, base);
    return;
  }
  const uint32_t flags = ctx.r4.u32;
  const uint32_t state = ctx.r5.u32;
  const uint32_t user = UserIndex(ctx.r3.u32, flags);
  Pad pad;
  if (NoDevice(user, flags) || !ReadPad(user, pad)) {
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
    return;
  }
  // Games call this with a null state as a connection query.
  if (state) {
    const Pad& last = g_last[user];
    if (pad.buttons != last.buttons || pad.left_trigger != last.left_trigger ||
        pad.right_trigger != last.right_trigger || pad.lx != last.lx || pad.ly != last.ly ||
        pad.rx != last.rx || pad.ry != last.ry) {
      ++g_packet[user];
      g_last[user] = pad;
    }
    auto* out = GuestPtr<X_INPUT_STATE*>(base, state);
    out->packet_number = g_packet[user];
    WriteGamepad(out->gamepad, pad);
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamInputGetCapabilities(user_index, flags, XINPUT_CAPABILITIES* caps)
extern "C" REX_FUNC(__imp__XamInputGetCapabilities) {
  if (!REXCVAR_GET(gears1_native_input)) {
    static PPCFunc* const original = Original("__imp__XamInputGetCapabilities");
    if (original) original(ctx, base);
    return;
  }
  const uint32_t flags = ctx.r4.u32;
  const uint32_t caps = ctx.r5.u32;
  const uint32_t user = UserIndex(ctx.r3.u32, flags);
  if (!caps) {
    ctx.r3.u64 = X_ERROR_BAD_ARGUMENTS;
    return;
  }
  Pad pad;
  if (NoDevice(user, flags) || !ReadPad(user, pad)) {
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
    return;
  }
  auto* out = GuestPtr<X_INPUT_CAPABILITIES*>(base, caps);
  *out = {};
  out->type = XINPUT_DEVTYPE_GAMEPAD;
  out->sub_type = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
  Pad full;           // all buttons and full axis ranges supported
  full.buttons = 0xF3FF;
  full.left_trigger = full.right_trigger = 0xFF;
  full.lx = full.ly = full.rx = full.ry = int16_t(0xFFC0);
  WriteGamepad(out->gamepad, full);
  out->vibration.left_motor_speed = 0xFFFF;
  out->vibration.right_motor_speed = 0xFFFF;
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamInputSetState(user_index, unknown, XINPUT_VIBRATION* vibration)
extern "C" REX_FUNC(__imp__XamInputSetState) {
  if (!REXCVAR_GET(gears1_native_input)) {
    static PPCFunc* const original = Original("__imp__XamInputSetState");
    if (original) original(ctx, base);
    return;
  }
  const uint32_t vibration = ctx.r5.u32;
  if (!vibration) {
    ctx.r3.u64 = X_ERROR_BAD_ARGUMENTS;
    return;
  }
  const uint32_t user = UserIndex(ctx.r3.u32, 0);
  std::lock_guard<std::mutex> lock(g_mutex);
  SDL_Gamepad* gamepad = user < kUserCount ? g_gamepads[user] : nullptr;
  if (!gamepad) {
    // Keyboard-only player 1 accepts (and ignores) vibration.
    ctx.r3.u64 = user == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    return;
  }
  // Left motor is low frequency, right is high frequency; held until the
  // next call (the game stops it explicitly).
  const auto* in = GuestPtr<const X_INPUT_VIBRATION*>(base, vibration);
  SDL_RumbleGamepad(gamepad, in->left_motor_speed, in->right_motor_speed, 0xFFFFFFFF);
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
