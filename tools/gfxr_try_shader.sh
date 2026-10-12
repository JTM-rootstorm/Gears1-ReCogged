#!/usr/bin/env bash
# Replay a capture with one shader module replaced; report whether the GPU faulted.
# Usage: tools/gfxr_try_shader.sh <module_id> <shader.spvasm|shader.spv> <capture.gfxr>
# Prints "fault" or "ok" (new NVIDIA Xid in the kernel log or not), or "asm-fail".
set -uo pipefail
mod=$1; src=$2; cap=$3
d=$(mktemp -d "${TMPDIR:-/tmp}/gfxr_try.XXXXXX"); trap 'rm -rf "$d"' EXIT
case $src in
  *.spvasm) spirv-as --target-env vulkan1.3 "$src" -o "$d/sh$mod" 2>"$d/asm.err" || { echo asm-fail; cat "$d/asm.err" >&2; exit 2; } ;;
  *) cp "$src" "$d/sh$mod" ;;
esac
before=$(journalctl -k --no-pager | grep -c Xid)
timeout 300 gfxrecon-replay --log-level error --swapchain offscreen --replace-shaders "$d" "$cap" >"$d/replay.log" 2>&1
sleep 1.5
after=$(journalctl -k --no-pager | grep -c Xid)
if grep -q FATAL "$d/replay.log"; then echo "replay-fatal"; grep -m1 FATAL "$d/replay.log" >&2; exit 3; fi
[ $((after - before)) -gt 0 ] && echo fault || echo ok
