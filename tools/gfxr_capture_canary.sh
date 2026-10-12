#!/usr/bin/env bash
# Capture Xenia Canary (the reference renderer) with GFXReconstruct, same
# settings as tools/gfxr_capture.sh, so its draws can be diffed against the
# recomp's. Goes through the strip-RTE layer (NVIDIA Xid 31 workaround); that
# layer sits above GFXR, so the captured shaders are already stripped and the
# capture replays safely.
# Usage: GFXR_FRAMES=10 tools/gfxr_capture_canary.sh [outdir] [extra canary args...]
# Canary needs /dev/shm mounted exec (sudo mount -o remount,exec /dev/shm).
# Captures contain game textures/shaders: they stay OUT of the repo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CANARY="$HOME/Documents/Programming/Recomp/Gears/xenia-canary/02d2cb5/build/bin/Linux/Release/xenia_canary"
OUT="${1:-$HOME/Documents/Programming/Recomp/Gears/gears1_captures/canary}"; shift || true
mkdir -p "$OUT"

LAYER_DIR="$ROOT/tools/vk_strip_rte"
[ -f "$LAYER_DIR/out/libVkLayer_gears1_strip_rte.so" ] || "$LAYER_DIR/build.sh"
export VK_ADD_LAYER_PATH="$LAYER_DIR/out${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
# First listed = closest to the application.
export VK_INSTANCE_LAYERS=VK_LAYER_GEARS1_strip_rte:VK_LAYER_LUNARG_gfxreconstruct
export STRIP_RTE_QUIET=1

export GFXRECON_CAPTURE_FILE="$OUT/canary.gfxr"
export GFXRECON_CAPTURE_FILE_TIMESTAMP=true
export GFXRECON_CAPTURE_FILE_FLUSH=true
export GFXRECON_CAPTURE_COMPRESSION_TYPE=LZ4
export GFXRECON_MEMORY_TRACKING_MODE=unassisted
export GFXRECON_CAPTURE_TRIGGER=F12
if [ -n "${GFXR_FRAMES:-}" ]; then export GFXRECON_CAPTURE_TRIGGER_FRAMES="$GFXR_FRAMES"; fi
export GDK_BACKEND=x11
export GFXRECON_LOG_LEVEL=info
export GFXRECON_LOG_FILE="$OUT/gfxrecon_capture.log"

exec "$CANARY" --gpu=vulkan "$@" "$HOME/Downloads/Gears of War/GoW/default.xex"
