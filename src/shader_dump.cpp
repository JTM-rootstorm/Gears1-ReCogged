// Dump every distinct shader the game binds (NATIVE_RENDERER.md, phase 0).
//
// UE3 builds its shader objects without calling D3D's Create*Shader (the
// pixel-shader init is fully inlined), so shaders are captured when they are
// bound: pre hooks on SetVertexShader / SetPixelShader (tools/hook_table.py)
// call the functions below with r3 = device, r4 = shader object.
//
// Each file is the original Xbox shader container: the header (virtual part,
// copied into the shader object) followed by the microcode (physical part).
// The vertex-shader microcode is patched in place when D3D binds it to a
// vertex declaration, so one shader can produce several files; they are
// deduplicated by content.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "probes.h"

REXCVAR_DEFINE_BOOL(gears1_shader_dump, false, "Gears1",
                    "Write every distinct bound shader (header + microcode) to "
                    "gears1_shader_dump_dir");
REXCVAR_DEFINE_STRING(gears1_shader_dump_dir, "shader_dump", "Gears1",
                      "Output directory for gears1_shader_dump");

namespace {

// Shader object layout (from 822229D0, the VS init, and 825A44C8, UE3's
// inlined PS init): offset of the header copy and of the microcode pointer.
struct ShaderKind {
  const char* name;
  uint32_t header_offset;
  uint32_t microcode_pointer_offset;
  uint32_t vertex_flag;  // header flags bit 0: set for vertex shaders
};
constexpr ShaderKind kVertexShader = {"vs", 872, 32, 1};
constexpr ShaderKind kPixelShader = {"ps", 40, 24, 0};

// Header fields: +0 flags (0x102A11xx), +4 header size, +8 microcode size.
constexpr uint32_t kMagicMask = 0xFFFFFF00;
constexpr uint32_t kMagic = 0x102A1100;
constexpr uint32_t kMaxHeaderSize = 0x10000;
constexpr uint32_t kMaxMicrocodeSize = 0x40000;

std::mutex g_mutex;
struct ObjectState {
  uint64_t hash = 0;
  uint32_t binds = 0;
};
std::unordered_map<uint32_t, ObjectState> g_objects;
std::unordered_set<uint64_t> g_written;
FILE* g_index = nullptr;
bool g_failed = false;
uint32_t g_warnings = 0;

void OnSetShader(const ShaderKind& kind, uint8_t* base, uint32_t object) {
  const bool dump = REXCVAR_GET(gears1_shader_dump);
  if (!object || !(dump || Gears1ProbeWantsShaderHashes())) {
    return;
  }
  using rex::memory::GuestPtr;
  using rex::memory::load_and_swap;
  const uint8_t* header = GuestPtr(base, object + kind.header_offset);
  const uint32_t flags = load_and_swap<uint32_t>(header);
  const uint32_t header_size = load_and_swap<uint32_t>(header + 4);
  const uint32_t microcode_size = load_and_swap<uint32_t>(header + 8);
  const uint32_t microcode_address =
      load_and_swap<uint32_t>(GuestPtr(base, object + kind.microcode_pointer_offset));

  std::lock_guard<std::mutex> lock(g_mutex);
  // Hashing on every bind is too slow (thousands per frame). Re-check an
  // object on its first binds (declaration patching) and then every 256th
  // (objects freed and reused for another shader).
  ObjectState& object_state = g_objects[object];
  const uint32_t binds = object_state.binds++;
  if (binds >= 4 && (binds & 255)) {
    return;
  }
  if ((flags & kMagicMask) != kMagic || (flags & 1) != kind.vertex_flag || header_size < 12 ||
      header_size > kMaxHeaderSize || !microcode_size || microcode_size > kMaxMicrocodeSize ||
      !microcode_address) {
    if (g_warnings++ < 16) {
      REXLOG_WARN("Shader dump: {} object {:08X} not recognised (flags {:08X}, header {}, "
                  "microcode {} at {:08X})",
                  kind.name, object, flags, header_size, microcode_size, microcode_address);
    }
    return;
  }
  const uint8_t* microcode = GuestPtr(base, microcode_address);
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  XXH3_64bits_update(&state, header, header_size);
  XXH3_64bits_update(&state, microcode, microcode_size);
  const uint64_t hash = XXH3_64bits_digest(&state);

  object_state.hash = hash;
  if (!dump || g_failed || !g_written.insert(hash).second) {
    return;
  }

  const std::filesystem::path dir{std::string(REXCVAR_GET(gears1_shader_dump_dir))};
  if (!g_index) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_index = std::fopen((dir / "index.txt").string().c_str(), "a");
    if (!g_index) {
      REXLOG_ERROR("Shader dump: cannot write to {}", dir.string());
      g_failed = true;
      return;
    }
    REXLOG_INFO("Shader dump: writing to {}", dir.string());
  }
  char file_name[32];
  std::snprintf(file_name, sizeof(file_name), "%s_%016llx.bin", kind.name,
                static_cast<unsigned long long>(hash));
  if (FILE* file = std::fopen((dir / file_name).string().c_str(), "wb")) {
    std::fwrite(header, 1, header_size, file);
    std::fwrite(microcode, 1, microcode_size, file);
    std::fclose(file);
  }
  std::fprintf(g_index, "%s %016llx object=%08X microcode=%08X header_size=%u microcode_size=%u\n",
               kind.name, static_cast<unsigned long long>(hash), object, microcode_address,
               header_size, microcode_size);
  std::fflush(g_index);
}

}  // namespace

void Gears1OnSetVertexShader(PPCContext& ctx, uint8_t* base) {
  OnSetShader(kVertexShader, base, ctx.r4.u32);
}
void Gears1OnSetPixelShader(PPCContext& ctx, uint8_t* base) {
  OnSetShader(kPixelShader, base, ctx.r4.u32);
}

uint64_t Gears1ShaderHash(uint32_t object) {
  if (!object) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_objects.find(object);
  return it == g_objects.end() ? 0 : it->second.hash;
}
