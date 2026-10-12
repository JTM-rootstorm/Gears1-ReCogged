#!/usr/bin/env bash
# Replay the spike repro (aiming capture 102245 truncated after frame 119553)
# N times and count pure-black pixels where the spikes appear.
# Clean (spike-free) result is 5479 every run; the NVIDIA bug gives large,
# different numbers each run. Usage: tools/spike_replay_check.sh [runs]
# REPLACE_SHADERS=<dir>: replay with gfxrecon-replay --replace-shaders (files
# named sh<module id>, e.g. sh664 = the skinning depth VS). A replaced shader
# may legitimately differ a little from 5479; judge by size and run-to-run
# consistency. KEEP_DIR=<dir>: keep each run's screenshot as run<N>.png.
set -euo pipefail
CAP="$HOME/Documents/Programming/Recomp/Gears/gears1_captures/cutscene/spike_frame119553.gfxr"
RUNS="${1:-3}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
EXTRA=()
if [ -n "${REPLACE_SHADERS:-}" ]; then EXTRA+=(--replace-shaders "$REPLACE_SHADERS"); fi
if [ -n "${KEEP_DIR:-}" ]; then mkdir -p "$KEEP_DIR"; fi
for k in $(seq 1 "$RUNS"); do
  rm -f "$TMP"/*.png
  gfxrecon-replay --log-level error --swapchain offscreen --screenshots 119553 \
    --screenshot-format png --screenshot-dir "$TMP" "${EXTRA[@]}" "$CAP" > /dev/null 2>&1
  if [ -n "${KEEP_DIR:-}" ]; then cp "$TMP/screenshot_frame_119553.png" "$KEEP_DIR/run$k.png"; fi
  python3 -c "
import numpy as np, sys
from PIL import Image
a = np.asarray(Image.open('$TMP/screenshot_frame_119553.png').convert('RGB')).max(-1)
n = int((a[700:2160, 0:1950] <= 1).sum())
print(f'run $k: {n} black px', '(clean)' if n == 5479 else '(SPIKES)')"
done
