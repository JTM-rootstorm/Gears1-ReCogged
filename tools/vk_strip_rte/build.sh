#!/usr/bin/env bash
# Build the strip-RTE Vulkan layer and its manifest into tools/vk_strip_rte/out/.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/out"
cc -O2 -fPIC -shared -fvisibility=hidden -Wall -o "$DIR/out/libVkLayer_gears1_strip_rte.so" "$DIR/strip_rte_layer.c" -lpthread
cat > "$DIR/out/VkLayer_gears1_strip_rte.json" <<EOF
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_GEARS1_strip_rte",
    "type": "GLOBAL",
    "library_path": "./libVkLayer_gears1_strip_rte.so",
    "api_version": "1.4.0",
    "implementation_version": "1",
    "description": "Strips RoundingModeRTE from SPIR-V (NVIDIA Xid 31 workaround)",
    "functions": {
      "vkGetInstanceProcAddr": "strip_rte_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "strip_rte_GetDeviceProcAddr"
    }
  }
}
EOF
echo "built $DIR/out"
