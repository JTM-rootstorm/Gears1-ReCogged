#!/usr/bin/env bash
# Run any Vulkan program with the strip-RTE layer enabled.
# Usage: tools/vk_strip_rte/run.sh <command> [args...]
DIR="$(cd "$(dirname "$0")" && pwd)"
[ -f "$DIR/out/libVkLayer_gears1_strip_rte.so" ] || "$DIR/build.sh"
export VK_ADD_LAYER_PATH="$DIR/out${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
export VK_INSTANCE_LAYERS="VK_LAYER_GEARS1_strip_rte${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
exec "$@"
