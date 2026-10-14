// Interfaces between the generated overrides (src/hooks_gen.cpp, from
// tools/gen_hooks.py and tools/hook_table.py) and the hand-written hooks:
// the probes (src/probes.cpp, PROBES.md), the shader hash cache
// (src/shader_dump.cpp) and the native input state (src/native_input.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

// Hook table kinds, described in tools/hook_table.py.
enum class Gears1ProbeKind : uint8_t {
  kHook,
  kPass,
  kScope,
  kPolicy,
  kDraw,
  kEvent,
  kFrame,
};

// Argument registers at entry, for post hooks.
struct Gears1HookEntry {
  uint32_t r3, r4, r5;
};

struct Gears1ProbeInfo {
  uint32_t address;
  const char* name;
  Gears1ProbeKind kind;
};

// Defined in src/hooks_gen.cpp.
extern const Gears1ProbeInfo kGears1Probes[];
extern const uint32_t kGears1ProbeCount;

// Called by the generated overrides. Enter returns true when the call must be
// skipped (gears1_probe_skip); the override then returns 0 in r3.
bool Gears1ProbeEnter(uint32_t id, PPCContext& ctx);
void Gears1ProbeLeave(uint32_t id);
void Gears1ProbePolicy(uint32_t id);
void Gears1ProbeDraw(uint32_t id, PPCContext& ctx, uint8_t* base);
void Gears1ProbeEvent(uint32_t id, PPCContext& ctx);
void Gears1ProbeFrameEnd();

// Whether an SDL scancode's key is down while the window has focus; safe on
// guest threads (src/native_input.cpp).
bool Gears1KeyDown(int scancode);

// Shader hashes (src/shader_dump.cpp): computed on bind while the dump or a
// probe trace wants them; 0 when unknown.
bool Gears1ProbeWantsShaderHashes();
uint64_t Gears1ShaderHash(uint32_t object);
