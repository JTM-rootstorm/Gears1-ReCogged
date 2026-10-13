// gears1_recomp - ReXGlue Recompiled Project

#include "generated/default/gears1_recomp_init.h"

#include "gears1_recomp_app.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

// UE3 renders the 2x MSAA scene in predicated screen tiles, which show black
// polygons on characters under emulation. GTilingMode = 4 selects its single
// full-screen tile preset with the tiling code left on (Canary's "Black
// Shading Fix" patch also sets GUseTilingCode = FALSE). Presets and globals:
// FINDINGS.md "UE3 TILING GLOBALS".
REXCVAR_DEFINE_BOOL(gears1_black_shading_fix, true, "Gears1",
                    "Render the UE3 scene as one full-screen tile (GTilingMode = 4) instead "
                    "of 2x MSAA predicated tiles")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gears1_use_tiling_code, -1, "Gears1",
                     "Override UE3 GUseTilingCode (0 or 1, -1 = default)")
    .range(-1, 1)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gears1_tiling_mode, -1, "Gears1",
                     "Override UE3 GTilingMode render target preset (0-4, -1 = default)")
    .range(-1, 4)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Low bytes of the big-endian 32-bit globals.
constexpr uint32_t kUseTilingCodeAddress = 0x82BB6BBB;
constexpr uint32_t kTilingModeAddress = 0x82BB6B53;
constexpr uint8_t kUseTilingCodeOriginal = 0x01;
constexpr uint8_t kTilingModeOriginal = 0x01;

}  // namespace

void Gears1RecompApp::OnPostLoadXexImage() {
  auto* memory = runtime()->memory();
  uint8_t* use_tiling_code = memory->TranslateVirtual<uint8_t*>(kUseTilingCodeAddress);
  uint8_t* tiling_mode = memory->TranslateVirtual<uint8_t*>(kTilingModeAddress);
  // Check the original bytes first so a different XEX build is never patched.
  if (*use_tiling_code != kUseTilingCodeOriginal || *tiling_mode != kTilingModeOriginal) {
    REXLOG_WARN("Tiling patches skipped: GUseTilingCode {:02X}, GTilingMode {:02X} (expected 01 01)",
                *use_tiling_code, *tiling_mode);
    return;
  }
  int32_t use = kUseTilingCodeOriginal, mode = kTilingModeOriginal;
  if (REXCVAR_GET(gears1_black_shading_fix)) {
    mode = 4;
  }
  if (REXCVAR_GET(gears1_use_tiling_code) >= 0) {
    use = REXCVAR_GET(gears1_use_tiling_code);
  }
  if (REXCVAR_GET(gears1_tiling_mode) >= 0) {
    mode = REXCVAR_GET(gears1_tiling_mode);
  }
  *use_tiling_code = uint8_t(use);
  *tiling_mode = uint8_t(mode);
  REXLOG_INFO("UE3 tiling: GUseTilingCode = {}, GTilingMode = {}{}", use, mode,
              REXCVAR_GET(gears1_black_shading_fix) ? " (black shading fix)" : "");
}

REX_DEFINE_APP(gears1_recomp, Gears1RecompApp::Create)
