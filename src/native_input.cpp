// Native input (NATIVE_PORT.md seam 1, stage 1): the game's XAM input
// imports implemented on SDL instead of ReXGlue's input system.
//
// The game reads pads only through XamInputGetState / GetCapabilities /
// SetState (via the XDK wrappers 826127C0..D8). Their import thunks are
// resolved to `__imp__XamInput*` symbols that librexruntime.so defines; a
// definition in the executable takes precedence, so these replace them. The
// runtime's versions stay reachable with dlsym(RTLD_NEXT) for the fallback
// (gears1_native_input=false).
//
// SDL is the runtime's copy (librexruntime.so exports it and pumps its
// events); linking a second SDL would have its own state and no window.
//
// User 0 gets the first SDL gamepad merged with the keyboard/mouse mapping
// below; users 1-3 get the next gamepads. Mouse look (stage 2) will bypass
// the controller path entirely.

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc.h>

#include "probes.h"

REXCVAR_DEFINE_BOOL(gears1_native_input, true, "Gears1",
                    "Read pads (and keyboard/mouse) natively through SDL instead of ReXGlue's "
                    "input system");
REXCVAR_DEFINE_BOOL(gears1_native_input_keyboard, true, "Gears1",
                    "Map keyboard and mouse buttons onto player 1's pad (WASD left stick, arrows "
                    "right stick, mouse buttons triggers)");

namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorBadArguments = 0xA0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kFlagGamepad = 0x1;
constexpr uint32_t kFlagAnyUser = 0x40000000;
constexpr uint32_t kUserCount = 4;

// XINPUT_GAMEPAD wButtons.
enum : uint16_t {
  kDpadUp = 0x0001,
  kDpadDown = 0x0002,
  kDpadLeft = 0x0004,
  kDpadRight = 0x0008,
  kStart = 0x0010,
  kBack = 0x0020,
  kLeftThumb = 0x0040,
  kRightThumb = 0x0080,
  kLeftShoulder = 0x0100,
  kRightShoulder = 0x0200,
  kA = 0x1000,
  kB = 0x2000,
  kX = 0x4000,
  kY = 0x8000,
};

struct Pad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // XInput convention: +Y is up
  bool connected = false;
};

void Store16(uint8_t* p, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
void Store32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
uint16_t Load16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}

// XINPUT_GAMEPAD, 12 bytes, big-endian.
void WriteGamepad(uint8_t* p, const Pad& pad) {
  Store16(p, pad.buttons);
  p[2] = pad.left_trigger;
  p[3] = pad.right_trigger;
  Store16(p + 4, uint16_t(pad.lx));
  Store16(p + 6, uint16_t(pad.ly));
  Store16(p + 8, uint16_t(pad.rx));
  Store16(p + 10, uint16_t(pad.ry));
}

int16_t InvertAxis(int16_t v) { return v == INT16_MIN ? INT16_MAX : int16_t(-v); }
uint8_t TriggerToByte(int16_t v) { return uint8_t(std::clamp(int(v), 0, 32767) * 255 / 32767); }
int16_t PickStronger(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

std::mutex g_mutex;
SDL_Gamepad* g_gamepads[kUserCount] = {};

// Focus and mouse buttons from SDL events (delivered on the thread that pumps
// events, the main thread): SDL_GetKeyboardFocus / SDL_GetMouseState must not
// be called from guest threads.
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
      {SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight},
      {SDL_GAMEPAD_BUTTON_START, kStart},
      {SDL_GAMEPAD_BUTTON_BACK, kBack},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftThumb},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightThumb},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftShoulder},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightShoulder},
      {SDL_GAMEPAD_BUTTON_SOUTH, kA},
      {SDL_GAMEPAD_BUTTON_EAST, kB},
      {SDL_GAMEPAD_BUTTON_WEST, kX},
      {SDL_GAMEPAD_BUTTON_NORTH, kY},
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
  int key_count = 0;
  const bool* keys = SDL_GetKeyboardState(&key_count);
  if (!keys) return;
  auto down = [&](SDL_Scancode s) { return int(s) < key_count && keys[s]; };
  struct {
    SDL_Scancode key;
    uint16_t bit;
  } constexpr kMap[] = {
      {SDL_SCANCODE_SPACE, kA},       {SDL_SCANCODE_F, kB},          {SDL_SCANCODE_E, kX},
      {SDL_SCANCODE_Q, kY},           {SDL_SCANCODE_R, kRightShoulder},
      {SDL_SCANCODE_TAB, kLeftShoulder}, {SDL_SCANCODE_C, kLeftThumb},
      {SDL_SCANCODE_1, kDpadUp},      {SDL_SCANCODE_2, kDpadRight},  {SDL_SCANCODE_3, kDpadDown},
      {SDL_SCANCODE_4, kDpadLeft},    {SDL_SCANCODE_ESCAPE, kStart}, {SDL_SCANCODE_RETURN, kStart},
      {SDL_SCANCODE_BACKSPACE, kBack},
  };
  for (const auto& m : kMap) {
    if (down(m.key)) pad.buttons |= m.bit;
  }
  auto axis = [&](SDL_Scancode negative, SDL_Scancode positive) {
    return int16_t((down(positive) ? 32767 : 0) - (down(negative) ? 32767 : 0));
  };
  const int16_t lx = axis(SDL_SCANCODE_A, SDL_SCANCODE_D);
  const int16_t ly = axis(SDL_SCANCODE_S, SDL_SCANCODE_W);
  const int16_t rx = axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT);
  const int16_t ry = axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP);
  pad.lx = PickStronger(pad.lx, lx);
  pad.ly = PickStronger(pad.ly, ly);
  pad.rx = PickStronger(pad.rx, rx);
  pad.ry = PickStronger(pad.ry, ry);

  const uint32_t mouse = g_mouse_buttons.load(std::memory_order_relaxed);
  if (mouse & SDL_BUTTON_RMASK) pad.left_trigger = 255;      // aim
  if (mouse & SDL_BUTTON_LMASK) pad.right_trigger = 255;     // fire
  if (mouse & SDL_BUTTON_MMASK) pad.buttons |= kRightThumb;  // zoom
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

using RuntimeImport = void (*)(PPCContext&, uint8_t*);
RuntimeImport Original(const char* name) {
  auto function = reinterpret_cast<RuntimeImport>(dlsym(RTLD_NEXT, name));
  if (!function) REXLOG_ERROR("Native input: no runtime {} to fall back to", name);
  return function;
}

uint32_t UserIndex(uint32_t user_index, uint32_t flags) {
  return ((user_index & 0xFF) == 0xFF || (flags & kFlagAnyUser)) ? 0 : user_index;
}

uint32_t g_packet[kUserCount] = {};
Pad g_last[kUserCount];

}  // namespace

bool Gears1WindowFocused() {
  EnsureEventWatch();
  return g_focused.load(std::memory_order_relaxed);
}
uint32_t Gears1MouseButtons() { return g_mouse_buttons.load(std::memory_order_relaxed); }

// XamInputGetState(user_index, flags, XINPUT_STATE* state)
extern "C" REX_FUNC(__imp__XamInputGetState) {
  if (!REXCVAR_GET(gears1_native_input)) {
    static const RuntimeImport original = Original("__imp__XamInputGetState");
    if (original) original(ctx, base);
    return;
  }
  const uint32_t flags = ctx.r4.u32;
  const uint32_t state = ctx.r5.u32;
  const uint32_t user = UserIndex(ctx.r3.u32, flags);
  if (((flags & 0xFF) && !(flags & kFlagGamepad)) || user >= kUserCount) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  Pad pad;
  if (!ReadPad(user, pad)) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
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
    Store32(base + state, g_packet[user]);
    WriteGamepad(base + state + 4, pad);
  }
  ctx.r3.u64 = kErrorSuccess;
}

// XamInputGetCapabilities(user_index, flags, XINPUT_CAPABILITIES* caps)
extern "C" REX_FUNC(__imp__XamInputGetCapabilities) {
  if (!REXCVAR_GET(gears1_native_input)) {
    static const RuntimeImport original = Original("__imp__XamInputGetCapabilities");
    if (original) original(ctx, base);
    return;
  }
  const uint32_t flags = ctx.r4.u32;
  const uint32_t caps = ctx.r5.u32;
  const uint32_t user = UserIndex(ctx.r3.u32, flags);
  if (!caps) {
    ctx.r3.u64 = kErrorBadArguments;
    return;
  }
  Pad pad;
  if (((flags & 0xFF) && !(flags & kFlagGamepad)) || user >= kUserCount || !ReadPad(user, pad)) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  uint8_t* p = base + caps;
  std::memset(p, 0, 20);
  p[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
  p[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
  Pad full;  // all buttons and full axis ranges supported
  full.buttons = 0xF3FF;
  full.left_trigger = full.right_trigger = 0xFF;
  full.lx = full.ly = full.rx = full.ry = int16_t(0xFFC0);
  WriteGamepad(p + 4, full);
  Store16(p + 16, 0xFFFF);  // vibration motors
  Store16(p + 18, 0xFFFF);
  ctx.r3.u64 = kErrorSuccess;
}

// XamInputSetState(user_index, unknown, XINPUT_VIBRATION* vibration)
extern "C" REX_FUNC(__imp__XamInputSetState) {
  if (!REXCVAR_GET(gears1_native_input)) {
    static const RuntimeImport original = Original("__imp__XamInputSetState");
    if (original) original(ctx, base);
    return;
  }
  const uint32_t vibration = ctx.r5.u32;
  if (!vibration) {
    ctx.r3.u64 = kErrorBadArguments;
    return;
  }
  const uint32_t user = (ctx.r3.u32 & 0xFF) == 0xFF ? 0 : ctx.r3.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  SDL_Gamepad* gamepad = user < kUserCount ? g_gamepads[user] : nullptr;
  if (!gamepad) {
    // Keyboard-only player 1 accepts (and ignores) vibration.
    ctx.r3.u64 = user == 0 ? kErrorSuccess : kErrorDeviceNotConnected;
    return;
  }
  // Left motor is low frequency, right is high frequency; held until the
  // next call (the game stops it explicitly).
  SDL_RumbleGamepad(gamepad, Load16(base + vibration), Load16(base + vibration + 2), 0xFFFFFFFF);
  ctx.r3.u64 = kErrorSuccess;
}
