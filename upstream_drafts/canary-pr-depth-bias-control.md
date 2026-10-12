# xenia-canary PR — POSTED as xenia-canary/xenia-canary#1289 (2026-10-11)

Branch: JTM-rootstorm:vulkan-depth-bias-control (a51e0ef7d, on 2372a5410)
Base: canary_experimental

---

**Title**

[Vulkan] Apply depth bias in constant units with VK_EXT_depth_bias_control

---

**Body**

Fixes the Z-fighting of ground blood decals in 4D5307D5 on Vulkan without
any configuration, on drivers that support `VK_EXT_depth_bias_control`.

The guest polygon offset for float24 depth is an absolute depth offset.
`kD3D10PolygonOffsetFactorFloat24` converts it assuming the bias unit of
float32 depth in [0.5, 1). Per the Vulkan specification (Depth Bias
Computation), the unit for a floating-point depth attachment is actually
2^(e - n), where e is the maximum exponent of the primitive's depth. So the
host bias shrinks at lower depths. With reversed depth most of the scene is
at low depths, and coplanar decals Z-fight there. A 4x larger bias hides it
in 4D5307D5, on either the guest or the host side.

Where the device supports `depthBiasControl`,
`leastRepresentableValueForceUnormRepresentation` and `depthBiasExact`, the
dynamic depth bias is set with `vkCmdSetDepthBias2EXT`, using
`VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT`
and `depthBiasExact`. The unit is then the constant 2^-n, where n is the
mantissa bits plus one for floating-point formats (2^-24 for `D32_SFLOAT`).
So the existing factor produces the guest offset at every depth. For
float24 the factor is halved, because host render targets store the guest
[0, 1] depth range as [0, 0.5] (`full_float24_in_0_to_1`).

The extension and its features are enabled in `VulkanDevice`, and the call
is recorded through the deferred command buffer. The new cvar
`vulkan_depth_bias_force_unorm` is enabled by default. Without the
extension features, the depth bias is applied exactly as before.

### Relation to `depth_bias_shader_offset`

`depth_bias_shader_offset` (from #1131) fixes the same Z-fighting by moving
decal-style draws to shader depth. The two don't conflict: for draws that
the shader offset handles, the fixed-function bias is already zeroed. This
change instead corrects the fixed-function bias itself:
- every draw with a polygon offset gets the guest's offset, not only the
  ones detected as decals;
- depth stays fixed-function, so early depth testing is unaffected;
- nothing needs to be configured where the extension is supported.

### Testing

Linux, RTX 4080 (NVIDIA 615.78.08), Vulkan with the FBO render target path,
4D5307D5, at a spot with ground blood decals from defeated enemies.

Every run also used:
- `window_offset_in_edram=false` and `xenos_sample_positions=true`;
- a SPIR-V main-loop fallthrough change equivalent to xenia-edge f145711a4,
  which this title needs on this driver.

The log confirms `VK_EXT_depth_bias_control`, `depthBiasExact` and
`leastRepresentableValueForceUnormRepresentation`.

| Run | `vulkan_depth_bias_force_unorm` | `depth_bias_shader_offset` | Ground decals |
|---|---|---|---|
| A | true | false | stable |
| B | false | true | stable |
| C | false | false | Z-fighting |

I didn't notice a performance difference between A and B. Only tested on
NVIDIA. AMD and Intel drivers that expose the same features should behave
the same, but I haven't tested them.

Was AI used for this PR: Yes, heavily relying on an LLM (Claude, Anthropic).
The cause was found and the change was written with it: tracing the game's
depth bias calls, and comparing guest and host bias with scaled-bias
experiments. I tested the results in game on my own hardware.
