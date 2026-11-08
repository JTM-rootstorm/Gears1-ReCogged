// Native renderer, phase 2: guest vertex and index buffers as SDL_GPU buffers.
//
// Captured at D3D DrawIndexedVertices and DrawVertices (pre hooks,
// tools/hook_table.py) from the device's bound state (NATIVE_RENDERER.md "D3D
// API map"):
// - the index buffer object: Common bit 31 = 32-bit indices, bits 29-30 =
//   endianness, +0x18 address, +0x1C size in bytes;
// - the vertex layout of the bound vertex shader (+0x3084). UE3 binds
//   shaders already patched for their vertex declaration and draws with no
//   declaration set (its RHI clears +0x2D10, so D3D doesn't patch), so the
//   layout is in the shader: the shader record (in the header copy at +872)
//   lists each vertex element's fetch instruction, usage and usage index, and
//   the patched fetch instruction (microcode pointer at +32) gives its fetch
//   constant, format, signedness, normalization, offset and stride;
// - each element's vertex fetch constant in the fetch constant shadow (with
//   SetStreamSource's offset applied).
// Buffers are byte-swapped by their endianness and uploaded when first drawn,
// then again whenever their contents change, and released after a while
// unused. Changes are found with the runtime's physical memory write watches
// (as the emulated GPU's shared memory finds them): a buffer is re-read, and
// re-uploaded if its hash changed, only after a write to one of its pages. The game's 8_8_8_8
// integer elements have no SDL_GPU format the translated shaders can read as float (no USCALED), so
// each gets a float4 copy in a buffer of its own. Vertex inputs are at the locations
// rexgpu-shader-tool --host-vertex-input gives each usage.
//
// The UP draws (BeginVertices / BeginIndexedVertices, post hooks) get their
// vertices and indices in command buffer memory that D3D returns and the
// caller fills afterwards (EndVertices is inlined), so they are read at the
// next draw or the end of the frame. D3D points vertex fetch constant 95
// (stream 0) at them, 8in32; indices are 8in16 or 8in32, offset by the base
// vertex index.
//
// The draw viewer lists the draws of one guest frame and renders the selected
// one in wireframe from its uploaded buffers and vertex layout.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mesh_preview.frag.h"
#include "mesh_preview.vert.h"
#include "native_renderer.h"
#include "probes.h"

namespace {

using rex::graphics::reg::PA_SU_SC_MODE_CNTL;
using rex::graphics::texture_conversion::CopySwapBlock;
using rex::graphics::xenos::Endian;
using rex::graphics::xenos::PrimitiveType;
using rex::graphics::xenos::VertexFormat;
using rex::graphics::xenos::xe_gpu_vertex_fetch_t;

// Device fields (NATIVE_RENDERER.md "Device layout" and register shadow).
constexpr uint32_t kDeviceFetchConstants = 0x400;
constexpr uint32_t kDeviceResetIndex = 0x2858;   // VGT_MULTI_PRIM_IB_RESET_INDX
constexpr uint32_t kDeviceModeControl = 0x28C8;  // PA_SU_SC_MODE_CNTL
constexpr uint32_t kDeviceIndexBuffer = 0x2F84;
constexpr uint32_t kDeviceVertexShader = 0x3084;
// Vertex shader object (src/shader_dump.cpp): header copy and microcode
// pointer. Header +0x18: offset of the shader record. Record +0 / +4: the
// program's offset in the microcode (after the literal constants) and size;
// vertex shader record +0x18 element table offset (dwords), +0x1C element
// count, entries from +0x24: bits 0:11 fetch instruction address (in 3-dword
// instructions from the program start), 12:15 usage, 16:19 usage index.
constexpr uint32_t kVertexShaderHeader = 872;
constexpr uint32_t kVertexShaderMicrocode = 32;
// Physical memory is mirrored at 0xA0000000 in the guest address space.
constexpr uint32_t kPhysicalMirror = 0xA0000000;
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kPageSizeLog2 = 12;  // host pages, the write watch granularity
// Host buffers unused for this many guest frames are released.
constexpr uint64_t kEvictFrames = 300;

// rexgpu-shader-tool's kVertexInputLocations (SDK local/shader-tools): the
// location of each D3DDECLUSAGE / usage index, as the translated vertex shaders
// read them.
struct VertexInputLocation {
  uint32_t usage, usage_index, location;
};
constexpr VertexInputLocation kVertexInputLocations[] = {
    {0, 0, 0},   // POSITION0
    {1, 0, 1},   // BLENDWEIGHT0
    {2, 0, 2},   // BLENDINDICES0
    {3, 0, 3},   // NORMAL0
    {6, 0, 4},   // TANGENT0
    {7, 0, 5},   // BINORMAL0
    {10, 0, 6},  // COLOR0
    {10, 1, 7},  // COLOR1
    {5, 0, 8},   // TEXCOORD0-7
    {5, 1, 9},  {5, 2, 10}, {5, 3, 11}, {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15},
};
constexpr const char* kLocationNames[] = {"P",  "BW", "BI", "N",  "T",  "B",  "C0", "C1",
                                          "T0", "T1", "T2", "T3", "T4", "T5", "T6", "T7"};

// A host buffer: the guest buffer's physical address and size in bytes, and
// which copy of it.
struct BufferKey {
  uint32_t address, size;
  uint32_t variant;  // kVertexBuffer, kIndexBuffer or ConvertedVariant()
  auto operator<=>(const BufferKey&) const = default;
};
constexpr uint32_t kVertexBuffer = 0, kIndexBuffer = 1;
// UP draw data, re-read on each draw.
constexpr uint32_t kUpVertexBuffer = 2, kUpIndexBuffer = 3;
// The vertex fetch constant D3D sets for UP draws.
constexpr uint32_t kUpFetchConstant = 95;
// The float4 copy of a vertex buffer's 8_8_8_8 integer element.
uint32_t ConvertedVariant(uint32_t stride, uint32_t offset) {
  return 1u << 31 | stride << 12 | offset;
}

struct Binding {
  BufferKey buffer;
  uint32_t stride;
};

struct Element {
  uint32_t location;
  uint32_t binding, offset;  // in the host buffers
  SDL_GPUVertexElementFormat format;
  // Where the guest data is, for reading it on the CPU.
  uint32_t guest_address, guest_stride, guest_offset;
  Endian endian;
  VertexFormat guest_format;
  bool integer;
};

struct DrawRecord {
  PrimitiveType primitive;
  bool indexed;
  bool up;
  // Indexed: indices start_index.. of the index buffer, each plus
  // index_offset; otherwise vertices start_index...
  uint32_t start_index, index_count;
  int32_t index_offset = 0;
  BufferKey index_buffer = {};
  bool index32 = false;
  bool reset = false;  // a strip with primitive reset indices in the drawn range
  // Vertices used (index_offset applied, reset indices excluded).
  uint32_t min_index = 0, max_index = 0;
  uint32_t vertex_shader;
  std::vector<Binding> bindings;
  std::vector<Element> elements;
  int position = -1;    // element of POSITION0
  std::string problem;  // why it can't be captured; empty if it can
};

struct IndexRange {
  uint32_t min_index, max_index;
  bool reset;
};

struct BufferState {
  uint64_t checked_frame = UINT64_MAX;
  uint32_t read_serial = 0;  // g_write_serial when last read
  uint64_t hash = 0;
  uint32_t version = 0;  // bumped whenever the contents change
};

struct HostState {
  uint32_t version = 0;  // of the guest buffer when last queued
  uint64_t used_frame = 0;
};

struct Upload {
  BufferKey key;
  SDL_GPUBufferUsageFlags usage;
  std::vector<uint8_t> data;
};

struct FrameStats {
  uint32_t draws = 0;
  uint64_t capture_ns = 0;
  uint64_t hash_ns = 0, hash_bytes = 0;  // re-reading written buffers
  uint64_t upload_bytes = 0;
  uint32_t uploads = 0;
  std::map<std::string, uint32_t> problems;  // reason -> draws
  std::array<uint32_t, 64> primitives = {};  // draws per Xenos primitive type
};

// A UP draw whose data the caller has not written yet.
struct PendingUp {
  size_t draw;  // in g_draws
  uint32_t vertex_bytes;
  uint32_t index_bytes;
  Endian index_endian;
  uint32_t reset_index;
};

// Write watches. The runtime calls OnPhysicalWrite on the first write to a
// watched page, on the writing thread and under its global lock (so it takes
// no locks of ours), then unprotects the pages: each write stamps its pages
// with a new serial.
std::atomic<uint32_t> g_write_serial{0};
std::atomic<uint32_t> g_page_serials[kPhysicalSize >> kPageSizeLog2];

std::mutex g_mutex;
uint8_t* g_base = nullptr;
void* g_watch_handle = nullptr;
// Guest thread (draw hook and frame end), under g_mutex.
uint64_t g_frame = 0;
std::unordered_map<BufferKey, BufferState, rex::XXHasher<BufferKey>> g_states;  // guest
std::unordered_map<BufferKey, HostState, rex::XXHasher<BufferKey>> g_hosts;  // host, queued
// Index range of each drawn range of an index buffer version.
struct RangeKey {
  BufferKey buffer;
  uint32_t version, start_index, index_count;
  bool operator==(const RangeKey&) const = default;
};
std::unordered_map<RangeKey, IndexRange, rex::XXHasher<RangeKey>> g_ranges;
// Fetch formats seen: format | signed << 6 | integer << 7.
std::array<bool, 256> g_formats = {};
std::vector<DrawRecord> g_draws;
std::vector<PendingUp> g_pending_up;
FrameStats g_stats;
// Published at the end of each guest frame, under g_mutex.
std::vector<DrawRecord> g_last_draws;
FrameStats g_last_stats;
std::vector<Upload> g_pending;
std::vector<BufferKey> g_evicted;
uint64_t g_total_upload_bytes = 0;

// Main thread only.
std::map<BufferKey, SDL_GPUBuffer*> g_buffers;

// Physical address of a guest virtual address, as D3D computes it for the GPU:
// the 0xE0000000 (4 KB page) range is 4 KB higher in physical memory.
uint32_t PhysicalAddress(uint32_t address) {
  return (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

bool InPhysicalMemory(const BufferKey& key) {
  return key.address < kPhysicalSize && key.size <= kPhysicalSize - key.address;
}

const uint8_t* GuestPhysical(uint32_t address) {
  return rex::memory::GuestPtr(g_base, kPhysicalMirror | address);
}

uint32_t InputLocation(uint32_t usage, uint32_t usage_index) {
  for (const VertexInputLocation& input : kVertexInputLocations) {
    if (input.usage == usage && input.usage_index == usage_index) {
      return input.location;
    }
  }
  return UINT32_MAX;
}

std::pair<uint32_t, uint32_t> OnPhysicalWrite(void*, uint32_t address, uint32_t length,
                                              bool exact_range) {
  if (!length || address >= kPhysicalSize) {
    return {0, UINT32_MAX};
  }
  length = std::min(length, kPhysicalSize - address);
  uint32_t page_first = address >> kPageSizeLog2;
  uint32_t page_last = (address + length - 1) >> kPageSizeLog2;
  // Like the emulated GPU, let a write unprotect up to 256 KB around it (far
  // fewer faults), all of which counts as written.
  if (!exact_range) {
    page_first &= ~63u;
    page_last |= 63u;
  }
  const uint32_t serial = g_write_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    g_page_serials[page].store(serial, std::memory_order_relaxed);
  }
  return {page_first << kPageSizeLog2, (page_last - page_first + 1) << kPageSizeLog2};
}

bool WrittenSince(const BufferKey& key, uint32_t serial) {
  const uint32_t page_last = (key.address + key.size - 1) >> kPageSizeLog2;
  for (uint32_t page = key.address >> kPageSizeLog2; page <= page_last; ++page) {
    if (g_page_serials[page].load(std::memory_order_relaxed) > serial) {
      return true;
    }
  }
  return false;
}

// The guest buffer's version, bumped when it is new or its contents changed
// (checked at most once per guest frame).
uint32_t Check(const BufferKey& key) {
  BufferState& state = g_states[key];
  if (state.checked_frame == g_frame) {
    return state.version;
  }
  state.checked_frame = g_frame;
  if (state.version && !WrittenSince(key, state.read_serial)) {
    return state.version;
  }
  // Watch the pages again (a write unprotected them), then read: a write from
  // here on is either read now or seen next time.
  state.read_serial = g_write_serial.load(std::memory_order_relaxed);
  rex::system::kernel_state()->memory()->EnablePhysicalMemoryAccessCallbacks(key.address,
                                                                            key.size, true, false);
  const uint64_t hash_start_ns = SDL_GetTicksNS();
  const uint64_t hash = XXH3_64bits(GuestPhysical(key.address), key.size);
  g_stats.hash_ns += SDL_GetTicksNS() - hash_start_ns;
  g_stats.hash_bytes += key.size;
  if (!state.version || hash != state.hash) {
    state.hash = hash;
    ++state.version;
  }
  return state.version;
}

// Queues the host buffer's upload unless this version of the guest buffer is
// already queued; fill writes the size bytes.
template <typename Fill>
void Queue(const BufferKey& key, uint32_t version, SDL_GPUBufferUsageFlags usage, uint32_t size,
           Fill fill) {
  HostState& host = g_hosts[key];
  host.used_frame = g_frame;
  if (host.version == version) {
    return;
  }
  host.version = version;
  Upload upload{key, usage, std::vector<uint8_t>(size)};
  fill(upload.data.data());
  ++g_stats.uploads;
  g_stats.upload_bytes += size;
  g_pending.push_back(std::move(upload));
}

// Queues a byte-swapped copy of the guest buffer.
void QueueSwapped(const BufferKey& key, uint32_t version, Endian endian,
                  SDL_GPUBufferUsageFlags usage) {
  Queue(key, version, usage, key.size,
        [&](uint8_t* out) { CopySwapBlock(endian, out, GuestPhysical(key.address), key.size); });
}

// Queues the float4 copy of an 8_8_8_8 integer element of the vertex buffer raw
// and returns its key.
BufferKey QueueConverted(const BufferKey& raw, uint32_t version, Endian endian, uint32_t stride,
                         uint32_t offset) {
  const BufferKey key = {raw.address, raw.size, ConvertedVariant(stride, offset)};
  const uint32_t vertices =
      raw.size >= offset + 4 && stride ? (raw.size - offset - 4) / stride + 1 : 0;
  Queue(key, version, SDL_GPU_BUFFERUSAGE_VERTEX, vertices * 16, [&](uint8_t* out) {
    const uint8_t* guest = GuestPhysical(raw.address) + offset;
    for (uint32_t v = 0; v < vertices; ++v) {
      uint8_t bytes[4];
      CopySwapBlock(endian, bytes, guest + size_t(v) * stride, 4);
      const float values[4] = {float(bytes[0]), float(bytes[1]), float(bytes[2]), float(bytes[3])};
      std::memcpy(out + size_t(v) * 16, values, 16);
    }
  });
  return key;
}

// The primitive reset index of a strip draw, or UINT32_MAX when reset is off.
uint32_t ResetIndex(const uint8_t* device, const DrawRecord& draw) {
  PA_SU_SC_MODE_CNTL mode_control;
  mode_control.value = rex::memory::load_and_swap<uint32_t>(device + kDeviceModeControl);
  const bool strip = draw.primitive == PrimitiveType::kTriangleStrip ||
                     draw.primitive == PrimitiveType::kLineStrip;
  if (!strip || !mode_control.multi_prim_ib_ena) {
    return UINT32_MAX;
  }
  return rex::memory::load_and_swap<uint32_t>(device + kDeviceResetIndex) &
         (draw.index32 ? 0xFFFFFF : 0xFFFF);
}

IndexRange ScanIndices(const DrawRecord& draw, Endian endian, uint32_t reset_index) {
  const uint32_t index_bytes = draw.index32 ? 4 : 2;
  std::vector<uint8_t> swapped(size_t(draw.index_count) * index_bytes);
  CopySwapBlock(endian, swapped.data(),
                GuestPhysical(draw.index_buffer.address) + size_t(draw.start_index) * index_bytes,
                swapped.size());
  IndexRange range = {UINT32_MAX, 0, false};
  for (uint32_t i = 0; i < draw.index_count; ++i) {
    uint32_t index;
    if (draw.index32) {
      std::memcpy(&index, swapped.data() + 4 * i, 4);
    } else {
      uint16_t index16;
      std::memcpy(&index16, swapped.data() + 2 * i, 2);
      index = index16;
    }
    if (index == reset_index) {
      range.reset = true;
      continue;
    }
    range.min_index = std::min(range.min_index, index);
    range.max_index = std::max(range.max_index, index);
  }
  return range;
}

void ScanRange(DrawRecord& draw, uint32_t version, Endian endian, uint32_t reset_index);

void CaptureIndices(const uint8_t* device, DrawRecord& draw) {
  const uint32_t object = rex::memory::load_and_swap<uint32_t>(device + kDeviceIndexBuffer);
  if (!object) {
    draw.problem = "no index buffer";
    return;
  }
  const uint8_t* index_buffer = rex::memory::GuestPtr(g_base, object);
  const uint32_t common = rex::memory::load_and_swap<uint32_t>(index_buffer);
  const Endian endian = Endian((common >> 29) & 3);
  draw.index32 = common >> 31;
  draw.index_buffer = {
      PhysicalAddress(rex::memory::load_and_swap<uint32_t>(index_buffer + 0x18)),
      rex::memory::load_and_swap<uint32_t>(index_buffer + 0x1C), kIndexBuffer};
  const uint32_t index_bytes = draw.index32 ? 4 : 2;
  if (!InPhysicalMemory(draw.index_buffer)) {
    draw.problem = "index buffer outside physical memory";
    return;
  }
  if (uint64_t(draw.start_index + draw.index_count) * index_bytes > draw.index_buffer.size) {
    draw.problem = "indices outside the index buffer";
    return;
  }
  const uint32_t version = Check(draw.index_buffer);
  QueueSwapped(draw.index_buffer, version, endian, SDL_GPU_BUFFERUSAGE_INDEX);
  ScanRange(draw, version, endian, ResetIndex(device, draw));
}

// The drawn range's vertices, cached per index buffer version.
void ScanRange(DrawRecord& draw, uint32_t version, Endian endian, uint32_t reset_index) {
  if (g_ranges.size() > (1 << 16)) {
    g_ranges.clear();
  }
  auto [it, inserted] = g_ranges.try_emplace(
      RangeKey{draw.index_buffer, version, draw.start_index, draw.index_count});
  if (inserted) {
    it->second = ScanIndices(draw, endian, reset_index);
  }
  draw.min_index = it->second.min_index + draw.index_offset;
  draw.max_index = it->second.max_index + draw.index_offset;
  draw.reset = it->second.reset;
}

// up_vertices: the UP draw's vertices, which fetch constant 95 reads; they are
// queued once the caller has written them (FinishUpDraws).
void CaptureVertices(const uint8_t* device, DrawRecord& draw,
                     const BufferKey* up_vertices = nullptr) {
  using rex::memory::load_and_swap;
  draw.vertex_shader = load_and_swap<uint32_t>(device + kDeviceVertexShader);
  if (!draw.vertex_shader) {
    draw.problem = "no vertex shader";
    return;
  }
  const uint8_t* shader = rex::memory::GuestPtr(g_base, draw.vertex_shader);
  const uint8_t* header = shader + kVertexShaderHeader;
  const uint8_t* record = header + load_and_swap<uint32_t>(header + 0x18);
  const uint32_t table_offset = load_and_swap<uint32_t>(record + 0x18);
  const uint32_t element_count = load_and_swap<uint32_t>(record + 0x1C);
  const uint32_t program_offset = load_and_swap<uint32_t>(record);
  const uint32_t program_size = load_and_swap<uint32_t>(record + 4);
  if (uint64_t(program_offset) + program_size > load_and_swap<uint32_t>(header + 8)) {
    draw.problem = "bad vertex shader program range";
    return;
  }
  const uint8_t* program =
      rex::memory::GuestPtr(g_base, load_and_swap<uint32_t>(shader + kVertexShaderMicrocode)) +
      program_offset;
  std::array<int, 96> constant_bindings;
  constant_bindings.fill(-1);
  std::array<xe_gpu_vertex_fetch_t, 96> constant_fetches;
  std::array<uint32_t, 96> constant_versions;
  // A mini fetch uses the stride and fetch constant of the full fetch before it.
  uint32_t full_constant = 0, full_stride = 0;
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t entry = load_and_swap<uint32_t>(record + 0x24 + 4 * (table_offset + i));
    const uint32_t address = entry & 0xFFF, usage = (entry >> 12) & 0xF;
    const uint32_t usage_index = (entry >> 16) & 0xF;
    if ((address + 1) * 12 > program_size) {
      draw.problem = "fetch outside the program";
      return;
    }
    uint32_t words[3];
    for (uint32_t w = 0; w < 3; ++w) {
      words[w] = load_and_swap<uint32_t>(program + address * 12 + 4 * w);
    }
    rex::graphics::ucode::VertexFetchInstruction fetch_instruction;
    std::memcpy(&fetch_instruction, words, sizeof(words));
    if (!fetch_instruction.is_mini_fetch()) {
      full_constant = fetch_instruction.fetch_constant_index();
      full_stride = fetch_instruction.stride() * 4;
    }
    const uint32_t constant = full_constant, stride = full_stride;
    const VertexFormat format = fetch_instruction.data_format();
    // Unpatched fetches have no format (0).
    if (format == VertexFormat(0)) {
      draw.problem = "unpatched vertex fetch";
      return;
    }
    if (fetch_instruction.offset() < 0 || uint32_t(fetch_instruction.offset()) >= 256) {
      draw.problem = "vertex fetch offset " + std::to_string(fetch_instruction.offset());
      return;
    }
    const uint32_t offset = uint32_t(fetch_instruction.offset()) * 4;
    const bool is_signed = fetch_instruction.is_signed();
    const bool integer = !fetch_instruction.is_normalized();
    bool& seen = g_formats[uint32_t(format) | uint32_t(is_signed) << 6 | uint32_t(integer) << 7];
    if (!seen) {
      seen = true;
      REXLOG_INFO("Native buffers: vertex fetch format {} (signed {}, integer {})",
                  uint32_t(format), is_signed, integer);
    }
    Element element;
    element.location = InputLocation(usage, usage_index);
    if (element.location == UINT32_MAX) {
      draw.problem = "usage " + std::to_string(usage) + "/" + std::to_string(usage_index);
      return;
    }
    if (std::any_of(draw.elements.begin(), draw.elements.end(),
                    [&](const Element& other) { return other.location == element.location; })) {
      continue;
    }
    if (constant_bindings[constant] < 0 && up_vertices) {
      if (constant != kUpFetchConstant) {
        draw.problem = "UP draw fetches constant " + std::to_string(constant);
        return;
      }
      constant_fetches[constant].endian = Endian::k8in32;
      constant_bindings[constant] = int(draw.bindings.size());
      draw.bindings.push_back({*up_vertices, stride});
    } else if (constant_bindings[constant] < 0) {
      const uint8_t* constant_words = device + kDeviceFetchConstants + constant * 8;
      xe_gpu_vertex_fetch_t& fetch = constant_fetches[constant];
      fetch.dword_0 = load_and_swap<uint32_t>(constant_words);
      fetch.dword_1 = load_and_swap<uint32_t>(constant_words + 4);
      const BufferKey key = {uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2,
                             kVertexBuffer};
      if (!key.size) {
        draw.problem = "empty vertex buffer";
        return;
      }
      if (!InPhysicalMemory(key)) {
        draw.problem = "vertex buffer outside physical memory";
        return;
      }
      constant_versions[constant] = Check(key);
      QueueSwapped(key, constant_versions[constant], fetch.endian, SDL_GPU_BUFFERUSAGE_VERTEX);
      constant_bindings[constant] = int(draw.bindings.size());
      draw.bindings.push_back({key, stride});
    }
    const Binding& binding = draw.bindings[constant_bindings[constant]];
    element.binding = constant_bindings[constant];
    element.offset = offset;
    element.guest_address = binding.buffer.address;
    element.guest_stride = stride;
    element.guest_offset = offset;
    element.endian = constant_fetches[constant].endian;
    element.guest_format = format;
    element.integer = integer;
    switch (format) {
      case VertexFormat::k_32_FLOAT:
        element.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        break;
      case VertexFormat::k_32_32_FLOAT:
        element.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        break;
      case VertexFormat::k_32_32_32_FLOAT:
        element.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        break;
      case VertexFormat::k_32_32_32_32_FLOAT:
        element.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        break;
      case VertexFormat::k_8_8_8_8:
        if (is_signed) {
          draw.problem = "signed 8_8_8_8";
          return;
        }
        element.format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
        if (integer) {
          const BufferKey& raw = binding.buffer;
          const BufferKey key =
              up_vertices ? BufferKey{raw.address, raw.size, ConvertedVariant(stride, offset)}
                          : QueueConverted(raw, constant_versions[constant], element.endian, stride,
                                           offset);
          element.binding = uint32_t(draw.bindings.size());
          element.offset = 0;
          element.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
          draw.bindings.push_back({key, 16});
        }
        break;
      default:
        draw.problem = "vertex format " + std::to_string(uint32_t(format));
        return;
    }
    if (element.location == 0) {
      draw.position = int(draw.elements.size());
    }
    draw.elements.push_back(element);
  }
}

// Reads the UP draws' data, which their callers have written by now.
void FinishUpDraws() {
  for (const PendingUp& pending : g_pending_up) {
    DrawRecord& draw = g_draws[pending.draw];
    if (!draw.problem.empty()) {
      continue;
    }
    const BufferKey& vertices = draw.bindings[0].buffer;
    const uint32_t version =
        uint32_t(XXH3_64bits(GuestPhysical(vertices.address), vertices.size)) | 1;
    QueueSwapped(vertices, version, Endian::k8in32, SDL_GPU_BUFFERUSAGE_VERTEX);
    for (const Element& element : draw.elements) {
      const BufferKey& key = draw.bindings[element.binding].buffer;
      if (key.variant >> 31) {
        QueueConverted(vertices, version, Endian::k8in32, element.guest_stride,
                       element.guest_offset);
      }
    }
    if (draw.indexed) {
      const uint32_t index_version =
          uint32_t(XXH3_64bits(GuestPhysical(draw.index_buffer.address), draw.index_buffer.size)) |
          1;
      QueueSwapped(draw.index_buffer, index_version, pending.index_endian,
                   SDL_GPU_BUFFERUSAGE_INDEX);
      ScanRange(draw, index_version, pending.index_endian, pending.reset_index);
    }
  }
  g_pending_up.clear();
}

// Starts capturing a draw: under g_mutex, after the previous UP draws' data
// has been read.
void BeginCapture(uint8_t* base) {
  g_base = base;
  if (!g_watch_handle) {
    g_watch_handle =
        rex::system::kernel_state()->memory()->RegisterPhysicalMemoryInvalidationCallback(
            OnPhysicalWrite, nullptr);
  }
  FinishUpDraws();
}

void AddDraw(DrawRecord&& draw, uint64_t start_ns) {
  ++g_stats.draws;
  ++g_stats.primitives[uint32_t(draw.primitive) & 63];
  if (!draw.problem.empty()) {
    ++g_stats.problems[draw.problem];
  }
  g_draws.push_back(std::move(draw));
  g_stats.capture_ns += SDL_GetTicksNS() - start_ns;
}

// Main thread: the selected draw of a captured frame, and its wireframe
// preview.
std::vector<DrawRecord> g_captured;
int g_selected = -1;
bool g_have_bounds = false;
float g_bounds_min[3], g_bounds_max[3];
SDL_GPUShader* g_preview_vertex_shader = nullptr;
SDL_GPUShader* g_preview_fragment_shader = nullptr;
SDL_GPUTexture* g_preview_target = nullptr;
std::map<std::string, SDL_GPUGraphicsPipeline*> g_preview_pipelines;
std::string g_preview_status;
constexpr uint32_t kPreviewSize = 512;

const char* PrimitiveName(PrimitiveType primitive) {
  switch (primitive) {
    case PrimitiveType::kPointList:
      return "PointList";
    case PrimitiveType::kLineList:
      return "LineList";
    case PrimitiveType::kLineStrip:
      return "LineStrip";
    case PrimitiveType::kTriangleList:
      return "TriangleList";
    case PrimitiveType::kTriangleFan:
      return "TriangleFan";
    case PrimitiveType::kTriangleStrip:
      return "TriangleStrip";
    case PrimitiveType::kRectangleList:
      return "RectangleList";
    case PrimitiveType::kQuadList:
      return "QuadList";
    default:
      return "other";
  }
}

bool ReadPosition(const DrawRecord& draw, uint32_t vertex, float out[3]) {
  const Element& element = draw.elements[draw.position];
  const uint32_t offset = vertex * element.guest_stride + element.guest_offset;
  const uint32_t size = draw.bindings[element.binding].buffer.size;
  if (offset + 4 > size) {
    return false;
  }
  const uint8_t* guest = GuestPhysical(element.guest_address) + offset;
  out[0] = out[1] = out[2] = 0.0f;
  if (element.guest_format == VertexFormat::k_8_8_8_8) {
    uint8_t bytes[4];
    CopySwapBlock(element.endian, bytes, guest, 4);
    for (int i = 0; i < 3; ++i) {
      out[i] = element.integer ? float(bytes[i]) : bytes[i] / 255.0f;
    }
    return true;
  }
  const int components = rex::graphics::xenos::GetVertexFormatComponentCount(element.guest_format);
  if (offset + 4 * components > size) {
    return false;
  }
  float values[4] = {};
  CopySwapBlock(element.endian, values, guest, 4 * components);
  std::memcpy(out, values, sizeof(float) * std::min(components, 3));
  return true;
}

void ComputeBounds(const DrawRecord& draw) {
  g_have_bounds = false;
  if (draw.position < 0 || draw.min_index > draw.max_index) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    g_bounds_min[i] = INFINITY;
    g_bounds_max[i] = -INFINITY;
  }
  for (uint32_t vertex = draw.min_index; vertex <= draw.max_index; ++vertex) {
    float position[3];
    if (!ReadPosition(draw, vertex, position)) {
      return;
    }
    for (int i = 0; i < 3; ++i) {
      g_bounds_min[i] = std::min(g_bounds_min[i], position[i]);
      g_bounds_max[i] = std::max(g_bounds_max[i], position[i]);
    }
  }
  g_have_bounds = true;
}

// Column-major 4x4 matrices, as GLSL reads them.
struct Matrix {
  float m[16];
};

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix r = {};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      for (int k = 0; k < 4; ++k) {
        r.m[column * 4 + row] += a.m[k * 4 + row] * b.m[column * 4 + k];
      }
    }
  }
  return r;
}

// Right-handed view looking from eye at center, z up (UE3's world is z up).
Matrix LookAt(const float eye[3], const float center[3]) {
  float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
  const float f_length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (float& c : f) {
    c /= f_length;
  }
  // s = normalize(cross(f, up)), u = cross(s, f), up = (0, 0, 1).
  float s[3] = {f[1], -f[0], 0.0f};
  const float s_length = std::max(std::sqrt(s[0] * s[0] + s[1] * s[1]), 1e-6f);
  s[0] /= s_length;
  s[1] /= s_length;
  const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                      s[0] * f[1] - s[1] * f[0]};
  Matrix r = {};
  for (int i = 0; i < 3; ++i) {
    r.m[i * 4 + 0] = s[i];
    r.m[i * 4 + 1] = u[i];
    r.m[i * 4 + 2] = -f[i];
  }
  r.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  r.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  r.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
  r.m[15] = 1.0f;
  return r;
}

// Right-handed perspective projection to depth 0..1.
Matrix Perspective(float fov_y, float aspect, float near_z, float far_z) {
  const float f = 1.0f / std::tan(fov_y / 2.0f);
  Matrix r = {};
  r.m[0] = f / aspect;
  r.m[5] = f;
  r.m[10] = far_z / (near_z - far_z);
  r.m[11] = -1.0f;
  r.m[14] = near_z * far_z / (near_z - far_z);
  return r;
}

bool PreviewTopology(const DrawRecord& draw, SDL_GPUPrimitiveType& topology) {
  switch (draw.primitive) {
    case PrimitiveType::kPointList:
      topology = SDL_GPU_PRIMITIVETYPE_POINTLIST;
      return true;
    case PrimitiveType::kLineList:
      topology = SDL_GPU_PRIMITIVETYPE_LINELIST;
      return true;
    case PrimitiveType::kLineStrip:
      topology = SDL_GPU_PRIMITIVETYPE_LINESTRIP;
      return !draw.reset;
    case PrimitiveType::kTriangleList:
      topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
      return true;
    case PrimitiveType::kTriangleStrip:
      // SDL_GPU (Vulkan) never enables primitive restart.
      topology = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
      return !draw.reset;
    default:
      return false;
  }
}

SDL_GPUGraphicsPipeline* PreviewPipeline(SDL_GPUDevice* device, const DrawRecord& draw,
                                         SDL_GPUPrimitiveType topology) {
  std::string key(1, char(topology));
  for (const Binding& binding : draw.bindings) {
    key += "b" + std::to_string(binding.stride);
  }
  for (const Element& element : draw.elements) {
    key += "e" + std::to_string(element.location) + "," + std::to_string(element.binding) + "," +
           std::to_string(element.offset) + "," + std::to_string(element.format);
  }
  auto [it, inserted] = g_preview_pipelines.try_emplace(key, nullptr);
  if (!inserted) {
    return it->second;
  }
  std::vector<SDL_GPUVertexBufferDescription> buffers(draw.bindings.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    buffers[i].slot = uint32_t(i);
    buffers[i].pitch = draw.bindings[i].stride;
    buffers[i].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  }
  std::vector<SDL_GPUVertexAttribute> attributes(draw.elements.size());
  for (size_t i = 0; i < attributes.size(); ++i) {
    attributes[i].location = draw.elements[i].location;
    attributes[i].buffer_slot = draw.elements[i].binding;
    attributes[i].format = draw.elements[i].format;
    attributes[i].offset = draw.elements[i].offset;
  }
  SDL_GPUColorTargetDescription target = {};
  target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = g_preview_vertex_shader;
  info.fragment_shader = g_preview_fragment_shader;
  info.vertex_input_state.vertex_buffer_descriptions = buffers.data();
  info.vertex_input_state.num_vertex_buffers = uint32_t(buffers.size());
  info.vertex_input_state.vertex_attributes = attributes.data();
  info.vertex_input_state.num_vertex_attributes = uint32_t(attributes.size());
  info.primitive_type = topology;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.target_info.color_target_descriptions = &target;
  info.target_info.num_color_targets = 1;
  it->second = SDL_CreateGPUGraphicsPipeline(device, &info);
  if (!it->second) {
    REXLOG_ERROR("Native buffers: preview pipeline failed: {}", SDL_GetError());
  }
  return it->second;
}

bool CreatePreviewResources(SDL_GPUDevice* device) {
  if (g_preview_target) {
    return true;
  }
  SDL_GPUShaderCreateInfo shader = {};
  shader.format = SDL_GPU_SHADERFORMAT_SPIRV;
  shader.entrypoint = "main";
  shader.code = reinterpret_cast<const Uint8*>(mesh_preview_vert);
  shader.code_size = sizeof(mesh_preview_vert);
  shader.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  shader.num_uniform_buffers = 1;
  g_preview_vertex_shader = SDL_CreateGPUShader(device, &shader);
  shader.code = reinterpret_cast<const Uint8*>(mesh_preview_frag);
  shader.code_size = sizeof(mesh_preview_frag);
  shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  shader.num_uniform_buffers = 0;
  g_preview_fragment_shader = SDL_CreateGPUShader(device, &shader);
  SDL_GPUTextureCreateInfo texture = {};
  texture.type = SDL_GPU_TEXTURETYPE_2D;
  texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texture.width = texture.height = kPreviewSize;
  texture.layer_count_or_depth = 1;
  texture.num_levels = 1;
  g_preview_target = SDL_CreateGPUTexture(device, &texture);
  if (!g_preview_vertex_shader || !g_preview_fragment_shader || !g_preview_target) {
    REXLOG_ERROR("Native buffers: preview resources failed: {}", SDL_GetError());
    return false;
  }
  return true;
}

}  // namespace

// Pre hook of D3D DrawIndexedVertices (8222DE50): r3 = device, r4 = primitive
// type (Xenos numbering), r6 = start index, r7 = index count (r5,
// BaseVertexIndex, is unused by D3D).
void Gears1NativeBuffersOnDrawIndexed(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  const uint64_t start_ns = SDL_GetTicksNS();
  DrawRecord draw;
  draw.primitive = PrimitiveType(ctx.r4.u32);
  draw.indexed = true;
  draw.up = false;
  draw.start_index = ctx.r6.u32;
  draw.index_count = ctx.r7.u32;
  const uint8_t* device = rex::memory::GuestPtr(base, ctx.r3.u32);
  std::lock_guard<std::mutex> lock(g_mutex);
  BeginCapture(base);
  CaptureIndices(device, draw);
  if (draw.problem.empty()) {
    CaptureVertices(device, draw);
  }
  AddDraw(std::move(draw), start_ns);
}

// Pre hook of D3D DrawVertices (8222DA48): r3 = device, r4 = primitive type,
// r5 = start vertex, r6 = vertex count.
void Gears1NativeBuffersOnDraw(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer)) {
    return;
  }
  const uint64_t start_ns = SDL_GetTicksNS();
  DrawRecord draw;
  draw.primitive = PrimitiveType(ctx.r4.u32);
  draw.indexed = false;
  draw.up = false;
  draw.start_index = ctx.r5.u32;
  draw.index_count = ctx.r6.u32;
  draw.min_index = draw.start_index;
  draw.max_index = draw.start_index + draw.index_count - 1;
  std::lock_guard<std::mutex> lock(g_mutex);
  BeginCapture(base);
  CaptureVertices(rex::memory::GuestPtr(base, ctx.r3.u32), draw);
  AddDraw(std::move(draw), start_ns);
}

// Post hook of D3D BeginVertices (8222CFF8): r3 = device, r4 = primitive
// type, r5 = vertex count, r6 = stride; returns the vertex data's address, or
// 0.
void Gears1NativeBuffersAfterBeginVertices(const Gears1HookEntry& entry, PPCContext& ctx,
                                           uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer) || !ctx.r3.u32) {
    return;
  }
  const uint64_t start_ns = SDL_GetTicksNS();
  DrawRecord draw;
  draw.primitive = PrimitiveType(entry.r4);
  draw.indexed = false;
  draw.up = true;
  draw.start_index = 0;
  draw.index_count = entry.r5;
  draw.min_index = 0;
  draw.max_index = entry.r5 - 1;
  const BufferKey vertices = {PhysicalAddress(ctx.r3.u32), entry.r5 * entry.r6, kUpVertexBuffer};
  std::lock_guard<std::mutex> lock(g_mutex);
  BeginCapture(base);
  CaptureVertices(rex::memory::GuestPtr(base, entry.r3), draw, &vertices);
  g_pending_up.push_back({g_draws.size(), vertices.size, 0, Endian::kNone, UINT32_MAX});
  AddDraw(std::move(draw), start_ns);
}

// Post hook of D3D BeginIndexedVertices (8222D4F8): r3 = device, r4 =
// primitive type, r5 = base vertex index, r6 = vertex count, r7 = index count,
// r8 = index format (bit 2: 32-bit), r9 = stride, r10 = where it stores the
// index data's address, the ninth argument slot = where it stores the vertex
// data's address; returns 0 on success.
void Gears1NativeBuffersAfterBeginIndexedVertices(const Gears1HookEntry& entry, PPCContext& ctx,
                                                  uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer) || ctx.r3.u32) {
    return;
  }
  using rex::memory::load_and_swap;
  const uint64_t start_ns = SDL_GetTicksNS();
  const uint8_t* device = rex::memory::GuestPtr(base, entry.r3);
  DrawRecord draw;
  draw.primitive = PrimitiveType(entry.r4);
  draw.indexed = true;
  draw.up = true;
  draw.start_index = 0;
  draw.index_count = entry.r7;
  draw.index_offset = int32_t(entry.r5);
  draw.index32 = entry.r8 & 4;
  const uint32_t index_data = load_and_swap<uint32_t>(rex::memory::GuestPtr(base, entry.r10));
  const uint32_t vertex_data = load_and_swap<uint32_t>(rex::memory::GuestPtr(
      base, load_and_swap<uint32_t>(rex::memory::GuestPtr(base, entry.r1 + 0x54))));
  draw.index_buffer = {PhysicalAddress(index_data), entry.r7 * (draw.index32 ? 4 : 2),
                       kUpIndexBuffer};
  const BufferKey vertices = {PhysicalAddress(vertex_data), entry.r6 * entry.r9, kUpVertexBuffer};
  std::lock_guard<std::mutex> lock(g_mutex);
  BeginCapture(base);
  CaptureVertices(device, draw, &vertices);
  g_pending_up.push_back({g_draws.size(), vertices.size, draw.index_buffer.size,
                          draw.index32 ? Endian::k8in32 : Endian::k8in16,
                          ResetIndex(device, draw)});
  AddDraw(std::move(draw), start_ns);
}

void NativeBuffersEndFrame() {
  std::lock_guard<std::mutex> lock(g_mutex);
  FinishUpDraws();
  g_last_draws.swap(g_draws);
  g_draws.clear();
  g_total_upload_bytes += g_stats.upload_bytes;
  if (g_frame % 600 == 0) {
    uint32_t up = 0, indexed = 0;
    for (const DrawRecord& draw : g_last_draws) {
      up += draw.up;
      indexed += draw.indexed;
    }
    std::string problems;
    for (const auto& [problem, count] : g_stats.problems) {
      problems += ", " + problem + ": " + std::to_string(count);
    }
    REXLOG_INFO("Native buffers: frame {}: {} draws ({} indexed, {} UP){}", g_frame,
                g_last_draws.size(), indexed, up, problems);
  }
  g_last_stats = std::move(g_stats);
  g_stats = {};
  ++g_frame;
  if (g_frame % 60) {
    return;
  }
  std::erase_if(g_states, [](const auto& entry) {
    return g_frame - entry.second.checked_frame > kEvictFrames;
  });
  std::erase_if(g_hosts, [](const auto& entry) {
    if (g_frame - entry.second.used_frame <= kEvictFrames) {
      return false;
    }
    g_evicted.push_back(entry.first);
    return true;
  });
}

void NativeBuffersUpload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer) {
  std::vector<Upload> pending;
  std::vector<BufferKey> evicted;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pending.swap(g_pending);
    evicted.swap(g_evicted);
  }
  for (const BufferKey& key : evicted) {
    auto it = g_buffers.find(key);
    if (it != g_buffers.end()) {
      SDL_ReleaseGPUBuffer(device, it->second);
      g_buffers.erase(it);
    }
  }
  if (pending.empty()) {
    return;
  }
  size_t total = 0;
  for (const Upload& upload : pending) {
    total += (upload.data.size() + 15) & ~size_t(15);
  }
  SDL_GPUTransferBufferCreateInfo transfer_info = {};
  transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer_info.size = uint32_t(total);
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
  if (!transfer) {
    REXLOG_ERROR("Native buffers: no {}-byte transfer buffer: {}", total, SDL_GetError());
    return;
  }
  auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, transfer, false));
  SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
  uint32_t offset = 0;
  for (const Upload& upload : pending) {
    if (upload.data.empty()) {
      continue;
    }
    SDL_GPUBufferCreateInfo create_info = {};
    create_info.usage = upload.usage;
    create_info.size = uint32_t(upload.data.size());
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &create_info);
    if (!buffer) {
      REXLOG_WARN("Native buffers: {}-byte buffer not created: {}", upload.data.size(),
                  SDL_GetError());
      continue;
    }
    std::memcpy(mapped + offset, upload.data.data(), upload.data.size());
    SDL_GPUTransferBufferLocation source = {transfer, offset};
    SDL_GPUBufferRegion destination = {buffer, 0, create_info.size};
    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
    offset += (create_info.size + 15) & ~15u;
    SDL_GPUBuffer*& slot = g_buffers[upload.key];
    if (slot) {
      SDL_ReleaseGPUBuffer(device, slot);
    }
    slot = buffer;
  }
  SDL_EndGPUCopyPass(copy_pass);
  SDL_UnmapGPUTransferBuffer(device, transfer);
  SDL_ReleaseGPUTransferBuffer(device, transfer);
}

void NativeBuffersRenderPreview(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer) {
  g_preview_status.clear();
  if (g_selected < 0 || g_selected >= int(g_captured.size())) {
    return;
  }
  const DrawRecord& draw = g_captured[g_selected];
  SDL_GPUPrimitiveType topology;
  if (!draw.problem.empty() || draw.position < 0 || !PreviewTopology(draw, topology)) {
    g_preview_status = "not previewable";
    return;
  }
  std::vector<SDL_GPUBufferBinding> vertex_buffers;
  for (const Binding& binding : draw.bindings) {
    auto it = g_buffers.find(binding.buffer);
    if (it == g_buffers.end()) {
      g_preview_status = "vertex buffer released";
      return;
    }
    vertex_buffers.push_back({it->second, 0});
  }
  auto index_buffer = g_buffers.find(draw.index_buffer);
  if (draw.indexed && index_buffer == g_buffers.end()) {
    g_preview_status = "index buffer released";
    return;
  }
  if (!CreatePreviewResources(device)) {
    return;
  }
  SDL_GPUGraphicsPipeline* pipeline = PreviewPipeline(device, draw, topology);
  if (!pipeline) {
    g_preview_status = "no pipeline";
    return;
  }

  // Orbit the drawn vertices' bounding box.
  float center[3] = {}, radius = 1.0f;
  if (g_have_bounds) {
    float extent = 0.0f;
    for (int i = 0; i < 3; ++i) {
      center[i] = (g_bounds_min[i] + g_bounds_max[i]) / 2.0f;
      extent += (g_bounds_max[i] - g_bounds_min[i]) * (g_bounds_max[i] - g_bounds_min[i]);
    }
    radius = std::max(std::sqrt(extent) / 2.0f, 1e-3f);
  }
  const float angle = float(SDL_GetTicks() % 20000) / 20000.0f * 6.2831853f;
  const float distance = radius * 2.5f;
  const float eye[3] = {center[0] + distance * std::cos(angle) * 0.85f,
                        center[1] + distance * std::sin(angle) * 0.85f,
                        center[2] + distance * 0.5f};
  const Matrix clip_from_object = Multiply(
      Perspective(0.8f, 1.0f, radius * 0.05f, radius * 10.0f), LookAt(eye, center));

  SDL_GPUColorTargetInfo target = {};
  target.texture = g_preview_target;
  target.load_op = SDL_GPU_LOADOP_CLEAR;
  target.store_op = SDL_GPU_STOREOP_STORE;
  target.clear_color = {0.08f, 0.08f, 0.1f, 1.0f};
  SDL_PushGPUVertexUniformData(command_buffer, 0, &clip_from_object, sizeof(clip_from_object));
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer, &target, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, pipeline);
  SDL_BindGPUVertexBuffers(pass, 0, vertex_buffers.data(), uint32_t(vertex_buffers.size()));
  if (draw.indexed) {
    const SDL_GPUBufferBinding index_binding = {index_buffer->second, 0};
    SDL_BindGPUIndexBuffer(
        pass, &index_binding,
        draw.index32 ? SDL_GPU_INDEXELEMENTSIZE_32BIT : SDL_GPU_INDEXELEMENTSIZE_16BIT);
    SDL_DrawGPUIndexedPrimitives(pass, draw.index_count, 1, draw.start_index, draw.index_offset, 0);
  } else {
    SDL_DrawGPUPrimitives(pass, draw.index_count, 1, draw.start_index, 0);
  }
  SDL_EndGPURenderPass(pass);
}

void NativeBuffersDrawBrowser() {
  ImGui::SetNextWindowPos(ImVec2(840.0f, 120.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(620.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Draws");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ImGui::Text("Last frame: %u draws, capture %.2f ms (hashing %.1f KB: %.2f ms), %u uploads "
                "(%.1f KB)",
                g_last_stats.draws, g_last_stats.capture_ns / 1e6,
                g_last_stats.hash_bytes / 1024.0, g_last_stats.hash_ns / 1e6,
                g_last_stats.uploads, g_last_stats.upload_bytes / 1024.0);
    ImGui::Text("Host buffers %zu, uploaded %.1f MB in total", g_buffers.size(),
                g_total_upload_bytes / (1024.0 * 1024.0));
    for (uint32_t primitive = 0; primitive < 64; ++primitive) {
      const uint32_t count = g_last_stats.primitives[primitive];
      if (!count) {
        continue;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("| %s %u", PrimitiveName(PrimitiveType(primitive)), count);
    }
    for (const auto& [problem, count] : g_last_stats.problems) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s: %u", problem.c_str(), count);
    }
    if (ImGui::Button("Capture frame")) {
      g_captured = g_last_draws;
      g_selected = -1;
    }
  }
  ImGui::SameLine();
  ImGui::Text("%zu draws captured", g_captured.size());
  ImGui::BeginChild("list");
  ImGuiListClipper clipper;
  clipper.Begin(int(g_captured.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const DrawRecord& draw = g_captured[i];
      std::string inputs;
      for (const Element& element : draw.elements) {
        inputs += std::string(" ") + kLocationNames[element.location];
      }
      char label[256];
      std::snprintf(label, sizeof(label), "%4d %-13s %6u %s%s  v %u-%u%s  vs %08X:%s %s", i,
                    PrimitiveName(draw.primitive), draw.index_count, draw.indexed ? "idx" : "vtx",
                    draw.up ? " UP" : "", draw.min_index, draw.max_index,
                    draw.reset ? " reset" : "", draw.vertex_shader, inputs.c_str(),
                    draw.problem.c_str());
      if (ImGui::Selectable(label, g_selected == i)) {
        g_selected = i;
        ComputeBounds(draw);
      }
    }
  }
  ImGui::EndChild();
  ImGui::End();

  if (g_selected < 0 || g_selected >= int(g_captured.size())) {
    return;
  }
  const DrawRecord& draw = g_captured[g_selected];
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(540.0f, 640.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Draw preview", &open);
  if (draw.indexed) {
    ImGui::Text("Draw %d: %s%s, %u indices from %u (%s, offset %d), vertices %u-%u", g_selected,
                PrimitiveName(draw.primitive), draw.up ? " UP" : "", draw.index_count,
                draw.start_index, draw.index32 ? "32-bit" : "16-bit", draw.index_offset,
                draw.min_index, draw.max_index);
  } else {
    ImGui::Text("Draw %d: %s%s, %u vertices from %u", g_selected, PrimitiveName(draw.primitive),
                draw.up ? " UP" : "", draw.index_count, draw.start_index);
  }
  for (size_t i = 0; i < draw.bindings.size(); ++i) {
    ImGui::Text("Buffer %zu: %08X, %u bytes, stride %u%s", i, draw.bindings[i].buffer.address,
                draw.bindings[i].buffer.size, draw.bindings[i].stride,
                draw.bindings[i].buffer.variant >> 31 ? " (float4 copy)" : "");
  }
  if (g_have_bounds) {
    ImGui::Text("Bounds (%.1f %.1f %.1f) - (%.1f %.1f %.1f)", g_bounds_min[0], g_bounds_min[1],
                g_bounds_min[2], g_bounds_max[0], g_bounds_max[1], g_bounds_max[2]);
  }
  if (!g_preview_status.empty()) {
    ImGui::TextDisabled("%s", g_preview_status.c_str());
  } else if (g_preview_target) {
    ImGui::Image(ImTextureRef(ImTextureID(g_preview_target)),
                 ImVec2(float(kPreviewSize), float(kPreviewSize)));
  }
  ImGui::End();
  if (!open) {
    g_selected = -1;
  }
}
