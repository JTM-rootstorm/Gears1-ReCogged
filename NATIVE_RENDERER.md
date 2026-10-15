# Native renderer plan

Goal: replace Xenos GPU emulation (PM4 packets → EDRAM emulation → translated
microcode) with a renderer that implements the game's own Direct3D calls on
SDL_GPU (Vulkan on Linux, Metal possible later; NATIVE_PORT.md "Platform:
SDL3"), in the spirit of Unleashed Recompiled. Keep the emulated path as a
switchable fallback and as the reference to compare against.

Status: phase 2 resources: textures (src/native_textures.cpp), vertex and
index buffers (src/native_buffers.cpp). Findings and progress go
in FINDINGS.md.

Decided (2026-10-10): hook at the Xbox D3D API level; prototype BOTH shader
translators on the skinning VS before choosing.

## Why

- Most emulation bugs fixed so far exist only because we emulate the GPU:
  EDRAM ownership transfers (stencil bit, gamma), predicated tiling, 2x MSAA
  sample positions, EDRAM depth resolve/restore. A native renderer has no
  EDRAM, so these problems don't exist there.
- Performance: no PM4 parsing, no EDRAM copies, no per-draw shader
  specialization for render-target state.
- Freedom: real MSAA, resolution scaling, widescreen, higher-precision
  depth, sane presentation.
- Not a guaranteed fix for the NVIDIA skinning-shader miscompile (FINDINGS,
  "LAVAPIPE VERDICT"): shaders still come from Xenos microcode. Different
  codegen (HLSL → DXC) may avoid it; test early.

## What ReXGlue already gives us

- `REX_HOOK` / `REX_HOOK_RAW` (include/rex/hook.h): replace any recompiled
  guest function with native C++.
- Detached presentation ("bring your own renderer"):
  `OnPreSetup` clears `config.graphics`, `OnCreateImmediateDrawer` supplies
  the ImGui drawer; the app owns device, swapchain, present timing.
- Reusable code in the SDK: Xenos texture untiling and format conversion
  (texture_cache), endian swaps, the Xenos shader translator (as a reference
  or fallback), vertex fetch decoding.

## Hook level (decision 1)

| Level | What we implement | Pros | Cons |
|---|---|---|---|
| PM4 (today) | GPU command stream | Generic, done | All the EDRAM/tiling emulation |
| **Xbox D3D API** (recommended) | `D3DDevice_*` functions statically linked in the XEX: SetRenderTarget, SetTexture, SetVertexShader, DrawIndexedVertices, Resolve, Present, Create*… | Well-defined, D3D9-like API; Unleashed precedent; engine-agnostic | Functions must be identified (no symbols); some tiling/predication APIs to stub |
| UE3 RHI | `RHI*` functions (RHIDrawPrimitive, RHISetRenderTarget…) | Fewer, higher-level calls; semantic knowledge of surfaces | Engine-specific; exists in this build (see "UE3 RHI layer"), but some draws and state writes bypass it |

Identification: Ghidra on default.xex plus the 2007 PC build (D3D9, same
engine) to match functions by strings, call graphs and the D3D call
patterns we already see in PM4. Turn tiling off with the existing data patch
(`gears1_black_shading_fix`) so predicated-tiling calls never happen.

## Shaders (decision 2)

Shaders live in UE3 cooked packages (`CookedXenon/*.xxx`, LZX-compressed), so
capture them at runtime instead of extracting offline: hook shader creation,
translate the microcode, cache the result on disk keyed by hash. Translator
options:

1. **XenosRecomp** (hedge-dev): microcode → HLSL → DXC → SPIR-V. Different
   codegen from Xenia's translator (may dodge the NVIDIA bug). Known gaps:
   dynamic register indexing unimplemented, partial dynamic constant
   indexing (our skinning shaders rely on relative constant addressing), a
   few Sonic-specific hardcodes. Would need extending.
2. **ReXGlue's SPIR-V translator** reconfigured for a native ABI (no EDRAM
   epilogue, plain bindings). Known-good semantics (we've fixed its bugs),
   but the same codegen that triggers the NVIDIA miscompile.

Plan: prototype with the skinning VS (VS 664 microcode) through both and run
`tools/spike_replay_check.sh`-style tests before committing.

Prototype result (2026-10-10, FINDINGS "TRANSLATOR PROTOTYPES"): XenosRecomp
+ DXC is deterministic and spike-free on NVIDIA once its dp4 summation order
matches the other shaders; ReXGlue's translation (even re-optimised) keeps
the NVIDIA nondeterminism. Constraint learned: every shader that writes depth
for the same surface must use the same arithmetic order (EQUAL depth test
between pre-pass and base pass), so in the native renderer ALL shaders must
come from one translator with Xenia-equivalent op ordering.

## Phases

0. **Inventory** (no rendering changes)
   - Identify D3D entry points in the XEX; hook them in observe-only mode
     (log, then call the original) while the emulated renderer keeps
     drawing. Validate against a PM4/GFXR capture of the same frame.
   - Per-frame call trace tool: passes, render targets, formats, resolves.
   - Shader census: count, stages, features used (relative addressing,
     loops, predication, vfetch formats).
1. **Skeleton**: SDL_GPU device + swapchain, hook device creation and
   Present; clear-to-colour each frame; ImGui overlay works. Built as a
   second window next to the emulated one (gears1_native_renderer), not
   detached: until every D3D call that feeds the GPU is replaced, the
   game's D3D writes PM4 and waits on the GPU (ring buffer space, fences,
   the vsync interrupt), so the emulated GPU must keep consuming it. The
   side-by-side window is also where later phases compare against the
   reference. Device init = 8223A840(device, D3DPRESENT_PARAMETERS*): the
   only D3D function calling VdInitializeEngines, called by UE3's back
   buffer setup 824A4D48 (which allocates and zeroes the 0x5080-byte
   device itself).
2. **Resources**: textures (untile + convert, reuse SDK code), vertex and
   index buffers (big-endian handling), render targets as real images,
   Resolve → copy/blit, MSAA as native multisampling.
3. **Shaders + state**: translation pipeline and cache, constants
   (float/bool/int), vertex declarations, samplers, blend/depth/stencil
   state, pipeline cache.
4. **Bring-up order**: menu UI → depth pre-pass + base pass → light passes
   → stencil shadow volumes + shadow-map projection → post (bloom, motion
   blur, tone mapping) → Bink movies → HUD.
   Compare every step against the emulated path (GFXR screenshots of the
   same frame).
5. **Polish**: resolution scaling, widescreen (Canary patch has addresses),
   frame pacing, Steam Deck / macOS (MoltenVK) later.

Fallback: a cvar selects emulated vs native renderer for the whole session.

## D3D API map (phase 0, static analysis of the observer trace)

`dev` = D3DDevice. Sources: gears1_d3d_observe trace (tools/gen_hooks.py) + Ghidra/generated
code. Confidence high unless marked. All of these are named in the Ghidra
project (D3DDevice_*, D3DQuery_*, D3DResource_*, D3D_* for internals;
2026-10-10).

| Address | Function |
|---|---|
| 8222DE50 | DrawIndexedVertices(dev, prim, baseIdx, startIdx, idxCount) |
| 8222D4F8 | BeginIndexedVertices (UP-style draw, allocates VB/IB in the cmd buffer) |
| 8222AFD8 | SetIndices(dev, ib) → +0x2F84 |
| 8222AE20 | SetStreamSource(dev, stream, vb, offset, stride, dirty) → +0x2F9C+n*4 |
| 82222B98 / 82222808 | SetVertexShader → +0x3084 / SetPixelShader → +0x3080 |
| 82222350 / 82222460 | Set{Vertex,Pixel}ShaderConstantF (out-of-line; UE3 also inlines it, see below) |
| 82220858 | SetTexture(dev, sampler, tex, dirty) |
| 8222A150 / 8222A2D8 | SetSamplerState MinFilter (fetch dword 3 bits 21-22) / MagFilter (bits 19-20) |
| 82228998..82228D28 | SetRenderState into the saved RB_BLENDCONTROL word +0x2D38 / flags +0x2D3C: 82228998 AlphaBlendEnable, 82228A28 BlendOp (bits 5-7), 82228AB8 SrcBlend (0-4), 82228B48 DestBlend (8-12), 82228BD8 BlendOpAlpha (21-23), 82228C48 SrcBlendAlpha (16-20), 82228CB8 DestBlendAlpha (24-28), 82228D28 SeparateAlphaBlendEnable |
| 82229B28 | SetRenderState_HighPrecisionBlendEnable (med-high) |
| 82229398 / 82229460 | SetRenderState SlopeScaleDepthBias / DepthBias |
| 8222ABF8 / 8222AB30 | SetViewport / SetScissorRect |
| 8222B068 | SetRenderTarget(dev, index, surface) → +0x2F88+i*4 |
| 82222EF8 | SetShaderGPRAllocation |
| 8223E860 | Swap (VdSwap); 8223DC78 = its front-buffer copy pass |
| 822306A0 / 822304A0 | D3DQuery GetData / Issue (occlusion, event, screen extent) |
| 8222E868 / 8222E8E0 / 8222E988 | D3DResource AddRef / Release / GetType |
| 8221E7B8, 8221E960, 8221EDF8, 8221E850 | texture format/size helpers (XGraphics) |
| 82228458, 82228618, 8222FEF0, 8222B678 | internal: tiling reset, tiled scissor, query sample write, GPR-change dummy draws |
| 8222CFF8 | BeginVertices(dev, prim, count, stride) → write pointer into the cmd buffer (DRAW_INDX 0xC0012201); caller copies vertices, then EndVertices is inlined (+0x28 = +0x3314) |
| 8222D4B0 | DrawVerticesUP (BeginVertices + memcpy + inlined EndVertices) |
| 8222DA48 | DrawVertices(dev, prim, start, count) (non-indexed, no return; 2 UE3 callers, not seen in trace) |
| 822346A8 | internal pre-draw state commit; called by all four draws (reads decl +0x2D10, shaders, dirty masks) |
| 8222B398 | SetDepthStencilSurface(dev, surface) → +0x2F98 (only writer) |
| 8222CC48 | Clear(dev, flags&0xF, rects/NULL, color, f1=Z, stencil); per-tile loop when tiling is on (worker 8222C5C0) |
| 82235528 | Resolve (only code that loads RB_COPY_CONTROL 0x2318); called by UE3 and by Swap's 8223EF60 |
| 822364F8 / 822369F0 | BeginTiling / EndTiling (call Clear, viewport, 82228458); dead with the tiling patch |
| 82222E18 | CreateVertexDeclaration(elements) (12-byte elements, Stream 0xFF ends; object type 5) |
| 82222DF8 | SetVertexDeclaration → +0x2D10 (UE3 also inlines this store: 8254E728, 8254E948, 82591A48, …; the RHI stores 0, so UE3 draws with pre-patched vertex shaders and no declaration, FINDINGS "NATIVE VERTEX/INDEX BUFFERS") |
| 82222D68 | internal: set a built-in declaration (+0x2F80 = 2), used by 8223A0E0 |
| 82222AC8 / 82222710 | CreateVertexShader / CreatePixelShader (XDK path: 8221D020, 8249AED8, 8254F9E0, 827DBF20) |
| 822229D0 | VS object init(vs, physical microcode): type 6, header copy at +872, microcode ptr at +32 |
| 82228F70 / 82229028 | SetRenderState ZEnable / StencilEnable (RB_DEPTHCONTROL bits 1 / 0) |
| 82228FB8..82229240 | other RB_DEPTHCONTROL fields (ZWrite, ZFunc, stencil funcs/ops, two-sided); reached via a dispatch table, not direct calls |
| 822288F8..82229EC8 (+0x28C8) | PA_SU_SC_MODE_CNTL fields (cull, fill, depth-bias enables); 82229558..82229600 ColorWriteEnable0-3 (RB_COLOR_MASK) |

Shaders as UE3 creates them: VS through 825A4360 (inlined CreateVertexShader +
822229D0), PS through 825A44C8, which inlines the whole PS init (type 7, header
copy at +40, microcode ptr at +24) with no D3D call. So capture microcode at
SetVertexShader / SetPixelShader (or at the draw), not at creation: header
fields +4 = virtual (header) size, +8 = physical (microcode) size, flags bit 0
set = vertex shader. Textures: no creation hook needed; key the texture cache
on the fetch constants in the device shadow, as the emulator does.

Register shadow (each a copy of a Xenos register, written by the setters and
flushed by 822346A8): `dev + 0x2800 + (reg-0x2000)*4` for 0x2000-0x200F,
0x2840 for 0x2080-0x2082 (window offset/scissor), `0x284C + (reg-0x2100)*4`
for 0x2100-0x2119 (0x285C RB_COLOR_MASK, 0x2858 reset index), `0x28B4 +
(reg-0x2200)*4` for 0x2200-0x220B (0x28B4 RB_DEPTHCONTROL, 0x28B8/0x28D8..0x28E0
RB_BLENDCONTROL0-3, 0x28BC RB_COLORCONTROL, 0x28C4 PA_CL_CLIP_CNTL, 0x28C8
PA_SU_SC_MODE_CNTL). Saved render-state values (not registers) at 0x2D38..0x2D5C
and 0x2DEC..0x2DF8.

Not D3D (UE3, ignore): 82213EA0, 82214098, 821FBEE8, 821FC470, 8221CBA8
(UE3 dynamic ring-buffer allocator - waits on a GPU read pointer), 822598E0,
82214820, 8225DDA0, 8225C0E0.

Device layout: dirty masks +0x00 (VS consts, shadow +0x700), +0x08 (PS
consts, shadow +0x1700), +0x10 (regs 0x2200.., fetch consts shadow +0x400,
0x2180, shader binding), +0x18 (0x2100/0x2000/0x2280), +0x20 (0x2300/0x2380).
Bound objects: IB +0x2F84, RT0-3 +0x2F88, depth +0x2F98, streams +0x2F9C,
strides +0x2FE0, viewport +0x3058, scissor +0x3070, PS +0x3080, VS +0x3084,
raw blend words +0x2D38/+0x2D3C. Command buffer write ptr +0x28, limit +0x30.

Implications for the native backend:
- Snapshot state at draw time from the device shadow (constants at
  +0x700/+0x1700, fetch constants at +0x400, register shadows) instead of
  hooking every setter: UE3 inlines SetVertexShaderConstantF in at least 7
  places (82A0FDE8, 82A12BA8, 82A15718, 82A17C20, 82A1A8E8, 82A1B828,
  82A1D530), and SetVertex/PixelShader write shader literals into the same
  shadow. So: hook the draw calls (8222DE50, 8222D4F8, 8222CFF8, 8222DA48;
  all go through 822346A8), Clear, Resolve and Swap, and read the shadow.
- Queries must complete: UE3 busy-waits in GetData (~43k calls/frame) until
  the fence at dev+0x2A1C passes q[5] and the 0xFFFFFEED sentinels are
  replaced by ZPASS counts. Same for the UE3 ring allocator's GPU read pointer
  (0x82C0CB24 +0x14). The native backend must advance both.
- Raw PM4 writers (tiling, GPR change, swap copy, query writes) become no-ops
  or semantic equivalents; with the tiling patch most tiling paths are dead.

## UE3 RHI layer (XeD3DDrv, static analysis 2026-10-10)

Gears 1 has an early UE3 RHI: state-object setters and draw wrappers in one
cluster (0x8254E728-0x8254FF18) sitting directly on the D3D functions above.
Found via strings (`XeD3D`/`XeDrv` packages, `[XeD3D] DrawUPVertexCheckCount`)
and the callers of the D3D draw entry points (every one has 1-6 callers, all
in this cluster or a few engine draw paths). Names are by behaviour, matched
to the UE3 RHI naming in public UE3/UDK docs.

| Address | Callers | Likely RHI function | Evidence |
|---|---|---|---|
| 8254E728 | 1 | RHISetStreamSource | SetStreamSource 8222AE20, clears decl +0x2D10 |
| 8254E7A0 | 19 | RHISetRasterizerState | PA_SU_SC_MODE_CNTL (shadow +0x28C8) fill/cull; depth bias + slope via D3D setters 82229460/82229398, **negated when reversed-Z** |
| 8254E848 | 11 | RHISetViewport | SetViewport 8222ABF8; MinZ/MaxZ swapped when reversed-Z |
| 8254E8C8 | 3 | RHISetScissorRect | 8222AB30 |
| 8254E948 | 36 | RHISetBoundShaderState | SetVertexShader 82222B98 + SetPixelShader 82222808 |
| 8254E9E0 | 26 | RHISetSamplerState | SetTexture 82220858, address/filter setters 8222A2D8/8222A150, fetch-constant shadow +0x400 |
| 8254EB30 | 23 | RHISetDepthState | RB_DEPTHCONTROL (+0x28B4) z enable/write/func |
| 8254ED40 | 28 | RHISetBlendState | 9 D3D blend setters 82228998..82228CB8, RB_COLORCONTROL (+0x28BC) alpha test, alpha ref (+0x2884, /255) |
| 8254EE20 | 26 | RHISetRenderTarget | SetRenderTarget 8222B068, SetDepthStencilSurface 8222B398; sets reversed-Z flag 0x82BFA554 when the depth surface's format word (+0x28) is 0x1A220197 (float depth, presumably D24FS8) |
| 8254F0A0 | 1 | RHIDrawIndexedPrimitive | SetIndices 8222AFD8 + DrawIndexedVertices 8222DE50; prim-count table 0x8207DD20 |
| 8254F168 | 3 | RHIDrawPrimitiveUP | 8222CFF8 returns a write pointer, memcpy; limit 0x82BB6B48 |
| 8254F2B0 | 6 | RHIDrawIndexedPrimitiveUP | 8222D4F8 + two memcpys; limits 0x82BB6B48/4C |
| 8254F3D0 | 7 | RHIClear | color/depth/stencil flags 0xF/0x10/0x20; depth = 1 - z when reversed-Z |
| 8254F4C0 | 2 | tiling setup | walks the tile-rect presets 0x82083728 |
| 8254F850 | 1 | tiled resolve (RHICopyToResolveTarget path) | per tile of GTilingMode (0x82BB6B50): predication 82236370, Resolve 82235528 color + depth |
| 8254FF18 | 5 | GPU/swap sync helper | 82221D78 |

Caveats:
- Not every draw goes through the wrappers: 825C0010, 825C3F88, 825D6E08 and
  827DBB10 call D3D draws directly, and some engine code writes the register
  shadow inline (e.g. 825E2680 sets colour write mask +0x285C and dirty bits
  +0x18 itself). An RHI-only hook layer would miss those; D3D-level draw
  hooks + the register shadow still see everything.
- 8222CFF8/8222D4F8 are BeginVertices/BeginIndexedVertices (they return a
  write pointer); the API map is corrected.
- **Reversed-Z**: with the float depth surface UE3 flips depth (viewport
  MinZ/MaxZ swapped, clear to 1 - z, depth bias negated). Relevant to the
  decal/blood-pool z-fighting lead and to Canary cde5d85ec.

The binary names all global shader types (FDepthOnly*, FShadowDepth*,
TEmissive*<lightmap policy>, TLight*<light, shadow policy>, TTranslucency*,
FShadowVolume*, FModShadow*, post-process: bloom/DOF/motion blur/
uber/gamma) and vertex factories (Local, GPUSkin, Particle*, Terrain,
TerrainDecal) as UTF-16BE strings in .rdata, so the shader map tables
can tie dumped microcode to a shader type. Render passes in this build:
depth-only, emissive (lightmaps), per-light additive, shadow volumes
(stencil), modulated shadows, translucency, distortion, height fog,
post-process.

### Scene renderer map (named in Ghidra 2026-10-10)

Named by behaviour plus the shader types each function binds (found by
linking .data shader-type objects to the static initializers that register
their name strings). Each has a Ghidra plate comment with its evidence and
confidence.

```
FDrawSceneCommand_Execute 825550A8
└─ FSceneRenderer_Render 82553258            loop over depth-priority groups
   ├─ RenderPrePass 82553EA0                 + RenderOcclusionQueries 82491DD8
   ├─ (tiling: XeInitTilingRects, scene clear via 8238A598)
   ├─ RenderEmissive 82554588                → RHICopyToResolveTarget 825A90D0 / XeTiledResolve
   ├─ RenderLights 825C2138                  RenderShadowVolumes 825C0468,
   │                                         RenderProjectedShadows 825BD9E8
   │                                         (RenderDepth 825BBC40, RenderProjection 825BBFD8),
   │                                         RenderLightFunction 825E2680
   ├─ RenderModulatedShadows 825C28B0        RenderModulatedShadowVolumes 825C0A68
   ├─ RenderFog 825D94A0                     world DPG only
   ├─ RenderDistortion 825D7438              DrawDistortionApplyScreen 825D6E08
   ├─ RenderTranslucency 825D4868            → resolve
   └─ RenderGammaCorrection 825D9F78         per view, after all DPGs
Post-process proxies (vtable slot 0 = Render): DOFAndBloom 825205F8,
UberPostProcess 82544148, DOF 82521FF8, MotionBlur 825307D0, Bloom 82511180;
shared helpers DrawDenormalizedQuad 825B6168, FilterTexture 825B6B48.
Other: FBatchedElements_Draw 8254CFA0, FMeshDrawingPolicy_DrawMesh 825C3F88
(the main mesh path; calls D3D draws directly), DrawEmissiveMesh 825D1268
(picks the lightmap policy and draws), DrawTranslucentMesh 825D3F70, RHICopyFromResolveTarget 825A9818 (EDRAM restore by drawing),
FSceneRenderTargets_BeginRenderingSceneColor 825B3940 / _BeginRenderingShadowVolumes
825B3A78, RHIBegin/EndDrawingViewport 824873D8 / 824A5170.
```

Scene surfaces: scene color 0x82C0DE54, scene depth 0x82C0DE9C, shadow
volume/light attenuation RT 0x82C0DF2C, RHI device pointer 0x82C0DF74.
Per-mesh state: FMeshDrawingPolicy_SetMeshRenderState 825C3EF8 (mesh element
DepthBias +0xF0, SlopeScaleDepthBias +0xF4, flags +0xF8) →
RHISetRasterizerStateImmediate 8257BD98 (initializer copy of RHISetRasterizerState,
same reversed-Z negation).
Drawing-policy DrawShared functions (bind shaders + parameters, then the
caller runs FMeshDrawingPolicy_DrawMesh): TLightDrawingPolicy_{Directional,
Point,Spot}_{NoStaticShadowing,ShadowTexture,ShadowVertexBuffer}_DrawShared,
FDepthDrawingPolicy 825CE600, FHitProxyDrawingPolicy 825CE198,
FVelocityDrawingPolicy 825CF8D0, TDistortionMeshDrawingPolicy 825D8408,
FShadowDepthDrawingPolicy 825BB0A0, TTranslucencyDrawingPolicy_Blend
{Translucent,Additive,Modulate} 825D4A58/825D4B30/825D4C80. Pixel shader
parameter setters for fog, distortion apply, light function, motion blur, DOF,
simple elements; FShadowTexturePolicy_SetShadowTexture 825148F0;
TLightPixelShader_SetLightAttenuationTexture_* (4 folded layouts).

Deliberately left unnamed (evidence too weak): 825B43B8 (texture param
setter shared by several passes), 825B9BC0, 825BE308, 82555640, 82556900,
82556B48, 82556DC0 (emissive DrawShared variants; lightmap policy per
function not established), 825B71C8 and the nine uncalled
sampler+9-constant setters 825B8328..825B9720, 82491958, 825D9B50 (pass under
show-flag bit 1, called twice per DPG), 825CFD58 (sets RT 0x82C0DF50),
827DBB10 (raw D3D draw, likely movie/UI).

How the strings were found: Ghidra's analysis left the lis/addi string
references unresolved, so .text/.rdata/.data were dumped through the Ghidra
MCP socket and scanned for lis+addi/load pairs (session scratchpad, not in
the repo): 7946 of 11692 strings resolve to referencing functions.

Note: the XEX also statically links the XDK shader compiler/state machine
(`xgraphics\ucode\compiler`, `ssm`): don't use it as a source for anything
destined for Canary (no XDK-derived information).

## Risks

- Unidentified or inlined D3D calls (UE3 may write command-buffer state
  directly for hot paths). Observe mode in phase 0 measures this early.
- Xbox-specific D3D features: predicated tiling (disabled via patch),
  memexport, tessellation, EDRAM-specific tricks (e.g. aliasing render
  targets to reinterpret formats). The emulated path's GFXR captures show
  which ones Gears actually uses.
- Shader translation gaps (relative addressing, predication, vfetch formats).
- Scale: hundreds of shaders, every rendering feature reimplemented.

## Open questions

- XenosRecomp vs ReXGlue translator: prototype both on the skinning VS
  (Canary, which shares Xenia's translator, shows the same aim-down-sights
  spikes on this NVIDIA card; its other upstream issues were the player
  going semi-transparent while idle and seeing through some walls).
- Backend: decided 2026-10-14, SDL_GPU (shaders as SPIR-V, MSL via
  SPIRV-Cross for Metal). Checked against SDL 3.5's SDL_gpu.h: no access to
  the underlying Vulkan handles (only SDL_GPUVulkanOptions to enable extra
  extensions/features at device creation), so SDL_GPU and raw Vulkan can't
  be mixed; no occlusion queries; depth bias is plain constant/slope/clamp
  (on float depth it shrinks at low depths, the blood-pool problem). Plan:
  shader-side depth offset for UE3's decal draws, skip the occlusion query
  pass (draw everything). Once Gears owns its SDL build (no ReXGlue), a small
  SDL patch or an upstream SDL feature is the fallback.
- MSAA resolve and depth formats in SDL_GPU (checked 2026-10-14, SDL 3.5
  header + a query program on the RTX 4080 / NVIDIA 615.78.08, Vulkan):
  - Colour resolve: only as a render pass store op (RESOLVE /
    RESOLVE_AND_STORE) into a 1x texture's mip/layer at the end of the pass.
    Xbox Resolve to a sub-rectangle or offset = resolve, then
    SDL_CopyGPUTextureToTexture / SDL_BlitGPUTexture for the region.
  - Depth/stencil resolve: not supported ("depth/stencil targets do not
    support multisample resolves"). UE3's sample-0 depth resolve needs either
    1x rendering (GTilingMode 4, what gears1_black_shading_fix already
    selects, has no 2x MSAA) or reading sample 0 in a shader, which SDL_GPU
    forbids: "For multisample textures: usage cannot contain SAMPLER or
    STORAGE flags" (SDL_CreateGPUTexture assertion with the debug device;
    without debug mode the creation is not validated and only looks like it
    works). So no shader access to individual MSAA samples at all, colour or
    depth. Plan: render at 1x and use post AA (FXAA/SMAA-style) instead of
    MSAA. Native MSAA would need an SDL patch (Vulkan and Metal both support
    sampled multisample images) once Gears owns its SDL build.
  - Depth formats: SDL guarantees only D16 plus either D24 or D32F, and
    either D24S8 or D32FS8, so query SDL_GPUTextureSupportsFormat. This GPU
    has all five, at 1x/2x/4x/8x and with SAMPLER usage. Gears needs stencil
    (shadow volumes): D24_UNORM_S8 where available gives depth bias in
    constant units (no decal Z-fighting, no shader offset needed); where only
    D32_FLOAT_S8 exists (Apple GPUs have no D24S8) use the shader-side decal
    offset.
  - Colour formats Gears needs that exist: R10G10B10A2_UNORM (2_10_10_10),
    R16G16B16A16_FLOAT (for 7e3 / 2_10_10_10_FLOAT), BC1-3 (DXT), sRGB 8888.
    No PWL gamma or 7e3 format: convert in shaders as ReXGlue does.
