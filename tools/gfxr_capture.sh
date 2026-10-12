#!/usr/bin/env bash
# Run the game under the GFXReconstruct capture layer to record every Vulkan
# call up to the GPU fault, for offline replay.
# Usage: tools/gfxr_capture.sh [outdir] [extra game args...]
# BUILD=release uses out/build/linux-amd64-release (default: debug).
# Captures contain game textures/shaders: they stay OUT of the repo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HOME/Documents/Programming/Recomp/Gears/gears1_captures}"; shift || true
mkdir -p "$OUT"

# runtime_override/ (if present) holds a rebuilt librexruntimed.so from the
# SDK branch (enables VK_EXT_sample_locations); it must come first.
BUILD_DIR="$ROOT/out/build/linux-amd64-${BUILD:-debug}"
export LD_LIBRARY_PATH="$BUILD_DIR/runtime_override:$HOME/Documents/Programming/ReXGlue/linux-amd64/lib"
export VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_gfxreconstruct
export GFXRECON_CAPTURE_FILE="$OUT/gears1.gfxr"
export GFXRECON_CAPTURE_FILE_TIMESTAMP=true
# GFXR_CRASH=1: flush every block (the game dies via abort() on device loss,
# and the last frames would never reach disk) and use unassisted tracking.
export GFXRECON_CAPTURE_COMPRESSION_TYPE=LZ4
EXTRA_ARGS=()
if [ -n "${GFXR_CRASH:-}" ]; then
  export GFXRECON_CAPTURE_FILE_FLUSH=true
  # page_guard uses SIGSEGV, which the recomp runtime owns; userfaultfd is
  # blocked (vm.unprivileged_userfaultfd=0). unassisted needs no signals: it
  # snapshots all mapped memory at each submit. Safe, but slow.
  export GFXRECON_MEMORY_TRACKING_MODE=unassisted
else
  # assisted records only vkFlushMappedMemoryRanges ranges; the runtime flushes
  # host-coherent memory too with this cvar (needs runtime_override/).
  export GFXRECON_MEMORY_TRACKING_MODE=assisted
  EXTRA_ARGS+=(--vulkan_flush_coherent_mapped_memory=true)
fi
# Force FIFO (vsync) presentation. With immediate/mailbox the presenter repaints
# thousands of times a second, and since GFXR serializes Vulkan calls that
# starved the game's GPU thread (~1 game frame per second while recording).
EXTRA_ARGS+=(--vulkan_allow_present_mode_immediate=false
             --vulkan_allow_present_mode_mailbox=false
             --vulkan_allow_present_mode_fifo_relaxed=false)
# Recording from boot in unassisted mode is ~0.1 fps, so start disabled and
# trim: press F12 (e.g. on the menu just before New Game) to snapshot state
# and record until the crash. GFXR reads the hotkey via X11, so run the
# window under XWayland.
export GFXRECON_CAPTURE_TRIGGER=F12
# GFXR_FRAMES=N: stop by itself N presents after F12 (default: until F12 again /
# exit). With the frame step hotkeys (F9 pause, F10 step, F12 while paused
# re-presents), leave it unset: F12 start, F10 steps, F12 stop.
if [ -n "${GFXR_FRAMES:-}" ]; then export GFXRECON_CAPTURE_TRIGGER_FRAMES="$GFXR_FRAMES"; fi
export SDL_VIDEO_DRIVER=x11
export GFXRECON_LOG_LEVEL=info
export GFXRECON_LOG_FILE="$OUT/gfxrecon_capture.log"

exec "$BUILD_DIR/gears1_recomp" \
  --game_data_root "$HOME/Downloads/Gears of War/GoW" \
  --gpu_plugin xenos "${EXTRA_ARGS[@]}" "$@"
