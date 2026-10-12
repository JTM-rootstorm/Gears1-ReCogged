# Probe hooks plan (UE3 RHI + D3D)

Goal: cheap, cvar-gated probes in the recomp so in-game investigations
(blood pool, future render bugs, native renderer work) can answer "which pass,
which policy, which state, which shader" without GFXR round trips. Status
(2026-10-10): phases 1 and 2 implemented (see Usage); phase 3 planned. Function names refer to the Ghidra project (see
NATIVE_RENDERER.md "UE3 RHI layer" and "Scene renderer map").

## Mechanism (already proven)

Every recompiled function is a weak `sub_XXXXXXXX`; a strong definition in
our sources replaces it and calls `__imp__sub_XXXXXXXX` (the original).
Indirect calls (vtables, function pointers) go through the same registered
symbol, so virtual Render methods hook the same way. Arguments: `ctx.r3..r10`,
`ctx.f1..`; guest memory is big-endian at `base + addr`.

Constraint: one strong override per symbol, so every override comes from
tools/gen_hooks.py (it replaced gen_d3d_observe.py and keeps its per-frame D3D
call counts and the shader dump pre hooks).

## Usage (phases 1-2)

Files: tools/hook_table.py (what is hooked) -> `python3 tools/gen_hooks.py`
-> src/hooks_gen.cpp (generated); src/probes.cpp (probes), src/probes.h.
All cvars default off; with them off each hook costs one atomic load.

| Cvar | Default | Effect |
|---|---|---|
| `gears1_probe` | false | one line per frame: draws per innermost pass |
| `gears1_probe_trace_file` | probe_trace.log | output (relative to the working directory); appended, one `# run start` header per run |
| `gears1_probe_trigger_file` | probe_trigger | create it to trace the next N frames; deleted when seen |
| `gears1_probe_trace_frames` | 1 | N |
| `gears1_probe_skip` | "" | comma list of hook_table names: passes/scopes return early, policies skip their DrawMesh calls |
| `gears1_probe_depth_bias_scale` | 1.0 | scales the r4 value of D3D SetDepthBias / SetSlopeScaleDepthBias |
| `gears1_probe_depth_bias_negate` | false | negates it |

Frame boundary: D3D Swap (8223E860). Settings are re-read there, so the
first frame after a change still uses the old ones.

Trace lines (`F<frame>` prefix):
- `ENTER <pass> r3 r4 r5 in=<stack> pol=<policy>` / `LEAVE <pass>`
- `DRAW <D3D draw> args=r4..r7 in=<stack> pol=<policy> vs ps rt ds depth
  blend color mode mask poly=<scale,offset> revz mesh=<element bias=..
  flags=..> cmd=<cmd buffer write ptr>`: register-shadow values
  (RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, PA_SU_SC_MODE_CNTL,
  RB_COLOR_MASK) and the poly offset the D3D setters wrote (dev+0x29D0/4);
  shader hashes match shader_dump file names (0 until the shader is rebound
  after the trace starts).
- `EVENT <Resolve|Clear|SetRenderTarget|SetDepthStencilSurface|SetDepthBias|
  SetSlopeScaleDepthBias> r3..r8 f1` (raw arguments; the depth bias value is
  the float bits in r4).

Caveats: draws issued with BeginVertices are counted once at Begin; a skipped
pass returns 0 (Emissive/Distortion/Translucency return "scene color dirty").

## Design

- `tools/gen_hooks.py` (replaces gen_d3d_observe.py): reads a hook table
  (`tools/hook_table.py`, plain Python: address, name, kind, extra) and emits
  `src/hooks_gen.cpp` with one override per address. Kinds:
  - `count`: the current observer behaviour (per-frame call counts).
  - `scope`: `ProbeEnter(id)` / call / `ProbeLeave(id)`; pushes a
    thread_local pass/policy stack.
  - `pre` / `post`: call a hand-written C++ hook before/after the original
    (shader dump, state decoders, overrides).
  - `skippable`: `if (ProbeSkip(id)) { set return value; return; }`.
- `src/probes.cpp` (hand-written): probe state, trace writer, decoders,
  overrides, skip list, frame boundaries. `src/shader_dump.cpp` stays and is
  called as a `pre` hook.
- All off by default; zero cost beyond one cvar check when off (same as the
  observer today).

### Cvars

| Cvar | Purpose |
|---|---|
| `gears1_probe` | master switch (pass stack, counters) |
| `gears1_probe_trace_file` | trace output path |
| `gears1_probe_trigger_file` | when this file appears, trace the next N frames, then delete it (works mid-game without input hooks; `touch` from a terminal) |
| `gears1_probe_trace_frames` | N (default 1) |
| `gears1_probe_skip` | comma list of hook names to skip (passes/policies only) |
| `gears1_probe_depth_bias_scale` / `_negate` | game-side depth bias override |
| `gears1_probe_gpu_markers` | phase 3: Vulkan debug labels per pass |

Threading: UE3 renders on its own thread; the pass stack and trace buffer
are thread_local, flushed at RHIEndDrawingViewport (frame boundary).

## Hook table

Tier 1, frame and scene structure (`scope`):

| Address | Function |
|---|---|
| 8223E860 | D3D Swap: frame boundary, trigger poll (RHIEnd/BeginDrawingViewport 824A5170 / 824873D8 not used: not confirmed per frame) |
| 825550A8 | FDrawSceneCommand_Execute |
| 82553258 | FSceneRenderer_Render |
| 82553EA0, 82554588, 825C2138, 825C28B0, 825D94A0, 825D7438, 825D4868, 825D9F78 | the passes (PrePass, Emissive, Lights, ModulatedShadows, Fog, Distortion, Translucency, GammaCorrection); `skippable` with care: Emissive and Distortion return a dirty flag in r3 |
| 825C0468, 825C0A68, 825BD9E8, 825BBC40, 825BBFD8, 825E2680 | shadow volumes / projected shadows / light function sub-passes |
| 825205F8, 82544148, 82521FF8, 825307D0, 82511180 | post-process proxy Render (vtable-called) |
| 8254CFA0 | FBatchedElements_Draw |

Tier 2, what is being drawn (`scope`, `skippable`):

| Address | Function |
|---|---|
| 825C3F88 | FMeshDrawingPolicy_DrawMesh (the main mesh draw; tags the draw) |
| 825C3EF8 | FMeshDrawingPolicy_SetMeshRenderState: `pre` hook records the mesh element's DepthBias (+0xF0), SlopeScaleDepthBias (+0xF4), flags (+0xF8) |
| 825D1268, 825D3F70 | DrawEmissiveMesh, DrawTranslucentMesh |
| the 9 TLightDrawingPolicy_*_DrawShared, 825CE600, 825CE198, 825CF8D0, 825D8408, 825BB0A0, 825D4A58/4B30/4C80 | DrawShared per policy (records the policy for the next draws) |

Tier 3, state (`pre` decoders, args are state objects or initializers):

| Address | Function | Decoded |
|---|---|---|
| 8254EE20 | RHISetRenderTarget | color/depth surface pointers, reversed-Z flag 0x82BFA554 after the call |
| 8254EB30 / 8254ED40 / 8254E7A0 | RHISetDepthState / BlendState / RasterizerState | z func/write, blend factors, fill/cull/bias |
| 8257BD98 | RHISetRasterizerStateImmediate | per-mesh fill/cull/bias |
| 8254E948 | RHISetBoundShaderState | VS/PS objects → shader hashes (shader_dump hashing) |
| 8254E9E0 | RHISetSamplerState | texture fetch constant / format |

Tier 4, D3D (existing observer counts kept; these get `pre`/`post`):

| Address | Function | Why |
|---|---|---|
| 822346A8 | D3D_CommitDirtyState | per-draw snapshot of the register shadow: catches state written inline by engine code (e.g. 825E2680 colour mask) |
| 8222DE50, 8222DA48, 8222CFF8, 8222D4F8 | draws | count per pass/policy; record cmd buffer write pointer (dev+0x28) to correlate with GFXR/PM4 |
| 82229460 / 82229398 | SetRenderState_DepthBias / SlopeScaleDepthBias | the single choke point for all depth bias (both rasterizer setters call them): apply `gears1_probe_depth_bias_scale` / `_negate` here |
| 82235528, 8222CC48 | Resolve, Clear | resolve/clear targets per pass |
| 8223E860 | Swap | existing frame counter |

Direct D3D users outside the RHI (31 functions; hook only when needed):
FSceneRenderer_Render and the passes (Clear/SetViewport/SetRenderTarget),
RHICopyToResolveTarget, RHICopyFromResolveTarget, 825A94C0, 825C0010
(direct DrawIndexedVertices, shadow-volume vtable method), 8249B038 (splash/
loading draw with DrawVerticesUP), 824A4D48 (back buffer setup), 82487510,
and 827DA1D0 / 827DB800 / 827DB910 / 827DBB10 (raw D3D draws with their own
shaders; likely movie/UI, unnamed). Tier 4 draw hooks see their draws
anyway; the pass stack will show them as "outside scene renderer".

## Trace format

One line per event, only while tracing:
```
F<frame> T<tid> <pass>/<policy> DRAW prim=<n> vs=<hash> ps=<hash> rt=<surf> ds=<surf> z=<func,write> blend=<..> cull=<..> bias=<const,slope> meshbias=<+F0,+F4> revz=<0|1> cmd=<guest addr>
F<frame> ... ENTER RenderTranslucency dpg=1
F<frame> ... RESOLVE src=<surf> dst=<tex> rect=<..>
```
Per-frame summary (when `gears1_probe` without tracing): draws per pass.

## Phases

1. Done: generator + scope hooks + trigger-file trace (tiers 1, 2, 4).
2. Done, simplified: the draw-time register-shadow snapshot replaces
   separate tier 3 decoders (RHI state setters are not hooked yet; add them
   to the table if their arguments are needed); depth bias override; skip
   list.
3. GPU markers: the game-side hook records (cmd buffer guest address, label)
   in a ring; the SDK command processor emits vkCmdBegin/EndDebugUtilsLabelEXT
   when execution passes that address, so GFXR/RenderDoc captures show pass
   names. Needs a small diagnostic SDK hook (keep out of upstream commits
   unless made generic). Rejected: writing PM4 NOPs into the guest command
   buffer (needs D3D's space reservation; corruption risk).

## First use: blood pool

1. Trigger-file trace on a frame with a glitching pool: find the decal draws
   (policy, pass, mesh depth bias, reversed-Z, depth func).
2. Game-side `gears1_probe_depth_bias_scale` vs host `vulkan_depth_bias_scale`:
   same result → host translation of a correct guest bias; different →
   guest-side value.
3. `gears1_probe_skip` on the decal's policy to confirm the draw identity.
