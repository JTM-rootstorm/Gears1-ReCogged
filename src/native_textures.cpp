// Native renderer, phase 2: guest textures as SDL_GPU textures.
//
// Each texture is decoded on the guest thread the first time D3D SetTexture
// binds it (pre hook, tools/hook_table.py): its GPU fetch constant (six
// dwords at +0x1C of the texture object, which SetTexture merges into the
// device's sampler state) gives the format, size, tiling, endianness and
// physical address. The base level is untiled and byte-swapped with the
// runtime's texture helpers (rex::graphics::texture_util / _conversion),
// then uploaded on the main thread. Channels stay as the guest stores them;
// the fetch constant's swizzle is applied when sampling (phase 3).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "native_renderer.h"

namespace {

using rex::graphics::FormatInfo;
using rex::graphics::xenos::TextureFormat;
using rex::graphics::xenos::xe_gpu_texture_fetch_t;

constexpr uint32_t kTextureFetchConstantOffset = 0x1C;
// Physical memory is mirrored at 0xA0000000 in the guest address space.
constexpr uint32_t kPhysicalMirror = 0xA0000000;

SDL_GPUTextureFormat HostFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::k_8:
      return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case TextureFormat::k_8_8:
      return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    case TextureFormat::k_8_8_8_8:
      return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case TextureFormat::k_DXT1:
      return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case TextureFormat::k_DXT2_3:
      return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case TextureFormat::k_DXT4_5:
      return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case TextureFormat::k_DXT5A:
      return SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
    case TextureFormat::k_DXN:
      return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    case TextureFormat::k_16_16_16_16_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::k_32_FLOAT:
      return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    default:
      return SDL_GPU_TEXTUREFORMAT_INVALID;
  }
}

struct DecodedTexture {
  uint32_t address;
  uint32_t width, height;  // texels
  uint32_t row_texels, rows;  // upload layout: whole blocks
  TextureFormat guest_format;
  SDL_GPUTextureFormat host_format;
  std::vector<uint8_t> data;
};

struct HostTexture {
  uint32_t address;
  uint32_t width, height;
  TextureFormat guest_format;
  SDL_GPUTexture* texture;
};

std::mutex g_mutex;
std::unordered_set<std::string> g_seen;  // fetch dwords 0-2: address, format, size
std::vector<DecodedTexture> g_pending;
std::map<std::string, uint32_t> g_unsupported;  // reason -> count
uint32_t g_decoded = 0;

// Main thread only.
std::vector<HostTexture> g_textures;

bool Decode(const xe_gpu_texture_fetch_t& fetch, uint8_t* base, DecodedTexture& out,
            std::string& reason) {
  using namespace rex::graphics;
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    reason = "dimension " + std::to_string(uint32_t(fetch.dimension));
    return false;
  }
  out.guest_format = fetch.format;
  out.host_format = HostFormat(fetch.format);
  const FormatInfo* info = FormatInfo::Get(fetch.format);
  if (out.host_format == SDL_GPU_TEXTUREFORMAT_INVALID || !info) {
    reason = "format " + std::to_string(uint32_t(fetch.format));
    return false;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (!base_page) {
    reason = "no base level";
    return false;
  }
  out.address = base_page << 12;
  out.width = width_minus_1 + 1;
  out.height = height_minus_1 + 1;
  const uint32_t blocks_x = (out.width + info->block_width - 1) / info->block_width;
  const uint32_t blocks_y = (out.height + info->block_height - 1) / info->block_height;
  const uint32_t block_bytes = info->bytes_per_block();
  out.row_texels = blocks_x * info->block_width;
  out.rows = blocks_y * info->block_height;
  out.data.resize(size_t(blocks_x) * blocks_y * block_bytes);
  const uint8_t* source = rex::memory::GuestPtr(base, kPhysicalMirror | out.address);
  const xenos::Endian endian = fetch.endianness;
  // The fetch constant's pitch is in 32-texel units.
  const uint32_t pitch_blocks = fetch.pitch * 32 / info->block_width;
  if (fetch.tiled) {
    texture_conversion::UntileInfo untile = {};
    untile.width = blocks_x;
    untile.height = blocks_y;
    untile.input_pitch = pitch_blocks;
    untile.output_pitch = blocks_x;
    untile.input_format_info = info;
    untile.output_format_info = info;
    untile.copy_callback = [endian](void* output, const void* input, size_t length) {
      texture_conversion::CopySwapBlock(endian, output, input, length);
    };
    texture_conversion::Untile(out.data.data(), source, &untile);
  } else {
    const uint32_t row_bytes = blocks_x * block_bytes;
    const uint32_t source_row_bytes = rex::align(pitch_blocks * block_bytes,
                                                 xenos::kTextureLinearRowAlignmentBytes);
    for (uint32_t y = 0; y < blocks_y; ++y) {
      texture_conversion::CopySwapBlock(endian, out.data.data() + size_t(y) * row_bytes,
                                        source + size_t(y) * source_row_bytes, row_bytes);
    }
  }
  return true;
}

}  // namespace

// Pre hook of D3D SetTexture (82220858): r3 = device, r4 = sampler, r5 =
// texture object or 0.
void Gears1NativeTexturesOnSetTexture(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gears1_native_renderer) || !ctx.r5.u32) {
    return;
  }
  uint32_t words[6];
  const uint8_t* constant = rex::memory::GuestPtr(base, ctx.r5.u32 + kTextureFetchConstantOffset);
  for (uint32_t i = 0; i < 6; ++i) {
    words[i] = rex::memory::load_and_swap<uint32_t>(constant + 4 * i);
  }
  xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(fetch));
  std::string key(reinterpret_cast<const char*>(words), 12);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_seen.insert(std::move(key)).second) {
      return;
    }
  }
  DecodedTexture texture;
  std::string reason;
  const bool decoded = Decode(fetch, base, texture, reason);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (decoded) {
    ++g_decoded;
    g_pending.push_back(std::move(texture));
  } else {
    ++g_unsupported[reason];
  }
}

void NativeTexturesUpload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer) {
  std::vector<DecodedTexture> pending;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pending.swap(g_pending);
  }
  if (pending.empty()) {
    return;
  }
  size_t total = 0;
  for (const DecodedTexture& texture : pending) {
    total += texture.data.size();
  }
  SDL_GPUTransferBufferCreateInfo transfer_info = {};
  transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer_info.size = uint32_t(total);
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
  if (!transfer) {
    REXLOG_ERROR("Native textures: no {}-byte transfer buffer: {}", total, SDL_GetError());
    return;
  }
  auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, transfer, false));
  SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
  uint32_t offset = 0;
  for (const DecodedTexture& texture : pending) {
    SDL_GPUTextureCreateInfo create_info = {};
    create_info.type = SDL_GPU_TEXTURETYPE_2D;
    create_info.format = texture.host_format;
    create_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    create_info.width = texture.width;
    create_info.height = texture.height;
    create_info.layer_count_or_depth = 1;
    create_info.num_levels = 1;
    SDL_GPUTexture* host = SDL_CreateGPUTexture(device, &create_info);
    if (!host) {
      REXLOG_WARN("Native textures: {}x{} format {} not created: {}", texture.width,
                  texture.height, uint32_t(texture.guest_format), SDL_GetError());
      continue;
    }
    std::memcpy(mapped + offset, texture.data.data(), texture.data.size());
    SDL_GPUTextureTransferInfo source = {};
    source.transfer_buffer = transfer;
    source.offset = offset;
    source.pixels_per_row = texture.row_texels;
    source.rows_per_layer = texture.rows;
    SDL_GPUTextureRegion destination = {};
    destination.texture = host;
    destination.w = texture.width;
    destination.h = texture.height;
    destination.d = 1;
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
    offset += uint32_t(texture.data.size());
    g_textures.push_back({texture.address, texture.width, texture.height, texture.guest_format, host});
  }
  SDL_EndGPUCopyPass(copy_pass);
  SDL_UnmapGPUTransferBuffer(device, transfer);
  SDL_ReleaseGPUTransferBuffer(device, transfer);
}

void NativeTexturesDrawBrowser() {
  static int page = 0;
  constexpr int kPerPage = 48;
  ImGui::SetNextWindowPos(ImVec2(10.0f, 120.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(820.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Textures (base level, guest channel order)");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ImGui::Text("Decoded %u, uploaded %zu", g_decoded, g_textures.size());
    for (const auto& [reason, count] : g_unsupported) {
      ImGui::SameLine();
      ImGui::TextDisabled("| %s: %u", reason.c_str(), count);
    }
  }
  const int pages = std::max(1, int((g_textures.size() + kPerPage - 1) / kPerPage));
  page = std::clamp(page, 0, pages - 1);
  ImGui::SliderInt("Page", &page, 0, pages - 1);
  for (int i = page * kPerPage; i < std::min<int>(int(g_textures.size()), (page + 1) * kPerPage);
       ++i) {
    const HostTexture& texture = g_textures[i];
    if ((i - page * kPerPage) % 8) {
      ImGui::SameLine();
    }
    ImGui::Image(ImTextureRef(ImTextureID(texture.texture)), ImVec2(92.0f, 92.0f));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%08X  %ux%u  format %u", texture.address, texture.width, texture.height,
                        uint32_t(texture.guest_format));
    }
  }
  ImGui::End();
}
