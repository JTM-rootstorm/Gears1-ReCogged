# Probe hooks (UE3 RHI + D3D)

Cheap, cvar-gated probes in the recomp, so in-game investigations can answer
"which pass, which policy, which state, which shader" without GFXR round
trips. Phases 1 and 2 are implemented; phase 3 is planned. Function names
refer to the Ghidra project (NATIVE_RENDERER.md "UE3 RHI layer" and "Scene
renderer map").

## Mechanism

Every recompiled function is a weak `sub_XXXXXXXX`; a strong definition in
our sources replaces it and calls `__imp__sub_XXXXXXXX` (the original).
Indirect calls (vtables, function pointers) go through the same registered
symbol, so virtual Render methods hook the same way. Arguments: `ctx.r3..r10`,
`ctx.f1..`; guest memory is big-endian.

One strong override per symbol, so every override comes from
tools/gen_hooks.py, which also keeps the per-frame D3D call counts
(`gears1_d3d_observe`).

## Usage

tools/hook_table.py lists what is hooked and documents the kinds; run
`python3 tools/gen_hooks.py` to regenerate src/hooks_gen.cpp. The probes are
in src/probes.cpp. All cvars default off; with them off each hook costs one
atomic load.

| Cvar | Default | Effect |
|---|---|---|
| `gears1_probe` | false | one line per frame: draws per innermost pass |
| `gears1_probe_trace_file` | probe_trace.log | output (relative to the working directory); appended, one `# run start` header per run |
| `gears1_probe_trigger_file` | probe_trigger | create it (`touch`) to trace the next N frames; deleted when seen |
| `gears1_probe_trace_frames` | 1 | N |
| `gears1_probe_skip` | "" | comma list of hook_table names: passes/scopes return early, policies skip their DrawMesh calls |
| `gears1_probe_depth_bias_scale` | 1.0 | scales the r4 value of D3D SetDepthBias / SetSlopeScaleDepthBias |
| `gears1_probe_depth_bias_negate` | false | negates it |

Frame boundary: D3D Swap (the hook table's `frame` entry). Settings are
re-read there, so the first frame after a change still uses the old ones.
UE3 renders on its own thread: the pass stack is thread_local, the trace
output is shared and flushed at the frame boundary.

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

## Not hooked yet

Add these to the hook table when their arguments are needed.

| Address | Function | Would give |
|---|---|---|
| 8254EE20 | RHISetRenderTarget | color/depth surface pointers, reversed-Z flag 0x82BFA554 after the call |
| 8254EB30 / 8254ED40 / 8254E7A0 | RHISetDepthState / BlendState / RasterizerState | z func/write, blend factors, fill/cull/bias |
| 8257BD98 | RHISetRasterizerStateImmediate | per-mesh fill/cull/bias |
| 825C3EF8 | FMeshDrawingPolicy_SetMeshRenderState | mesh element DepthBias (+0xF0), SlopeScaleDepthBias (+0xF4), flags (+0xF8); DRAW lines already read them via DrawMesh |
| 8254E948 | RHISetBoundShaderState | VS/PS objects |
| 8254E9E0 | RHISetSamplerState | texture fetch constant / format |
| 822346A8 | D3D_CommitDirtyState | per-draw register shadow including state written inline by engine code (e.g. 825E2680 colour mask) |

RHIEnd/BeginDrawingViewport (824A5170 / 824873D8) are not used as the frame
boundary: not confirmed to run once per frame.

Direct D3D users outside the RHI (31 functions): FSceneRenderer_Render and
the passes (Clear/SetViewport/SetRenderTarget), RHICopyToResolveTarget,
RHICopyFromResolveTarget, 825A94C0, 825C0010 (direct DrawIndexedVertices,
shadow-volume vtable method), 8249B038 (splash/loading draw with
DrawVerticesUP), 824A4D48 (back buffer setup), 82487510, and 827DA1D0 /
827DB800 / 827DB910 / 827DBB10 (raw D3D draws with their own shaders; likely
movie/UI, unnamed). The D3D draw hooks see their draws anyway; the pass stack
shows them as outside the scene renderer.

## Phase 3: GPU markers

The game-side hook records (cmd buffer guest address, label) in a ring; the
SDK command processor emits vkCmdBegin/EndDebugUtilsLabelEXT when execution
passes that address, so GFXR/RenderDoc captures show pass names. Needs a
small diagnostic SDK hook (keep it out of upstream commits unless made
generic). Rejected: writing PM4 NOPs into the guest command buffer (needs
D3D's space reservation; corruption risk).
