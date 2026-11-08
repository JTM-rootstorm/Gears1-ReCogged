# Gears of War (360) recomp — findings

Running log of what we've learned. Rules live in CLAUDE.md; this file is facts.

## Status (2026-10-09)
- PLAYABLE into gameplay with the patched plugin (RTE off): New Game → intro
  cutscene → gameplay, no GPU fault (driver 615.78.08, log 021). Perf is
  inconsistent (Debug build; 551 pipelines compiled in ~90 s).
- Rendering bugs seen in gameplay (user report): red tint over the scene
  appearing without damage (so not the Crimson Omen overlay), incorrect
  lighting, some geometry lit/shaded strangely, halo/outline around some
  models. Next: GFXR capture of a frame showing them (GFXR_FRAMES=3
  tools/gfxr_capture.sh .../redtint), replay with --screenshots and
  dump-resources per pass; compare stock vs RTE-stripped shaders to rule out
  the RTE workaround; suspects: fbo render-target path / EDRAM resolve and
  formats, half-pixel offset, MSAA emulation.
- Red tint analysis (captures in gears1_captures/redtint/, cell/opening scene):
  - Not the RTE workaround: re-adding RoundingModeRTE to all 539 modules gives
    byte-identical frames 1-2 (frame 3 hits the NVIDIA fault again).
  - Tinted (164217) vs untinted (164321) frames share the exact same
    post-processing chain (same pipelines/shaders) and identical UE3 post
    constants (tools/gfxr_ubo.py), so the game isn't requesting a tint.
  - Per-pass render-target dumps (dump-resources) diverge at the plugin's
    render-target TRANSFER draw (pipeline 29876, FS module 29875 =
    xe_transfer_color: OpImageFetch from a uint MSAA view), which re-interprets
    EDRAM data from the R16G16B16A16_SFLOAT HDR targets into the R8G8B8A8
    1280x2048 target. That's the fbo path's approximation of EDRAM aliasing →
    suspected cause of tint/lighting/halos.
  - FSI (exact EDRAM emulation) assert root cause: with dynamic rendering,
    VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass
    (command_processor.cpp ~3164) calls GetLastUpdateRenderingAttachments() →
    last_update_accumulated_render_targets() asserts Path::kHostRenderTargets.
    The legacy render-pass branch doesn't. Next test (user):
    `--render_target_path_vulkan=fsi --vulkan_dynamic_rendering=false`.
  - FSI + legacy render passes boots to title (30 s smoke test, 314 pipelines,
    no assert, no Xid). Gameplay comparison pending.
  - 7 captures (redtint/, 3 frames each; contact sheet of frame 1 via
    --screenshots 1): 1 deep red cell; 2-3 correct; 4-6 red cast plus
    salmon/pink blotches on walls/floor (lighting/decoding); 7 heavy red wash
    under fire. Geometry bugs not visible in frame 1s.
  - "FSI crashes in the opening scene" was my own 30 s smoke test timing out
    (no log/Xid/core after 16:49:51). Real FSI gameplay test still pending.
- Black spikes from player/gun toward screen centre (captures 5 and 6):
  tools/gfxr_const_scan.py on capture 6 frame 1 (11411 draws, 91 with a
  relative-addressing/skinning VS): skinned draws 213505, 213520, 213525
  (~5.2k indices, pipes 993/911/940, VS 455/454) have 176-192 of 256 VS float
  constant rows (set 1 b1) full of garbage — NaN, ±1e36, denormals like
  1.4e-45 / -1.5e-36 = guest ints/pointers (0x83xxxxxx) read as floats.
  Bone matrices are garbage → exploded vertices. 213494 has 9 bad rows.
  (3.4e38 rows in plugin transfer draws, VS 106, are intentional FLT_MAX.)
  Constants come from the wrong memory: either recomp CPU code (bone buffer
  address/contents) or the plugin's PM4 constant loading (LOAD_ALU_CONSTANT
  address/size). Next: PM4-level trace of those draws' constant sources.
- FSI gameplay run (log 024, `--render_target_path_vulkan=fsi
  --vulkan_dynamic_rendering=false`): clean log, no Xid. User: FIXES the red
  tint and much of the bad lighting; new issue: blocky shadows; spikes get
  WORSE (slower GPU side → more garbage bone constants → suggests the plugin
  reads constant memory after the game has reused it, i.e. a timing issue).
- Plugin LOAD_ALU_CONSTANT (command_processor.cpp) reads guest memory at PM4
  processing time, same as upstream Xenia.
- CORRECTION (spikes): the "garbage bone constants" conclusion above was
  wrong. `gpu_log_bad_constants` run (log 025, FSI, spikes seen): 0 reports →
  the game sends sane float constants. The garbage rows in the capture are
  stale registers the plugin uploads for every relative-addressing shader
  (it uploads all 256), not proof those rows are used. Characters aren't
  drawn by the relative-addressing draws at all.
- CAPTURE PITFALL: in these 3-frame trimmed captures every present shows the
  front buffer resolved BEFORE the trigger (frame 3 is pixel-identical with
  all draws disabled except the presenter blit, draw 235117 = 4-vertex strip
  into renderPass 52). The frame rendered in-capture (one ~11k-draw command
  buffer, submit 235153) is presented after the capture ends. So screenshot
  tests on these captures (poison-cull, exploded-vertex cull, skinned-draw
  disable) were INVALID. gfxrecon-replay --dump-resources segfaults on that
  cb. Need captures with more frames (GFXR_FRAMES>=8) so a present shows
  in-capture rendering; then tools-style screenshot bisection works (16
  replays ≈ 20 s on capture 6).
- 10-frame spike captures (gears1_captures/spikes/, 171135 and 171408):
  later frames do show in-capture rendering. Capture 171408 frame 10 has the
  long dark bar across the doorway (grey 12, same flat value as the slab;
  x≈2445-2515 at y=1600). tools/gfxr_artifact_bisect.py (recursive split,
  treats a black screen as "blind"): frame 10 depends only on draws
  ≥239911; the bar needs ≥2 draws (window 265526-273437 removes it, neither
  half does), and every "gone" result also brightened the whole scene
  (44→55): the bar behaves like a lighting/shadow-term artifact, not a
  separate dark object. Draw 272813 (72 idx, HDR RGBA16F 1280x1024 +
  D32S8, depth GEQUAL + write) only changed it indirectly.
  Working hypothesis: shadow-map depth precision (Xenos D24FS8 = 20e4 float
  depth emulated with host D32F) → shadow test errors streaking as
  wedges/bars; FSI's blocky shadows fit too. Live test:
  `--depth_float24_convert_in_pixel_shader=true --depth_float24_round=true`
  (both default false, src/graphics/flags.cpp).
- Live test, fbo + depth_float24_convert_in_pixel_shader + depth_float24_round:
  mixed. Some lighting now matches FSI; more flickering black geometry;
  red now appears only IN SHADOWS (not whole screen); spikes seem worse when
  aiming (camera zoom → depth distribution changes). Reading: red = colour
  data aliased into lighting/shadow buffers via the fbo transfer path (FSI
  has none); depth precision matters but fbo tuning isn't the way forward.
  Spikes exist on BOTH paths → likely in shared code (resolve / depth-texture
  sampling of shadow maps, texture_cache), not the RT path. Next: reference
  comparison against upstream Xenia Canary (Vulkan, FSI) in the same scene.
- Reference emulator: Xenia Canary 02d2cb5 (2026-03-24, latest release,
  github.com/xenia-canary/xenia-canary-releases) at
  ~/Documents/Programming/Recomp/Gears/xenia-canary/02d2cb5/build/bin/Linux/Release/xenia_canary
  (portable.txt → config/saves stay in that folder). Run with --gpu=vulkan and
  --render_target_path_vulkan=fbo|fsi on the same default.xex.
- Config default stays fbo (user's choice); FSI is opt-in via flags.
- Canary won't start here: "Unable to allocate code cache generated code
  storage" = its JIT maps a /dev/shm file PROT_EXEC, and /dev/shm is mounted
  noexec (EPERM). Workaround: `sudo mount -o remount,exec /dev/shm` for the
  session (revert with remount,noexec). The recomp doesn't need exec shm.
- Canary also needs XWayland on this Wayland desktop (GDK_BACKEND=x11;
  log: "GTKWindow: The window system of the GTK window is not supported").
- UPSTREAM CONFIRMATION: Canary FBO dies with the identical fault (Xid 31,
  GPCCLIENT_GCC, 0x3_F8000000, 17:32:03; log "Failed to wait for a Vulkan
  fence: ErrorDeviceLost"). The RTE/NVIDIA bug is in upstream Xenia too,
  not introduced by ReXGlue. Canary has no option to disable RTE.
- tools/vk_strip_rte/: a Vulkan layer (VK_LAYER_GEARS1_strip_rte) that drops
  OpCapability/OpExecutionMode RoundingModeRTE from every shader module at
  vkCreateShaderModule. `tools/vk_strip_rte/run.sh <cmd>` builds it on demand
  and enables it via VK_ADD_LAYER_PATH + VK_INSTANCE_LAYERS. Verified:
  single_draw.gfxr replays with 0 Xid through it. Works for unmodified Canary
  or a stock ReXGlue plugin.
- CANARY FBO (with strip-RTE layer): normal, expected lighting and shadowing,
  no red tint. Its only issues: player character semi-transparent while
  idle (solid when moving), seeing through some walls (depth), and the
  spikes when aiming down sights. → Spikes + transparency are UPSTREAM Xenia
  bugs; red tint + bad lighting are REXGLUE REGRESSIONS on the fbo path.
- Canary src (xenia-canary @ 02d2cb5, sparse src/xenia/gpu) cloned at
  ~/Documents/Programming/Recomp/Gears/xenia-canary/src. Diff of the Vulkan
  render target cache (clang-format + rename-normalised): 159 hunks; in
  GetTransferShader ReXGlue adds gamma_render_target_as_unorm16 (8_8_8_8_GAMMA
  stored as R16G16B16A16_UNORM with PWL gamma<->linear in transfers, default
  ON, ReXGlue-only) and value-converting 16-bit packs (half/SNORM) where
  Canary moves raw bits. On NVIDIA the 16-bit ownership views are still
  R16G16(B16A16)_UINT (integer MSAA sampling supported), so the raw-bit path
  holds; prime suspect = gamma-as-unorm16. A/B: --gamma_render_target_as_unorm16=false.
- gamma_render_target_as_unorm16=false (log 028): no change (red tint and
  lighting the same) → not the cause.
- Per-file divergence vs Canary (normalised diff, changed lines): ALU
  translator 26, fetch translator 113, translator.cpp 43, but
  vulkan/command_processor 3264, vulkan/pipeline_cache 2276,
  vulkan/texture_cache 1507, vulkan/render_target_cache 1147,
  spirv_translator 1116, spirv_translator_rb 785, texture/cache 568.
- BUG FOUND (ReXGlue-only): spirv_translator_fetch.cpp texture fetch applied
  the result exponent bias from bits 13:18 of fetch-constant WORD 4 (inside
  lod_bias, bits 12-21) instead of WORD 3 (exp_adjust, per xenos.h dword_3
  +13). Canary uses word 3. Effect: every texture sample scaled by 2^(slice
  of the LOD bias), and real exp_adjust ignored → wrong lighting/brightness
  plausible. Fixed on the SDK branch (load word 3, as Canary). Vertex fetch
  exp_adjust path is identical to Canary. Pending live test (fbo + fsi).
- The transfer shader where the tint first showed (FS 29875) is a faithful
  64bpp→32bpp bit reinterpretation, same scheme as Canary — not itself buggy.
- CANARY FSI = Canary FBO (normal lighting/shadows; same upstream
  transparency + aim spikes). So ReXGlue's blocky FSI shadows are ALSO a
  ReXGlue regression. ReXGlue-only list: fbo red tint + lighting; fsi
  blocky shadows; fsi + dynamic rendering assert.
- exp_adjust fix live test (logs 029/030, patched plugin confirmed staged
  17:45): NO change on fbo or fsi. Kept (it matches Canary) but not the cause.
- Formatting-insensitive diff tool: tools/tokdiff.py canary.cc rexglue.cpp.
  ALU translator identical; fetch differs only in resolution-scale code;
  FSI blend differs for MIN/MAX (Canary applies factors), but no capture uses
  MIN/MAX blending, so not relevant.
- ReXGlue maps guest k_2_10_10_10 render targets to A8B8G8R8_UNORM (Canary:
  A2B10G10R10_UNORM). Gears uses one (1280x2048, the dest of the transfer
  where the tint first showed: 7e3 HDR -> 2_10_10_10 alias). Lossy (10->8 bit).
- ReXGlue texture cache drops signed k_2_10_10_10 sampling (returns 0);
  Canary samples A2B10G10R10_SNORM.
- PRIME SUSPECT: GPU cvar defaults that differ from Canary/upstream:
  execute_unclipped_draw_vs_on_cpu canary=true rex=false (ReXGlue's own
  comment keeps upstream's text: without it unclipped full-screen draws may
  claim the whole EDRAM -> spurious LOSSY ownership transfers corrupting other
  RTs; fbo-only, FSI has no transfers), gpu_allow_invalid_fetch_constants
  canary=true rex=false (invalid fetch -> black), anisotropic_override
  canary=-1 rex=3, non_seamless_cube_map canary=true rex=false,
  clear_memory_page_state canary=false rex=true, readback_memexport
  canary=false rex=true, primitive_processor_cache_min_indices canary=4096 rex=0.
  Test: run with all Canary defaults; bisect if it helps.
- Canary-defaults run, fbo (log 031, clean): RED TINT GONE. Still: black
  spikes, wrong lighting. Noticed: some main-menu elements drawn at wrong depth,
  covering others (new vs. pre-existing not yet known). Next: bisect which of
  the 7 flags removes the tint (expect execute_unclipped_draw_vs_on_cpu).
- Canary-defaults run, fsi: shadows STILL blocky (so not aniso / invalid
  fetch constants), lighting more correct than fbo, heat haze (invisibility)
  works on fsi but not fbo, spikes harder to trigger, no menu-cog depth bug.
- SDK patch: k_2_10_10_10(_AS_10_10_10_10) RT host format back to
  A2B10G10R10_UNORM_PACK32 (Canary). Plugin rebuilt + staged. Pending fbo test
  for lighting / heat haze.
- Both the A8B8G8R8 mapping and the execute_unclipped_draw_vs_on_cpu=false
  default date from the SDK's "Initial Commit" (96bf707, 2026-01-22): no
  commit message explains either change.
- execute_unclipped_draw_vs_on_cpu=true ALONE fixes the red tint (now in
  config/gears1_recomp.toml). 10-bit RT fix and --vulkan_dynamic_rendering=false
  do NOT fix fbo lighting / heat haze / menu-cog depth. Cog depth bug is
  ReXGlue-only (Canary cog fine), fbo-only, pre-existing; same family as the
  opening-scene enemy drool drawn at wrong depth vs the model.
- Menu captures (gears1_captures/menu = recomp fbo, menu_canary = Canary fbo,
  10 frames each; tools/gfxr_capture_canary.sh, tools/gfxr_draw_diff.py):
  per-draw fixed-function state (depth test/write/compare, blend, viewport
  depth range, depth bias, attachment formats) is IDENTICAL for all aligned
  draws. So the bug is in shader output, constants, textures or transfers,
  not pipeline state. Cog = 2700-index draw into 2xMSAA RGBA16F + D32S8,
  followed by small quads (ghost "GEARS OF WAR" text) in the same pass.
- VS constants b1 rows 7-10 = view-projection. Menu camera tilts over time in
  Canary (rotation terms 0 in early frames, non-zero later); recomp capture
  stayed level - timing, not a math bug. Camera zoom advances ~15x less per
  frame in the recomp than in Canary (game time slower per frame?).
- Cog bug = no depth of field on the cog (user). Final menu composite
  (recomp draw blk 12263 / Canary 19372, RGBA8 1280x720) blends sharp scene
  with the 400x6560 blur chain; Canary blurs the cog, recomp leaves it sharp.
  Blur chain looks alike in both. Composite samples a 1280x720 R32_SFLOAT
  scene-depth texture (k_24_8_FLOAT resolved from EDRAM, loaded by compute
  shader module 170 recomp / 156 Canary).
- RULED OUT with replay A/B on the composite's dumped output (dump-resources +
  --replace-shaders; sensitivity verified - a wrong loader changes 893k px):
  Canary's depth texture-load shader, and 15 matched compute shaders
  (resolve/dump/load), give a bit-identical composite. Runtime dump shaders
  match Canary except unused constants.
- R16G16_SNORM pass with the cog = motion-blur velocity (current + previous
  view-projection in VS consts), not DOF. Velocity scale 12x higher in recomp,
  camera drift 15x slower per frame: shorter recomp frame time under capture.
- PITFALL again: recomp menu-capture screenshots of frames 8/9/10 are identical
  (stale front buffer). Measure via dump-resources of a draw's output instead.
- GFXR 1.0.5 dump-resources ignores DumpDepth/DumpAllDescriptors/DumpRawImages
  options (only color attachments, bmp). New tools: gfxr_draw_diff.py,
  gfxr_draw_context.py, gfxr_draw_textures.py.
- Next: compare the TRANSLATED guest shaders (cog VS/PS, composite PS) between
  recomp and Canary: same microcode, two translators.
- Canary's translated PSs declare gl_SampleMask unconditionally (non-FSI) but
  the cog PS never writes it; ReXGlue omits it when EarlyFragmentTests is set.
  Removing it from Canary's cog PS in replay: composite bit-identical -> not
  the cause (tools/spv_drop_interface.py).
- Translated-shader comparison in progress (cog VS/PS: recomp modules 590/844
  vs Canary 445/575; composite PS 939 vs 623); disassembly in scratchpad/xlate.
- ROOT CAUSE FOUND (menu cog / "covers effects" / no DOF look, fbo only):
  ReXGlue's SPIR-V translator writes pixel-shader depth (oDepth) to host
  float24 depth buffers in guest units [0,1], but host float24 depth is stored
  as guest*0.5 (draw_util full_float24_in_0_to_1 viewport remap). The DXBC
  translator scales oDepth by 0.5 when kSysFlag_DepthFloat24
  (CompletePixelShader_DSV_DepthTo24Bit); the Vulkan path didn't.
  Gears' UE3 restores scene depth each frame with a full-screen pass whose PS
  writes oDepth from the resolved depth texture (menu_select capture, draw
  #24, blk 13464) -> restored depth = 2x -> everything restored looks nearer
  -> later smoke/fog/fire/text draws fail GEQUAL in the cog silhouette -> cog
  stays bright & sharp in front. FSI writes guest units into EDRAM (correct).
  Canary looked right only because its SPIR-V translator doesn't implement
  oDepth on the fbo path at all (restore becomes a flat clear).
  Verified in replay: halving the restore PS depth makes fbo match FSI
  (skull-region R 0.79->1.25 after particles, ->0.43 after smoke; FSI 1.30/0.40).
  Fix: spirv_translator_rb.cpp, non-FSI epilogue: if writes_depth and not a
  float24-conversion mode, depth *= 0.5 when kSysFlag_DepthFloat24. Plugin
  rebuilt + staged. Likely also affects gameplay depth (drool vs model),
  shadows/lighting that depend on restored depth.
- How it was found: recomp FSI vs fbo captures of the menu; GFXR dump-resources
  options must be nested under "DumpResourcesOptions" in the JSON
  ({"DumpDepth":true,"DumpAllDescriptors":true,"DumpRawImages":true,
  "DumpBeforeCommand":true}); per-draw dumps inside one render pass show the
  pass end state. FSI EDRAM buffer (set0 b1, 10 MB) decodable per tile
  (tools in scratchpad; tile = base + (y/16)*pitch_tiles + x/80, 80x16 px).
- oDepth x0.5 fix CONFIRMED live (user): fbo now matches FSI (cog/menu OK).
- Remaining (both fbo+fsi, ReXGlue-only unless noted): lighting flickers;
  character lighting per-polygon (flat) instead of smooth; character shadows
  show "boxes"; black spikes when zooming (upstream, Canary too).
- Cell captures (gears1_captures/cell_recomp/*194232 = recomp fbo, overhead
  floor view; cell_canary/*195100 = Canary, best pair). JSON in scratchpad
  (cell_recomp_20261009T194232.json frame 1, cell_canary_..195100 frame 4).
- Shadow atlas (D24_UNORM 1x 880x2992, 422/211 tiles, casters 4248/1056/3738
  idx etc.) content IDENTICAL to Canary -> shadow maps fine.
- UE3 modulated shadow projection = 36-index BOX per caster (cube volume),
  blend DST_COLOR*ZERO (multiply), stencil NOT_EQUAL, depth test off, depth
  bias on. Recomp draw blk 78950 (VS 703 / PS 329) vs Canary blk 106455
  (VS 500 / PS 324): identical pipeline state; PS samples scene depth R32
  1280x720 (b0), 64x64 RGBA8 jitter/noise tex (b2), atlas R32 864x864 (b4).
  PS consts equivalent (camera/light differ slightly).
  RESULT: recomp output after/before ratio median 0.005, max 73, after max
  35.8 (multiply src far outside [0,1]) -> visible dark/bright boxes. Canary:
  no visible change at that draw. => the projection PS computes garbage in
  ReXGlue. NEXT: diff translated PS 329 vs 324 (shader-diff agent approach,
  spirv in scratchpad/spv_*), focus on texture fetches (atlas compare, jitter
  tex k_8_8_8_8 sampling, signed/unsigned, exp_adjust word3 fix, offsets,
  GetTextureWeights/LOD), and patch-in-replay test (output intermediates via
  shader edits, see tools/spv_drop_interface.py approach; spirv-as
  --preserve-numeric-ids). Same bug likely explains per-polygon lighting.
- Replay tricks that worked: dump-resources JSON with
  {"DumpResourcesOptions":{"DumpDepth":true,"DumpAllDescriptors":true,
  "DumpRawImages":true,"DumpBeforeCommand":true}}; tools gfxr_draw_diff.py,
  gfxr_draw_context.py, gfxr_draw_modules.py, gfxr_draw_textures.py,
  gfxr_dispatch_sources.py; gfxrecon-extract --dir for SPIR-V; patch shaders
  via spirv-dis -> edit -> spirv-as --preserve-numeric-ids -> --replace-shaders.
- Shadow projection draw (recomp 78950 / Canary 106455) - RULED OUT:
  translated PS+VS (agent: no semantic diffs, only layout/flag remaps), fetch
  constants (identical: fc0 scene depth k_24_8_FLOAT, fc1 atlas k_24_8, fc2
  64x64 k_8_8_8_8 jitter), texture_swizzled_signs (all depth comps unsigned
  in both -> ReXGlue's signed depth view is NOT used here), samplers
  (identical: nearest, clamp/repeat, no aniso, transparent-black border),
  atlas contents (identical). Scene-depth ranges comparable (guest units).
  Canary at that draw: no pixel change at all (stencil NOT_EQUAL may reject,
  or output exactly 1). Recomp: 53% of screen changed, src values up to 73.
  NEXT: replay shader-patch the recomp projection PS to output intermediates
  (reconstructed view depth, light-space coords, atlas sample, final factor)
  and the stencil state (dump stencil aspect before the draw in both:
  DumpDepth gives DEPTH + STENCIL files); check stencil ref/compare/write
  masks dynamic state for this draw in both captures.
- Stencil dynamic state identical too (ref 0, NOT_EQUAL, masks 0xFF): the box
  only shades pixels an earlier marking pass set non-zero. Recomp: ~53% of
  screen marked+shaded; Canary: nothing marked -> Canary most likely does NOT
  render these modulated character shadows at all (its missing oDepth turns
  the depth restore into a flat clear, breaking the depth-tested marking).
  => Canary is NOT a valid reference for character shadows/lighting. The
  recomp renders them, but the projection PS output is out of range (up to
  73). Continue with replay shader-patch of the recomp projection PS (output
  intermediates) and reason about expected values (real-HW semantics), e.g.
  scene depth b0 units, depth linearization consts c1, light matrix c4-c7.
- CORRECTION: the "ratio median 0.005 / max 73 / 53% of screen" numbers above
  were a GFXR artifact. dump-resources re-executes the command buffer prefix
  once per dump point (before + after = 2 runs); this cb reads the EDRAM
  buffer and later writes it, so the 2nd run starts from modified data and
  RT ownership transfers produce 7e3-decoded garbage. Rule: for passes after
  RT transfers, use DumpBeforeCommand=false and ONE draw per replay; get dst
  by a second replay with the PS replaced by an identity (output 1.0) shader.
  Also skip DumpAllDescriptors unless needed (512 MB shared-memory dumps fill
  /tmp).
- CHARACTER SHADOW "BOXES" ROOT CAUSE (ReXGlue bug, fixed): the host
  depth->stencil-bit ownership transfer shader never kills samples. In
  vulkan/render_target_cache.cpp the kill is guarded by `if (packed)`, but for
  TransferOutput::kStencilBit the depth source isn't loaded, so `packed` stays
  NoResult; the 8 bit passes (static ref 0xFF, write mask 1<<bit) all write ->
  restored stencil = 0xFF everywhere. UE3 modulated shadows use z-fail stencil
  volumes (marking pass: GEQUAL, front zfail INC / back zfail DEC, expects
  stencil 0; projection: NOT_EQUAL 0, REPLACE 0). From 0xFF the true inside
  wraps to 0 and the rest of the box stays 0xFF -> inverted shadow, whole box
  footprint shaded, receivers project outside the caster's atlas tile into a
  stale black (depth 0) band -> dark boxes. Canary has the fix
  (`if (packed == NoResult && mode.output == kStencilBit) packed =
  source_stencil[0];`); ported to ReXGlue. Verified in replay by patching the
  captured transfer FS (sh21296): stencil before marking 0xFF -> 0, boxes gone,
  Marcus gets a soft self/floor shadow (39k px shaded instead of ~290k).
  Plugin rebuilt + staged; needs live test (fbo). Possibly also behind the
  flat/per-polygon character lighting and lighting flicker (any stencil-based
  pass after a depth RT transfer). FSI doesn't use host transfers, so if FSI
  still shows boxes it's a separate cause.
- Canary-vs-ReXGlue RT cache review (agent): ReXGlue enables
  gamma_render_target_as_unorm16 on Vulkan host RTs (Canary Vulkan hard-codes
  false). Its 8888 <-> 8888_GAMMA color transfer converted in the wrong
  direction (vs Canary D3D12 3594-3606); fixed (dest == 8888 -> LinearToPWL).
  A/B knobs if lighting/gamma still looks off: --gamma_render_target_as_unorm16=false
  (Canary Vulkan behaviour), --vulkan_dynamic_rendering=false (transfers use
  render passes like Canary). Other diffs judged low impact (device-capability
  fallbacks, FSI 2x MSAA forced to 4x, stricter EDRAM barriers).
- Character "per-polygon / faceted" lighting ROOT CAUSE (both fbo+FSI): 2x
  MSAA sample pattern. UE3 draws the base pass at 2x (tiled), resolves depth
  (guest sample 0) -> restore pass at 1x -> per-light additive passes use
  GREATER_OR_EQUAL against it (no depth bias). Whole triangles fail -> black.
  Measured in replay (cell_recomp 193735, Marcus "Shit." frame, light draw
  94511): light-pass depth - stored depth = 0.505 * dDepth/dy (x term ~0),
  uniform over all tiles. Not shaders (bones/VP constants identical; interps
  smooth), not the restore (pixel-aligned), not sample choice alone:
  stored-point offset = (0,-0.5) px with host sample 1, (+0.5,0) host sample 0,
  (+0.25,-0.25) averaged. The guest itself shifts 2x viewports by (-0.25,+0.25)
  (sys const ndc_offset.x = 0.5/1280 vs 1/1280 at 1x) - on Xenos that puts
  guest sample 0 exactly on the pixel center. Host D3D10/Vulkan 2x pattern is
  the vertical mirror, so it lands half a pixel off.
  Fix (SDK, cvar vulkan_xenos_2x_sample_locations, default on):
  VK_EXT_sample_locations, guest s0 at (0.25,0.75) / s1 at (0.75,0.25) of the
  pixel (host index 1 / 0); 2x pipelines (pipeline_cache + transfer pipelines)
  chain VkPipelineSampleLocationsStateCreateInfoEXT; 2x depth RTs created with
  SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT and layout barriers carry the
  VkSampleLocationsInfoEXT. Extension is enabled in the RUNTIME
  (ui/vulkan/vulkan_device.cpp, as a local flag to keep the Extensions ABI) ->
  needs the rebuilt librexruntimed.so, staged in
  out/build/linux-amd64-debug/runtime_override/ (put first in LD_LIBRARY_PATH);
  the plugin detects enablement via vkGetDeviceProcAddr(vkCmdSetSampleLocationsEXT)
  and logs "Xenos 2x MSAA sample locations enabled/disabled".
  FSI path still uses 2x-as-4x with standard positions -> not fixed there yet.
- After the 2x sample-location fix (user, fbo): MUCH better. Left: rare black
  bits on characters at some camera angles, a flickering black square (top
  left, before Marcus emerges), and a row of dark dots on Dom's cheek
  (blacksquare capture, frames 1-3). The dots are single game pixels darkened to
  ~0.18 (modulated shadow color), evenly spaced on one row (~y 353) - looks like
  shadow-volume stencil misclassification where a box face meets the face
  surface. Not removed by tiny light-VS depth nudges. depth_float24_convert_in_
  pixel_shader=true did NOT fix it (user).
- Debug frame stepping (plugin, cvar gpu_frame_step_hotkeys, default on;
  base graphics/command_processor.cpp, after IssueSwap): F9 pause/resume at the
  next guest swap, F10 advance one frame (also starts a pause), F12 (while paused)
  re-presents the paused frame and STAYS paused, so the GFXR F12 trigger fires
  and every following F10 step is recorded (a one-frame glitch must be stepped
  into while recording - the paused frame's own draws predate the trigger).
  Stepped capture 213109: Dom's arm black wedge visible only in the paused
  frame (screenshots 1-7 = re-presents), gone by frame 8 -> not analyzable. Guest time frozen while paused
  (Clock::set_guest_time_scalar(1/16384), clock flushed around changes). Only
  uses runtime symbols that the installed runtime exports. No rewind (would need
  full save states).
- Capture speed: unassisted tracking + per-block file flush made each captured
  frame take seconds (~126 MB of mapped memory dumped per frame, 3-4 submits),
  and the window's re-presents while paused counted toward GFXR_FRAMES -> the
  "engine hangs for a minute". Now: runtime cvar
  vulkan_flush_coherent_mapped_memory (ui/vulkan/vulkan_util.cpp) flushes
  host-coherent memory too, and tools/gfxr_capture.sh uses assisted tracking
  with it by default (GFXR_CRASH=1 restores unassisted + flush for crash
  captures). Frame step: a step now runs guest time for exactly 33 ms (timer
  thread refreezes), and at most one step is queued (key auto-repeat).
- Frame-step DEADLOCK (fixed): the "33 ms of guest time per step" timer froze
  guest time mid-frame when a captured frame took longer; the game paces frames
  on guest time and the GPU then sat in WAIT_REG_MEM on the guest forever (GPU
  thread in hrtimer_nanosleep, no swaps). Now: frozen only while waiting at the
  swap, cvar gpu_frame_step_time_scale (default 1/16) while a stepped frame is
  produced, normal when resumed. Capture 214943 (1.2 GB, 3065 presents) has no
  new frames - all idle repaints of the paused image (the window keeps
  presenting ~25/s while paused; each counts as a GFXR frame).
- NEW: GFXR trim snapshot = end state of the paused frame. Extract images from
  InitImageCommand meta blocks (block ordinal = JSON index; payload after
  meta_id,tid,device,image,data_size,aspect,layout,level_count,level_sizes;
  LZ4 if type&0x80000000). Partial gfxrecon-convert (kill at ~300 MB) is
  enough for the state section. Scratchpad: snap_depth/stencil/color.npy.
- BLACK WEDGE / "spike" ROOT DIRECTION: in 214943's snapshot the 1x scene depth
  (D32S8 1280x2048, img 18372) has a thin near-depth sliver from the left edge
  to Dom's head (rows ~338-354, x 0-~600), depth rising linearly with x
  0.30->0.45 host (toward 0.5 = near plane), vs 0.016-0.07 around it. Stencil
  all 0. 2x base-pass depth (18251) has no sliver -> drawn by a 1x
  depth-writing pass after the depth restore. It's geometry with a vertex
  thrown to the near plane (exploded vertex) -> light passes behind it fail ->
  black wedge/spikes (same family as the upstream "black spikes").
  1x depth-writing draws per frame (spike capture 171408 c2.json, frame 10
  window 273927+): restore (3 verts, pipe 695), then GEQUAL+write meshes
  1440/2208/2676/2016/2016/1764/2346 (VS 377/486), 5244/7314 (VS 351, pipe 993 =
  skinned, the old "garbage bone constants" draws!), and 30720/3045/7314
  (VS 467). NEXT: dump the 1x depth (img for view 37286) after each of these in
  171408 frame 10 (bar at game px ~817-837,527-540) to find the draw that
  writes the sliver, then inspect its VS inputs/constants for the bad vertex.
- SPIKE HUNT RESULT (2026-10-10): the 214943 sliver is ALSO in the resolved
  depth texture (R32F 1280x720, img 18370 = guest depth, exactly 2x the 1x host
  depth) -> the restore just copies it; the sliver was in the 2x BASE-PASS depth
  at resolve time. It is one long thin triangle from off-screen left to Dom's
  cheek, clipped at the near plane at the cheek (depth -> 0.5 host = guest 1.0)
  -> a triangle with a vertex at/behind the camera (exploded vertex) drawn in
  the base pass; light passes behind it fail -> dark wedge + cheek dots.
  Velocity buffer (img 18396) is 0 under the sliver -> not the velocity pass.
  The snapshot's 2x depth (18251) only holds the last predicated-tiling tile
  (screen rows 512-719 in RT rows 0-207), so "2x clean" earlier was wrong.
  171408 is a dead end: its velocity draws (274075-274110, R16G16_SNORM, GEQUAL
  +write) change no depth, its base pass has no sliver at the bar, and it
  predates the stencil/sample-location fixes (its bar was likely the old
  stencil bug). DumpDepth DOES work with DumpRawImages (2x dumps give 1 sample,
  layout = EDRAM tile rows, 2x host depth = 1x/2 in that build).
  Replays of 171408 take ~1 s each (scratchpad sk/one.sh).
  NEXT: fresh current-build capture of a frame with a spike (frame-step:
  F9 pause on the glitch, F12, F10 x2, F12), then bisect the base pass on the
  2x depth (tile rows) to find the draw, then its VS/vertices/constants.
  If the spike vanishes once stepped (time ~frozen), suspect a CPU/GPU race
  on per-frame data (FSI = slower GPU made spikes worse).
- STEPPED CAPTURES (2026-10-10, gears1_captures/spike2/, 6 files): frame
  stepping works. Each F10 step = one real guest frame, but its GPU work is a
  deferred command buffer played into Vulkan + submitted later, buried among
  thousands of idle-repaint presents (~8000 presents/s while paused). Find
  steps by scanning vkBeginCommandBuffer..vkQueueSubmit spans > 3000 blocks
  (scratchpad s2/steps.py); screenshot frame (submit frame + 3). 196 steps
  total: 081658 = gun spike (24 steps), 081600 = "Welcome back" (Dom's arm).
  The gun spike persists across 10+ steps (pose-deterministic, not a race).
- PITFALL (big): dump-resources results on these captures are NOT
  reproducible - the same dump spec replayed 4x gave 31130/25259/22626/6271
  spike px in the depth pre-pass (the dump's cloned command buffer races the
  original). Full replays + screenshots ARE bit-identical. Everything derived
  from depth dumps today (pre-pass spikes from skinned VS 662, neuter bisect,
  vertex-range bisect) was dump noise: culling VS 662 does not change the
  visible spike. Use screenshot-based bisection (tools/gfxr_artifact_bisect.py)
  on a truncated capture (cut a few presents after the target frame).
- Ruled out along the way: sync validation (VK_LAYER_ENABLES=
  VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT) reports no
  hazards; shared memory + staging are in the trim snapshot; staging/uniform
  writes ARE recorded (batched FillMemory per page before the deferred cb is
  played); no shader writes shared memory; VS 662 skinning data (6218 verts,
  bones 0-74 -> c5..c229, weights sum 1, sane bone matrices) projects fine on
  the CPU. VS index: host draws with the raw BE u16 index buffer and the VS
  byte-swaps gl_VertexIndex (system const vertex_index_endian).
- SPIKES = UE3 MSAA PREDICATED TILING (2026-10-10). Stepped frame of 081658
  (screenshot frame 15355, truncated copy) shows the real spikes: dark wedges
  from the gun converging exactly on the screen centre, clipped to a tile
  rect (x<640, y<512). The stepped frame's render is NOT deterministic in
  replay either (same file -> discrete different spike sets; 41682 vs ~105k
  black px), independent of VS 662 compile flags (spirv-opt -O, no
  SignedZeroInfNanPreserve: still spikes). Canary game-patches has
  "Black Shading Fix" for 4D5307D5 (boma): GUseTilingCode=FALSE
  (be8 0x82BB6BBB 01->00) + GTilingMode=4 (be8 0x82BB6B53 01->04), "dynamic
  lighting artifacts caused by light environments and MSAA tiling". Our XEX
  has the expected original bytes (Ghidra). Applied in src/main.cpp
  (Gears1RecompApp::OnPostLoadXexImage, verifies bytes first), cvar
  gears1_black_shading_fix (default true; =false to compare). Pending live
  test of spikes, Dom's arm polygons, black square, character flicker.
- Tiling patch live test: CRASH at the opening scene, kernel Xid 31
  GPCCLIENT_GCC (constant cache) VIRT_READ @0x3_f8000000 - same fault class
  as the original "RTE" crash. GCC = uniform-buffer reads. Root-cause theory:
  relative constant addressing (skinning VS: a0 = clamp(floor(3*idx+.5),
  -256,255); rows c[a0+5..7]) is translated to an UNCHECKED dynamic UBO index
  (spirv_translator.cpp LoadOperandStorage). Out of range = undefined in
  Vulkan (garbage -> exploded vertices, varies run to run; or unmapped ->
  Xid 31), while D3D/Xenia DXBC read 0. The old RTE "fix" and the 4th-bone
  predicate finding are consistent with this (they perturbed the codegen /
  removed the OOB read). FIX (SDK, plugin): cvar spirv_safe_relative_addressing
  (default true) - relative float-constant and register reads load index 0
  when out of range and return vec4(0). Relative register WRITES are not
  bounds-checked yet. Plugin rebuilt + staged; pending live test with and
  without gears1_black_shading_fix.
- Live test of both fixes: safe relative addressing FIXES THE CRASH. Tiling
  patch (gears1_black_shading_fix) fixes Dom's random black polygons and the
  flickering black box (box returns with the patch off). Thin black spikes
  (e.g. across Dom's face) remain with either -> separate bug, next target.
- Captures cleaned 2026-10-10: kept spike2/, cell_canary/, single_draw.gfxr;
  deleted all older captures referenced above (RTE investigation, menu*,
  redtint, cell_recomp, blacksquare, flicker, spikes, stepped).
- REMAINING SPIKES = NVIDIA DRIVER BUG (615.78.08), 2026-10-10. Capture
  cutscene/102245 (aiming), frame 119553 (truncated copy tc245.gfxr):
  * Capture slowness: presenter used immediate/mailbox and repainted ~1.4k/s;
    GFXR serializes calls -> game ~1 fps while recording (only 140 game
    frames in the 2-min cutscene capture). tools/gfxr_capture.sh now forces
    FIFO (--vulkan_allow_present_mode_{immediate,mailbox,fifo_relaxed}=false).
  * Spikes come from ONE draw: 3517354 = Marcus's depth pre-pass, VS 664
    (position-only skinning, pipe 2245, VS-only, GEQUAL+write). Neutering it
    alone removes them (deterministic clean result 5479 px); its base-pass
    counterpart (VS 754/755) is fine.
  * Inputs verified deterministic/sane: vertices (6218, bones in range,
    weights sum 1), indices (30720, max 6217), UBO bindings match, all
    barriers whole-buffer and correct, staging filled once before submit.
  * Replays of the identical file give DIFFERENT spikes every run (a few
    recurring discrete outcomes). Still random with gfxrecon-replay --sync,
    with every barrier in the cb widened to ALL_COMMANDS, sparse binding off
    live, NMin->FMin, no SignedZeroInfNanPreserve, Volatile loads, constants
    copied to a local array, OOB->NaN.
  * VS 664 patched to ALSO store its gl_Position (8 SSBO stores, bound sparse
    chunk 121) -> recorded outputs sane + identical across runs AND the
    spikes vanish (3/3). Same shader with one never-executed store -> GPU
    hang + Xid 31 GPCCLIENT_GCC @0x3_f8000000 (identical to the original
    crash). => logically identical SPIR-V gives random garbage / correct /
    fault depending on incidental codegen = NVIDIA shader compiler (or HW
    scheduling) bug, same family as the RoundingModeRTE fault.
  NEXT: test another driver branch (Gentoo has 595.104.02, 580.178.04):
  replay tc245 3x (spike px in [700:2160,0:1950] of frame 119553; clean =
  5479) + live run. If clean -> report to NVIDIA with tc245 + VS 664.
- Driver 595.104.02 (user switched from 615.78.08): spike repro still random
  (327453 / 1158104 / repeat) -> not a 615 regression. Replays are much slower
  on 595 (~9 min/run vs ~1 min). Repro + checker saved outside /tmp:
  gears1_captures/cutscene/spike_frame119553.gfxr, tools/spike_replay_check.sh.
  VS 664 with gl_Position kept in a private var (no output read-back) still
  spikes. Lavapipe (media-libs/mesa video_cards_lavapipe) installed for a
  reference replay: VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
  gfxrecon-replay -m rebind ... (supports sparse, dynamic rendering, sample
  locations, VS stores, robustness2, FSI).
- LAVAPIPE VERDICT (2026-10-10): the identical spike repro replayed on
  lavapipe (llvmpipe, LLVM 22.1.8, gfxrecon-replay -m rebind, 9.5 min)
  renders the full scene correctly with NO spikes (5119 black px in the
  spike region vs NVIDIA clean baseline 5479; NVIDIA 595 + 615 never clean
  unmodified). Same SPIR-V, same commands -> the spikes are an NVIDIA
  driver/compiler bug in VS 664 (Marcus's depth pre-pass skinning shader),
  not ReXGlue/Xenia logic. PARKED. Options: report to NVIDIA with
  spike_frame119553.gfxr (contains game data - user's call) + sh664 SPIR-V;
  re-test on future drivers with tools/spike_replay_check.sh; a native
  renderer with different shader codegen may avoid it.
- NATIVE RENDERER PHASE 0 (2026-10-10): Xbox D3D library located via kernel
  imports in the generated code: sub_8223E860 = swap/present (VdSwap,
  VdGetSystemCommandBuffer; the Canary 60 FPS patch 0x8223E7BB sits just
  before it), sub_82221EC0 = ring buffer init (VdInitializeRingBuffer, ME_INIT),
  sub_8223A840 = VdInitializeEngines; UE3 callers at ~0x824A4D48/0x824A5170.
  Library code ~0x821F8000-0x82280000 (other XDK libs interleaved). PM4 header
  scan (lis/ori pairs -> type-3 opcodes): DRAW_INDX_2 sub_8222B678,
  SET_CONSTANT/LOAD_ALU_CONSTANT sub_8223DC78, predicated-tiling bins in
  sub_82228458/82228618/8222FEF0..., EVENT_WRITE/WAIT_REG_MEM sites listed in
  scratch pm4.json. Ghidra project has no strings defined and inline scripts
  are disabled, so the generated C++ is the analysis source.
  Observer: tools/gen_d3d_observe.py -> src/d3d_observe.cpp (474 functions
  in that range called from outside it; strong overrides of the weak
  sub_XXXXXXXX aliases, counting then calling __imp__sub_XXXXXXXX). Off by
  default; --gears1_d3d_observe=true writes one line of call counts per
  frame to --gears1_d3d_observe_file.
- Observer run (log gears1_captures/d3d_observe.log, 1057 frames menu ->
  loading -> opening scene start) ended in the NVIDIA Xid 31 crash again (615,
  random fault mode of the same bug - observer is CPU-only). Confirmed names:
  sub_82220858 = D3DDevice_SetTexture(device, sampler, texture, dirtyMask)
  (6-dword fetch constant -> device+0x400+sampler*0x18, texture ptrs at
  device+(0xBFC+sampler)*4); sub_8222DE50 = D3DDevice_DrawIndexedVertices
  (device, primType, baseIndex, startIndex, indexCount): flushes dirty
  register blocks (0x2000..0x2380, 0x4900, ALU consts 0x4000/0x4400) via
  sub_82233560, then DRAW_INDX 0xC0032201; splits >0xFFFF indices; has a
  predicated-tiling variant. Device cmd-buffer write ptr device+0x28, limit
  +0x30, refill sub_82221980. PM4 scan bug: type-3 bit0 is the predicate
  flag, so headers can end in 01. Some hot addresses (82222350, 82228998...)
  are not functions in the Ghidra project (ReXGlue found more functions).
- D3D API map (draw, index/stream binding, shaders, constants, textures,
  samplers, blend/depth-bias states, viewport/scissor, render targets, swap,
  queries, resource refcounting) now in NATIVE_RENDERER.md "D3D API map".
  Key: UE3 inlines SetVertexShaderConstantF -> snapshot the device shadow at
  draw time; occlusion queries + UE3 ring allocator busy-wait on GPU fences.
  Static search (generated-code scan for device-offset and register-immediate
  accesses) then filled in: SetDepthStencilSurface 8222B398, Clear 8222CC48,
  Resolve 82235528, DrawVertices 8222CFF8 / DrawVerticesUP 8222D4B0, shared
  pre-draw commit 822346A8, vertex decl create/set 82222E18 / 82222DF8,
  Create{Vertex,Pixel}Shader 82222AC8 / 82222710, Z/stencil/cull setters
  and the register-shadow layout. UE3 inlines the pixel-shader init
  (825A44C8) and SetVertexDeclaration, so capture shaders at bind/draw time.
- Shader dump (src/shader_dump.cpp, cvar gears1_shader_dump, dir
  gears1_shader_dump_dir): on SetVertexShader/SetPixelShader, writes each
  distinct shader as the original container (header + microcode) to
  {vs,ps}_<xxh3>.bin plus index.txt. Hooked via PRE_HOOKS in
  tools/gen_d3d_observe.py (one strong override per symbol). Dumps go to
  gears1_captures/shaders/ (game data: never in git).
- First shader dump (2026-10-10, gameplay until the usual Xid 31 crash):
  55 VS + 153 PS in gears1_captures/shaders/. Containers start 0x102A11xx,
  header holds a D3DX constant table with names (20-byte entries: name,
  regset, register, count) and compiler 2.0.3529.0. VS 664 (spike shader,
  re-extracted as sh664 from spike_frame119553.gfxr) = vs_570404661ac6b2de:
  BoneMatrices c5..c229 (225 = 75 bones x 3 rows; matches the relative
  read c[a0+5..7]), LocalToWorld c0-3, ViewProjectionMatrix c230-233 (sh664
  uses int 230-233), MaxBoneInfluences float c4; no other dumped VS has this
  constant set. Shadow-depth skinning = vs_614245514f16e919, velocity
  skinning = vs_9c11fe82cabc8370.
- TRANSLATOR PROTOTYPES on VS 664 (2026-10-10), spike repro, NVIDIA 615.78.08:
  * Container: +0x18 = offset of the shader record {microcode offset within
    the physical part, microcode size}; the physical part starts with the
    def-c# literals (0x40 bytes here), so microcode != physical part.
  * B = ReXGlue translator offline: rexgpu-shader-tool (SDK option
    REXGLUE_BUILD_SHADER_TOOL, src/graphics/pipeline/shader/shader_tool_main.cpp)
    reproduces sh664 BYTE-IDENTICALLY from vs_570404661ac6b2de.bin (ucode hash
    70ce10db85614760) + writes the ucode disassembly. Same translation +
    spirv-opt -O: still random spikes (298902/1766229/1748183) -> the bug
    follows the translator's structure, not instruction order.
  * A = XenosRecomp (tools_ext/XenosRecomp, built against Microsoft's DXC
    1.9.2609 Linux release in tools_ext/dxc) -> HLSL; tools/xr_xenia_adapter.py
    wraps it in Xenia's VS interface (set 1: system 0/float 1/fetch 4, set 0:
    shared memory; index swap/base/clamp; vfetch; W/XY/Z flags + NDC epilogue)
    -> DXC -spirv -> gfxrecon-replay --replace-shaders (sh664).
    Result: DETERMINISTIC (3 byte-identical runs) and CLEAN (5479 x3) once
    dp4 uses Xenia's sequential NoContraction sum (with HLSL dot(): fixed
    95539 px of whole black triangles on Marcus's head = pre-pass vs base-pass
    depth mismatch under the EQUAL test, same look as the random black
    polygons). Draw verified live (depth-0 variant blacks out Marcus + Dom);
    vs dropped draw only 2088 px differ by <=3.
  * PITFALLS: (1) disassembly prints vf0 but the translator's fetch constant
    is 95 (D3D streams count down): sh664 reads fetch_constants[47].zw.
    (2) A broken replacement that rasterises nothing ALSO scores 5479, as
    does a full-screen triangle at depth 1.0 - validate with a depth-0
    variant (near = 0 here; any z < 1 blacks the frame).
  * XenosRecomp gaps seen: relative-constant bound check ignores negative
    indices; vertex input/interpolator model is Unleashed's (needs a native
    ABI or this adapter); everything else on this shader translated right.
  NEXT: find WHICH Xenia translator construct triggers the NVIDIA bug
  (pc switch-loop control flow, xe_var_registers function array,
  ZeroIfAnyOperandIsZero selects, SignedZeroInfNanPreserve) by moving the
  DXC shader toward Xenia's structure (or vice versa) - a translator-side
  fix would cure the emulated path too.
- NVIDIA BUG BISECT (2026-10-10, VS 664 repro, 3 replays each; "random" =
  different spike counts per run, "clean" = 5479 x3 byte-identical):
  Xenia translation, each edit alone -> ALL STILL RANDOM: spirv-opt -O (removes
  the register array + all function vars), no gl_Position Invariant, no SM3
  zero-multiply selects (new cvar spirv_sm3_zero_multiply), NDC consts loaded
  as scalars (scalar-layout vec3 straddling 16 B), NMin/NMax/NClamp ->
  FMin/FMax/FClamp everywhere, no Restrict on shared memory, no
  DontFlatten/DontUnroll hints, float-constant index via SClamp instead of
  select (plus earlier: no SignedZeroInfNanPreserve, NMin->FMin in guards,
  Volatile loads, constants copied to a local array, OOB->NaN, gl_Position
  in a private var).
  XenosRecomp+DXC, each edit alone -> ALL STILL CLEAN: Xenia-style pc
  loop+switch around the body (T1), + vertex fetches inside case 0 (T3),
  Xenia's exact interface (scalar layout, offsets, vec3 NDC loads,
  line-loop index rule) (T2).
  => Not the interface/layout, not any single Xenia construct tried; the
  trigger is in how Xenia's instruction stream is shaped as a whole (NVIDIA
  backend codegen). Tools: rexgpu-shader-tool --<cvar>=<v>; variants in
  gears1_captures/replace/<name>/sh664.
  Possible next steps: delta-debug with a DETERMINISM oracle (3 runs
  byte-identical vs not) by NOP-ing microcode instructions (zero write
  masks) and re-translating; or a SPIR-V->HLSL->DXC round trip of Xenia's
  own shader (needs spirv-cross) to separate "program" from "IR shape".
- NVIDIA SPIKE TRIGGER FOUND (2026-10-10): switch-case FALLTHROUGH inside the
  translator's main loop. ProcessLabel ended each case with an OpBranch into
  the next case label; jumps already go `pc = target` -> continue block ->
  loop header -> switch. NVIDIA (595, 615) miscompiles the fallthrough form.
  * Minimal repro via tools/ucode_ddmin.py (microcode delta-debugging,
    determinism oracle, 15 geometry halves pinned): besides the skinning
    core only 31v/32s (p0 = c4.x > 1), 53s (p0 = weight.w > 0) and 60v
    (predicated mad into r4) are needed: three (!p0) jmp L21 + a predicated
    accumulate right before label L21. gears1_captures/ddmin664/minimal.bin.
  * Same minimal program: Xenia SPIR-V random; SPIRV-Cross GLSL -> glslang
    deterministic; XenosRecomp+DXC deterministic (same image). Full VS 664
    Xenia SPIR-V -> HLSL (SPIRV-Cross, sys consts rewritten to uvec4[32]) ->
    DXC: clean. Full VS 664 with scalar layout removed: still random.
  * Hand edit of ONLY that edge (fallthrough -> pc=21 via continue) in the
    full, otherwise unmodified VS 664: clean x3, byte-identical to the
    XenosRecomp result.
  * FIX (SDK branch, spirv_translator.cpp ProcessLabel): cvar
    spirv_no_switch_fallthrough (default true) enters each label through the
    loop instead of falling through. rexgpu-shader-tool output of VS 664:
    clean x3, byte-identical. Plugin rebuilt + staged (stage_gpu_plugin_override).
    Shader storage keeps ucode only, so no cache flush is needed.
  * Also added (diagnostic, default unchanged): spirv_sm3_zero_multiply.
  * LIVE TEST (2026-10-10, 615.78.08): opening cutscene and aiming down
    sights clean - no spikes. NVIDIA spike bug SOLVED by the translator fix.
  * RTE retest (2026-10-10): opening scene with
    --vulkan_shader_rounding_mode_rte=true runs fine, no Xid -> the RTE
    "fault" was the same fallthrough bug. Workaround removed from
    config/gears1_recomp.toml (cvar back at its default, true).
    spirv_no_switch_fallthrough is now the only NVIDIA-specific fix needed.
  * SDK work committed 2026-10-10 on branch fix/gears1-xenos-rendering
    (10 topic commits on top of v0.10.0; fix/nvidia-rte-workaround had no
    commits of its own and is superseded). Remotes: origin = fork
    JTM-rootstorm/rexglue-sdk, upstream = rexglue/rexglue-sdk. Per the
    upstream Contributing wiki: base on and PR against main, rebase (never
    merge), every PR needs a linked issue; clang-format clean on changed
    lines. upstream/development (24 commits ahead, no graphics changes, no
    overlap with our fixes) is ignored.
  * Commits on the fork are GPG-signed + Verified (re-signed after a
    filter-branch trailer strip had dropped the signatures).
- Xenia Canary fork set up 2026-10-10: ~/Documents/Programming/xenia-canary
  (origin JTM-rootstorm/xenia-canary, upstream xenia-canary/xenia-canary,
  branch canary_experimental). Same fallthrough code at
  src/xenia/gpu/spirv_shader_translator.cc:936 -> port candidate. Upstream
  already has "[GPU] Rasterize at Xenos MSAA sample positions". Builds on
  Gentoo with --cmake-define CMAKE_CXX_FLAGS=-Wno-error=unused-result
  (unchecked fread in png_utils.cc vs glibc warn_unused_result).
  Run: GDK_BACKEND=x11 (Wayland unsupported -> gray window) --gpu=vulkan
  --render_target_path_vulkan=fbo.
- Fresh Canary 04f85416e (2026-10-10, 615.78.08): no crash, NO transparency
  and NO see-through-walls (fixed upstream since 02d2cb5), but spikes (more
  of them) and black polygons/shadows (no game patch installed -> predicated
  tiling on). Fallthrough fix ported (unconditional, no cvar) on local branch
  spirv-label-no-fallthrough, commit 12f79522a "[GPU] Enter SPIR-V control
  flow labels through the main loop" (signed, lint clean): user confirmed
  spikes gone in Canary too. Not pushed. Remaining upstream emulator bug
  worth chasing in Canary: predicated tiling -> black polygons/shadows.
- PLAY SESSION 2026-10-10 (fallthrough fix + RTE on + shader dump): blood
  decal on the floor drawn with a straight seam; the part above it is shifted
  sideways with a strip missing. Seam at the same place on the decal in two
  frames -> deterministic texture layout bug (pitch / mip / tiling) for that
  texture, not an upload race. CORRECTION (user, same day): it's the blood
  pool of a downed teammate, position varies, and it FLICKERS in the recomp;
  fresh Canary doesn't flicker (only a straight lit/dark boundary across the
  decal, likely light/receiver edge). UE3 draws a decal once per receiver
  mesh section with its own projection constants -> more likely one
  section's decal draw gets wrong/stale constants (GPU state handling in the
  older ReXGlue code, or a recompiled-CPU miscompile in the decal projection
  math). Texture-layout candidates below are now secondary. NEXT: GFXR
  capture of the glitch (tools/gfxr_capture.sh .../gears1_captures/blood,
  F9/F12/F10 x2-3/F12), compare the decal draws per receiver. ReXGlue's Xenos code predates almost all of
  Canary's Aug-Oct 2026 texture fixes (tools: scratchpad has_commit.py, line
  presence check). Candidates: 4a863a0e1 base map with a separate mip page,
  ace153cb8 level 0 packed mip tails at the base address, bc6b7a020
  array/volume mip layout, 19da17403 linear 96bpp mip row pitch. NEXT: same
  spot in fresh Canary (12f79522a build): intact -> port the matching fix;
  broken too -> GFXR capture + inspect the decal's fetch constant.
- UE3 TILING GLOBALS (static, 2026-10-10): GTilingMode = BE word 0x82BB6B50
  (patched byte 0x82BB6B53), GUseTilingCode = word 0x82BB6BB8 (byte
  0x82BB6BBB). GTilingMode indexes presets: tile counts at 0x82BB6B90 =
  {3,2,2,3,1}; tile rects at 0x82083728 (1280x720 horizontal bands): mode 0
  rows 0-256/256-512/512-720, mode 1 (game default) 0-512/512-720, mode 2
  0-384/384-720, mode 4 = one full-screen tile; per-mode half-pixel offsets
  -0.125/-0.25 for 0-3, 0 for mode 4 (=> mode 4 looks like the non-MSAA
  preset). Users: 8254F680 (BeginTiling caller: depth/RT surface per mode),
  8254F850 (per-tile Resolve loop, table[mode] iterations even with tiling
  code off). GUseTilingCode is written at runtime by 8249C3E8 (toggle) and
  824A4D48 (from render config flags & 0x140).
  Black polygon fix (Canary patch) therefore = switch the game to the
  1-tile/no-MSAA preset. With the fix OFF, an enemy's blood pool renders
  correctly (user, camera moved around); with it ON a teammate's pool showed
  the seam/flicker. New cvars (src/main.cpp): gears1_use_tiling_code,
  gears1_tiling_mode (-1 = default) to test combinations.
- BLOOD POOL FIX (2026-10-10): with Canary's patch (GUseTilingCode = 0,
  GTilingMode = 4) enemy blood pools also glitch (close up, moving camera);
  with no patch they're fine but black polygons return. GUseTilingCode = 1
  + GTilingMode = 4 (tiling code on, single full-screen tile preset): black
  polygons gone AND blood pool clean while growing (user, patched plugin).
  => New default: gears1_black_shading_fix only sets GTilingMode = 4.
  UPDATE (release build, same defaults, log confirms 1/4): enemy pools DO
  glitch, growing and fully grown -> the clean result was a lucky surface.
  User CONFIRMED it depends on the surface/texture the pool lands on. Tiling settings
  are NOT the cause; UE3 draws the decal per receiver (BSP / static mesh /
  terrain: different vertex factories) -> suspect one receiver path.
  NEXT: GFXR capture of a static glitched pool (BUILD=release
  tools/gfxr_capture.sh .../gears1_captures/blood), diff the receiver draws.
  CAPTURES (2026-10-10 18:07-18:08, gears1_captures/blood/, release build,
  8 live trim captures; replay screenshot frame numbers are RELATIVE 1..N,
  ~60 s state load each): 180827/180833 = walkway with metal grooves, pool
  cut along a straight line; past it, on the strip next to the wall, a
  solid red block with straight edges (decal texture edge smeared = wrong
  decal UVs on that receiver strip). 180809/180812 = lighter stone floor,
  pool intact. (Loading a checkpoint keeps old blood pools.)
  Bisect of 180833 frame 88 (block box 2160,920,2240,1120; floor
  2100,1300,2250,1420; grey 0-30): bisect833.log. Result was MISLEADING:
  many halves black the frame ("blind"); only lead was draw 196746 = the
  final full-screen copy of scene colour (R16G16B16A16F 1280x720, view
  3973/image 3972) to the frame: VS 1121 ~ vs_7b7b8a528573a12d (UE3 simple
  element, const Transform), PS 524 ~ ps_0691ed52ea0dfad1 (Texture, Gamma,
  SceneColorBiasFactor); one per game frame (182495, 189190, 196746).
  Neutering it shows the underlying image: the pool shape incl. the straight
  cut is already there -> the bad piece comes from the decal draws
  themselves. Tools: 833.json (gfxrecon-convert, 142 MB, ~1 s), shader
  match = translate all dumps with rexgpu-shader-tool, fuzzy-compare SPIR-V
  opcode sequences (emulator VS differ from the tool by the interpolator
  modification, so no byte match). NEXT: find the draws that sample the
  blood decal texture in the final game frame (182495..196746), compare
  their vertex layout / constants per receiver strip; neuter-diff each
  (one replay, ImageChops diff vs shots/180833/screenshot_frame_88.png).
  Also: one dumped shader asserts in rexgpu-shader-tool (investigate).
  Region-diff bisect (scratchpad region_bisect.py; region833.log): removing
  ALL 1089 draws of the final game frame changes the block only 14% and the
  frame 45% - INCONCLUSIVE: camera static, so leftover EDRAM content looks
  like the current frame. Neuter tests need a capture with camera motion,
  or a marker approach (replace candidate PS/VS with a solid-colour shader).
  RULED OUT: occlusion culling - ReXGlue fakes VIZ_QUERY as visible and
  draws conditional draws unconditionally (game logs no viz queries), and
  ZPD queries return query_occlusion_fake_sample_count = 1000 (always
  visible). Canary (correct pool) has real VIZ/ZPD handling + Aug-Oct
  texture/vertex-fetch fixes ReXGlue lacks; its pool shows only a lit/dark
  boundary. NEXT IDEAS: (1) capture while slowly turning; (2) find the
  draws sampling the blood texture (DXT decal) and compare per receiver;
  (3) A/B with query_occlusion_fake_sample_count / vertex fetch ports.
- CANARY PORT SURVEY (2026-10-10): ReXGlue's Xenos code ~ Canary as of
  Feb 2026 plus cherry-picks; 100+ later Canary GPU commits missing
  (scratchpad presence.txt). Most relevant to the pool: cde5d85ec "[GPU]
  Host RT polygon offset for Z-fighting decals" (shader-depth bias for
  coplanar decal redraws, cvar depth_bias_shader_offset, default FALSE in
  Canary; comment names UE3 static decals). ReXGlue's fixed-function depth
  bias code == Canary's with that cvar off. Z-fight test first: new
  uncommitted diagnostic cvar vulkan_depth_bias_scale (multiplies host
  depth bias; SDK Release plugin rebuilt + staged). If 4x/16x fixes the
  pool -> port cde5d85ec (Vulkan/SPIR-V parts + draw_util). Other port
  candidates: 9e9d3cdd3 OOB vertex fetch words = 0, 0c843efb3 round
  normalized unsigned fixed fetches, 92ada8ebc scalar ALU swizzles with
  three-source vector ops, 4aeb518c9 clamp maxas, 25597a546 depth clamp,
  e20f26963 GPU register reset defaults, 9781a75a2 stale texture state,
  the Sep 2026 texture layout fixes.
  Canary's game-patches entry could be improved the same way (only the
  GTilingMode byte) - separate repo (xenia-canary/game-patches), not done.
- RELEASE BUILD (2026-10-10): SDK `cmake --build out/build/linux-amd64
  --config Release --target rexruntime rexgpu-xenos` (patched, from branch
  fix/gears1-xenos-rendering); game `cmake --preset linux-amd64-release
  -DCMAKE_CXX_COMPILER=/usr/lib/llvm/22/bin/clang++ -DCMAKE_C_COMPILER=clang
  -DCMAKE_PREFIX_PATH=$HOME/Documents/Programming/ReXGlue/linux-amd64
  -DGEARS1_GPU_PLUGIN_OVERRIDE=<sdk>/out/linux-amd64/Release/librexgpu-xenos.so`
  (the preset's clang++-20 doesn't exist here) + runtime_override/
  librexruntime.so copied from the SDK Release output. Run with
  LD_LIBRARY_PATH=out/build/linux-amd64-release/runtime_override:<sdk lib>.
- PITFALL (2026-10-10 17:43): building only the gears1_recomp target re-staged
  the STOCK v0.10.0 plugin (rexglue_setup_target POST_BUILD) without running
  stage_gpu_plugin_override -> a play run without any GPU fixes (spikes
  back). Check: the log line "Xenos 2x MSAA sample locations" only exists in
  the patched plugin. Fixed in CMakeLists.txt: the override is also copied
  in a POST_BUILD of gears1_recomp (after the stock copy).
- MOVED 2026-10-10: everything Gears now lives in ~/Documents/Programming/
  Recomp/Gears/ (Gears1, gears1_captures, tools_ext, xenia-canary); Recomp/
  is shared with other projects. Hard paths in tools/ + docs updated; both
  CMake builds reconfigured in place (same compiler/plugin overrides,
  runtime_override kept); codegen.d paths rewritten. Older FINDINGS entries
  may still mention Recomp/<name> paths in prose = Recomp/Gears/<name>.
  tools_ext/XenosRecomp/build has the old absolute path in its CMake cache
  (binary works; reconfigure before rebuilding it).
- Replay technique for exact depth: PS writes FragCoord.z*2^22 split into two
  11-bit integers (R,G) into an additive pass over a black dst; VS z nudge
  (clip z += eps*w) to make all fragments pass, subtract -0.5*eps afterwards.
- Projection PS math (decompiled via spirv-opt ssa-rewrite + a symbolic
  printer, scratchpad sp_dbg/sym.py): W = 1/(z*c1.z - c1.w),
  L = (sx*W, sy*W, W, 1) * [c9;c10;c11;c12], uv = L.xy/L.w + jitter*c19,
  ref = min(L.z, 0.999), lit if atlas > ref (normal Z, atlas clear = 1.0);
  4 taps, then 12 more if 0.0001 < avg < 0.9999; out = lerp(lerp(1,
  shadowColor c2, spot atten), 1, avg^3). Constants and math were correct.
- Recomp runs that are killed (timeouts, Alt-F4, crashes) leave
  /dev/shm/xenia_memory_* guest-memory files (4.8 GB sparse, ~0.7 GB RSS
  each); clean with `rm /dev/shm/xenia_memory_*` when no instance runs.
- Spike appearance: flat uniform-dark wedge (grey 12 vs surroundings 17-28),
  not lit geometry. No stencil-test or multiply-blend pipelines in frame 3
  range. Hypothesis still open (shadow projection volume vs geometry).
- Added diagnostic cvar `gpu_log_bad_constants` (SDK branch, command_processor.cpp):
  after SET_CONSTANT / SET_CONSTANT2 / SET_SHADER_CONSTANTS / LOAD_ALU_CONSTANT
  ALU writes, warns (max 300) when >=4 float-constant dwords are NaN/Inf/>1e6,
  with source packet, guest address, register range and sample values.
- Project CMake: plugin override is now a separate ALL target
  (stage_gpu_plugin_override, copy_if_different) so plugin rebuilds are
  re-staged even without an exe relink.
- Boots to the main menu with the xenos GPU plugin (Vulkan, RTX 4080).
- Codegen: 0 warnings, 32237 functions sealed (`rexglue codegen gears1_recomp_manifest.toml`, ~12s).
- Build: `cmake --build --preset linux-amd64-debug` → out/build/linux-amd64-debug/gears1_recomp.
- Untested past the menu: no controller on hand. Keyboard/mouse emulation exists
  (`--mnk_mode=true`) but the default bindings are awkward.

## Running
```
LD_LIBRARY_PATH=~/Documents/Programming/ReXGlue/linux-amd64/lib \
  out/build/linux-amd64-debug/gears1_recomp \
  --game_data_root ~/Downloads/Gears\ of\ War/GoW --gpu_plugin xenos
```
- Without `--gpu_plugin xenos` the game silently stalls after VdInitializeRingBuffer
  (no GPU → it waits forever). The plugin must be staged next to the exe:
  `rexglue_setup_target(gears1_recomp GPU_PLUGINS xenos)` in CMakeLists.txt.
- Unknown `--flags` are ignored silently; a typo won't error.
- Under gdb, also `handle SIG34..SIG64 nostop` — the runtime uses real-time
  signals and gdb -batch otherwise stops and kills the process.

## Toolchain
- CMake preset names clang-20, which isn't installed. The existing build cache
  overrides to system clang 22.1.8 (+ CMAKE_PREFIX_PATH=ReXGlue/linux-amd64). Works.

## Ghidra
- Project gears_recomp (in repo dir, gitignored), Ghidra 12.1.3 with XEXLoaderWV.
  `/default.xex` imported; 25239 functions vs ReXGlue's 32237, so Ghidra lacks
  many small leaf functions.
- GhidraMCP inline scripts are disabled (GHIDRA_MCP_ALLOW_SCRIPTS unset).
  For bulk byte access, dump the guest image from a running build instead:
  gdb `dump binary memory image.bin 0x182000000 0x182CE0000` (keep it out of the repo).

## Binary layout notes
- No RTTI (VTableScanner finds 0 COLs).
- .pdata at 0x82137200 (size 0x314B8): 8-byte entries (begin, packed);
  function length in instructions = (packed >> 8) & 0x3FFFFF.
- MSVC switch tables are inline in .text right after `bctr`, base loaded with
  lis/addi. Don't mistake the base for a function pointer.
- MSVC tail-merges shared exits (`li r3,0; blr` etc.) into the next function.

## Manifest fixes (see gears1_recomp_manifest.toml)
- 3 orphan tail-call targets (reached only via `b` thunks in gaps GapFill
  didn't claim): explicit `end` bounds.
- 0x8239BE88: standalone leaf (unlinks r3 from a singly-linked list, next
  ptr +0x1AC, head +0x324). end=0x8239BEE8 (last blr 0x8239BEE0 + zero pad).
- 127 pointer-only functions: vtable slots, `addi r3,r3,-N; b` adjustor thunks,
  callbacks built with lis/addi. No .pdata, no direct `bl`, so codegen never
  created them → "Call to invalid or unregistered function" at runtime.
  First hits: 0x824FA420 (callback passed by sub_82278E28), 0x825F8A80.
  0x822A84D0 alone has 1068 data references.
- Shared tails split out so pointer-only functions can tail into them:
  0x82389334, 0x824D3EB8.
- Found with tools/find_missing_funcs.py: scans .rdata/.data words and code
  lis/addi pairs for .text targets that aren't registered, aren't inside a
  .pdata body, follow a terminator, and aren't a jump-table base. End = next
  known start. Bad picks surface as codegen "Unresolved conditional branch" /
  "cannot seal" errors. Codegen `loc_` labels are NOT proof an address is
  mid-function: codegen merges runs of tiny no-.pdata functions into one body.

## Input
- Keyboard/mouse driver: `--mnk_mode=true`; rebind with `--keybind_<button>=Key1,Key2`
  (e.g. keybind_a, keybind_start, keybind_left_trigger). Defaults from the SDK
  strings: A=Space/`;`, B=Backspace/`'`, Start=Return/X, Back=Tab/Z,
  LT/RT=Q/E, D-pad=Shift+arrows, right stick=mouse (`--mnk_mouse`, `--mnk_sensitivity`).

## Crash: GPU device lost on New Game (2026-10-09, open)
- Menu works with a controller (wired, xone driver, 045e:0b12). Starting a new
  game → ~10s into loading the GPU is lost and rex::FatalError aborts
  ("Graphics device lost") from VulkanCommandProcessor::EndSubmission at a swap.
- Kernel: NVIDIA Xid 31, MMU fault, GRAPHICS GPC0 GCC, VIRT_READ @ 0x3_F8000000,
  FAULT_PDE (page directory not present). Swap is only where it's noticed; the
  faulting draw/dispatch is earlier in the same submission.
- Side effect: another GPU process (python) got Xid 109 ctx-switch timeout.
- RULED OUT: sparse shared memory. With `--vulkan_sparse_shared_memory=false`
  (log confirms full 512 MB buffer) it faults at the identical VA 0x3_F8000000,
  across 3 runs, different GPCs. Deterministic address → not the shared-memory
  buffer. GCC = constant cache (UBO / descriptor reads), so suspect a uniform
  buffer or descriptor pointing at a bad/freed allocation.
- Standard validation (`--vulkan_validation_enabled=true`, log 009) shows nothing
  that explains it before the fault. Pre-fault oddities: vkCmdEndQuery without a
  begun query (occlusion query, 14:56:52), swapchain semaphore reuse
  (presenter). Everything after 14:57:06 is device-lost fallout.
- Last activity before the fault: async shader compile → "Skipping Vulkan frame
  presentation due to async placeholder draw usage", then Checkpoints dir
  creation, then ~8s quiet (level streaming), then the fault.
- RULED OUT: async shader compilation. `--async_shader_compilation=false`
  (log 010, no placeholder draws) still faults identically at 14:58:49.
- GPU-AV MASKS IT. `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=1 --vulkan_validation_enabled=true`
  (log 011; layer confirms GPU-AV active) → no Xid, zero GPU-AV reports, and it
  plays into the first in-engine cutscene. So the recompiled game isn't feeding
  obviously bad descriptors/constants (GPU-AV would have flagged them).
  Leading theory: plugin-side lifetime/sync bug (UBO or descriptor set
  freed/reused while GPU still reads it); GPU-AV's slowdown or extra
  allocations hide it.
- Perf in that run is meaningless (Debug + validation + GPU-AV).
- Workaround to keep playing: the GPU-AV command above (slow).
- RULED OUT (as far as validation can see): sync hazards.
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1 --vulkan_validation_enabled=true`
  (log 012) → no SYNC-HAZARD, same Xid at 15:02:19. Core validation also never
  reported destroy-while-in-use.
- Device has robustBufferAccess + VK_EXT_robustness2 enabled, so plain
  UBO/SSBO OOB reads wouldn't fault. Remaining suspects: stale/garbage
  descriptor, or driver codegen for a specific plugin shader (GPU-AV rewrites
  shaders, which would explain why it hides the fault).
- Stock Xenia users report "graphics device lost" in Gears 1/2 too (r/xenia),
  so this is likely an emulator-side weakness, not a recomp bug.
- Next: toggle plugin paths. Run with `--occlusion_query_enable=false
  --vulkan_dynamic_rendering=false` (logs show a real vkCmdEndQuery-without-begin
  bug every run); then bisect. If still crashing: `render_target_path_vulkan`.
  Release build later for real perf.
- RULED OUT (probably; app doesn't echo flags): occlusion queries + dynamic
  rendering. `--occlusion_query_enable=false --vulkan_dynamic_rendering=false`
  (log 013) → same Xid at 15:04:57.
- render_target_path_vulkan accepts fbo / fsi; default appears to be fbo (fsi is
  only mentioned as a fallback). Next: `--render_target_path_vulkan=fsi`.
- DEAD END: `--render_target_path_vulkan=fsi` aborts on the first draw (log 014):
  assert in RenderTargetCache::last_update_accumulated_render_targets
  (cache.h:492), reached via VulkanRenderTargetCache::GetLastUpdateRenderingAttachments
  from IssueDraw. The plugin's FSI path calls host-RT (fbo-only) code; FSI is
  broken in this SDK build. Stick with fbo.
- If paths don't matter, find the faulting draw directly: capture with
  GFXReconstruct up to the crash and replay, or bisect PM4 submissions.
  GFXReconstruct 1.0.5 now installed. Capture with tools/gfxr_capture.sh
  (writes to ~/Documents/Programming/Recomp/Gears/gears1_captures, outside the repo).
  Memory tracking must be `unassisted`: page_guard uses SIGSEGV (owned by the
  runtime) and userfaultfd is blocked (vm.unprivileged_userfaultfd=0). File
  flush is on because the crash is abort(). Smoke test: ~2 MB/s, reaches the
  title screen, file parses (37 frames / 331 pipelines in 25s).
- NVIDIA driver is 615.78; trying another driver branch is a cheap
  cross-check if the plugin side stays clean.

### GFXR capture + offline replay (2026-10-09)
- Hotkey-trimmed capture (F12 on the New Game menu) caught the crash:
  gears1_captures/gears1_trim_trigger_20261009T152257.gfxr (191 MB, 1 frame,
  12 s). Live Xid at 15:23:09.
- `gfxrecon-replay --swapchain offscreen <file>` REPRODUCES the identical Xid 31
  (GCC, 0x3_F8000000) in under a second. (With a window, replay instead gets
  Xid 13 "WIDTH CT Violation" because it shrinks the 4K swapchain; ignore that.)
  So the bug is fully contained in the Vulkan command stream; game/runtime/
  timing are irrelevant.
- Bisection by patching the capture (set api_call_id of draw/dispatch blocks
  to an unknown id → replay skips them; block header = u64 size, u32 type,
  then u32 api_call_id; block ordinal == gfxrecon-convert "index"):
  first faulting work item in submission order = block 34903, vkCmdDrawIndexed
  (30876 indices) in cb 20156, submitted at 37379 (the submit right before the
  first VK_ERROR_DEVICE_LOST at 37400).
- That draw: depth-only pass (no color attachments, 880x2992 target, 422x422
  viewport — shadow-map atlas), pipeline 697, layout 556 = sets
  [117 SSBO shared memory, 63 five UBOs, 62 empty, 62 empty]; VS module 494,
  FS module 408. Descriptors/UBO ranges/buffers all valid and alive.
- Shader-replacement tests (`--replace-shaders`, file name sh<module id>):
  - trivial VS → no fault; trivial FS → fault. VS 494 is the trigger.
  - VS with shared-memory loads replaced by 0 → no fault.
  - mask/clamp every dynamic index (7 shared-memory loads to <512 MB, 12
    float-constant c[a0+n] reads to 0..255, both together) → STILL faults.
    Only 19 dynamic accesses exist in the VS; all guarded. Function-array
    register file uses constant indices only.
  - VS outputs overwritten with constants (loads become dead) → still faults.
  - Clamping shared-memory load indices to a max M: ok for M < byte
    0x10DF8000, fault once reads may reach 0x10DF8000..0x10DFC000 (the vertex
    data, just below the draw's index buffer at 0x10E20000).
  - That range IS sparse-bound (0x10C00000-0x11000000 → memory 95039, 4 MB,
    allocated, never freed, no further sparse binds in the capture).
- So: a VS reading valid, bound, in-range shared memory faults the GPU at an
  unrelated fixed VA, unless the shader is rewritten (GPU-AV) or the loads
  removed. No API-level invalidity found. Leading suspects now: NVIDIA
  driver (shader compiler / sparse-residency handling, driver 615.78) rather
  than the plugin or the recomp. Caveat: the user's live run with sparse off
  also faulted at the same VA, which a sparse-only bug wouldn't explain.
- Tools: tools/gfxr_blocks.py (block walker), tools/gfxr_bisect.py
  (global draw bisect). Address-window bisect was ad hoc (SClamp on the VS loads).
- Sparse-off capture (gears1_captures/nosparse/, 2.76 GB, 46 frames, F12 after
  New Game): live game got into the intro cutscene ("14 years after E-Day")
  without faulting; it only faulted on Alt-F4 teardown (15:49:19, same Xid
  signature — teardown freeing GPU objects reproduces the same VA/GCC fault).
  Its offscreen replay faults in frame 1-2, also with `--sync`. Sparse is NOT
  the variable. ("Failed to map handle for object id 51" in replays = old
  swapchain handle, harmless.)
- Write-after-submit theory REFUTED: the faulting draw's vertex data comes
  from vkCmdCopyBuffer at block 34899 (same cb, 4 blocks before the draw):
  upload buffer 17890 (memory 17891, 2 MB, persistently mapped) +0x97000 →
  shared memory 0x10DE0000-0x10E20000. Decoded all 5 FillMemory snapshots of
  memory 17891 (pure-Python LZ4; FillMemory block = 12B block header + u32
  meta id + u64 thread + u64 memory id + u64 offset + u64 size, then LZ4
  data): the source region is byte-identical before and after submit 37379.
  The data is sane big-endian skinned vertices (40 B: float3 pos, packed
  normal, float2 uv, bone index 0x13 / weight 0xFF) → recomp output is fine.
- spirv-opt -O and -Os versions of VS 494 still fault (not tied to exact
  SPIR-V shape).
- Load bisect: of the 7 shared-memory loads, zeroing ONLY load #5 (vertex
  dword 9, unpacked as 4x u8 → float, i.e. blend weights/color) removes the
  fault; zeroing any other load doesn't. So valid weight data drives the VS
  down a skinning path that faults the GPU with all memory accesses in range.
- Current best explanation: NVIDIA driver (615.78) bug triggered by this
  shader + data. Self-contained repro = upto_fault.gfxr (capture 1 with draws
  after block 34903 disabled) replayed offscreen.
- Unexplained: why heavy slowdowns (GPU-AV, GFXR live) sometimes survive.
  Possibly pipeline 697 isn't ready under async compilation, so the draw is
  skipped as a placeholder; unverified.
- Narrowed further inside VS 494 (all via --replace-shaders on upto_fault.gfxr):
  - load #5 → r2 = UBYTE4N blend weights. Skinning is 4 blocks, each
    `if (r2.<c> > 0) { a0 = clamp(floor(r1.<c>*c255.z+.5)); r4 += w*(bone rows
    c[a0+7..9] · pos) }`.
  - Zeroing r2 at only read site 7 (the 4th-bone predicate, r2.w > 0, SPIR-V
    block %1080, lines ~1501-1700) removes the fault; bones 1-3 don't matter.
  - Within that block, truncation bisect is confounded by DCE (the block's
    final OpStore to r4 keeps it all live).
  - Not the cause: dynamic UBO indexing (all 12 indices → constant 7 still
    faults); NaN/Inf in the 4th-bone result (sanitized → still faults);
    extreme positions (gl_Position clamped to ±1e6 → still faults).
- SDK source: github.com/rexglue/rexglue-sdk cloned at
  ~/Documents/Programming/ReXGlue/rexglue-sdk, HEAD = "Release v0.10.0" (matches
  the binary SDK). Vulkan backend in src/graphics/vulkan/, device setup in
  src/ui/vulkan/vulkan_device.cpp (robustBufferAccess enabled at :630,
  robustness2 nullDescriptor at :730). A from-source build is possible via
  REXSDK_DIR in generated/rexglue.cmake.
- Driver 615.71.09 (downgraded from 615.78): upto_fault.gfxr still faults
  identically. Not a 615.78 regression.
- Bone blocks 1-4 are textually identical apart from register numbers /
  swizzles; nothing semantically special about bone 4.
- Not shader-heap pressure: replacing all 445 other VS/FS/CS modules with
  trivial ones still faults.
- SINGLE-DRAW REPRO: gears1_captures/single_draw.gfxr = capture 1 with every
  draw/dispatch except block 34903 disabled → still faults. Minimal repro.
- Pipeline 697 state is unremarkable: no vertex input (VS pulls from the
  SSBO), triangle list, depth-only D24S8 dynamic rendering, depth bias on,
  7 dynamic states. Index buffer = shared-memory buffer 66 @0x10E20000 (u16).
- Translator hints irrelevant: DontFlatten → None/Flatten, DontUnroll → None
  all still fault.
- Scratch dir was wiped by the reboot for the driver downgrade; ad-hoc
  variant scripts were lost (tools/gfxr_blocks.py, tools/gfxr_bisect.py survive).
- ROOT CAUSE FOUND (workaround): the shaders' `OpExecutionMode RoundingModeRTE 32`.
  tools/gfxr_try_shader.sh tests a replacement module against single_draw.gfxr.
  Removing Restrict/NonWritable on the SSBO → still faults; removing
  SignedZeroInfNanPreserve → still faults; removing RoundingModeRTE → OK.
  Stripping RTE from all 328 modules that declare it: both full captures
  replay clean (0 Xid; nosparse plays all 46 frames at ~55 fps).
  RTE is the GPU's default fp32 rounding anyway, so dropping the explicit mode
  should be behaviour-neutral; the NVIDIA driver mishandles it on this VS
  (both 615.71.09 and 615.78) → driver bug, worth reporting with
  single_draw.gfxr.
- Fix: SDK patch on branch fix/nvidia-rte-workaround in
  ~/Documents/Programming/ReXGlue/rexglue-sdk: new plugin cvar
  `vulkan_shader_rounding_mode_rte` (default true = upstream behaviour) gating
  both emission sites (src/graphics/pipeline/shader/spirv_translator.cpp
  Features ctor, src/graphics/vulkan/pipeline_cache.cpp host shaders). Set it
  false for this project. Only the rexgpu-xenos plugin needs rebuilding.
- Built: `cmake --build out/build/linux-amd64 --config Debug --target rexgpu-xenos`
  in the SDK clone (after `git submodule update --init --recursive --depth 1`
  and `cmake --preset linux-amd64`) → out/linux-amd64/Debug/librexgpu-xenosd.so.
  Project configure (the preset's clang-20 must be overridden):
  `cmake --preset linux-amd64-debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_PREFIX_PATH=~/Documents/Programming/ReXGlue/linux-amd64
  -DGEARS1_GPU_PLUGIN_OVERRIDE=~/Documents/Programming/ReXGlue/rexglue-sdk/out/linux-amd64/Debug/librexgpu-xenosd.so`.
  CMakeLists copies the override after the stock plugin, and copies
  config/gears1_recomp.toml (gpu_plugin = "xenos", RTE off) next to the exe.
  Smoke test: boots to title from config alone, no Xid, no unknown-cvar warning.
  Replay verification of the fix was done on 615.71.09, and repeated on
  615.78.08 after restoring the driver: single_draw.gfxr still faults with the
  stock shaders, and with RTE stripped single_draw + both full captures replay
  with 0 Xid.
- Config: the app loads <exe dir>/gears1_recomp.toml (flat `key = value`);
  plugin cvars set there are held until the plugin registers them. Env vars
  REX_<CVAR_NAME> also work.
- Next: replay upto_fault.gfxr on another NVIDIA driver branch (one command);
  if clean → driver bug, report to NVIDIA with the capture (contains game
  data — user's call). Further narrowing possible by bisecting the VS code
  after load #5 (early-exit at successive points).
- Benign: NtCreateFile 0xC0000035 on D:\, D:\WarGame, D:\WarGame\Checkpoints
  (directory already exists when creating save dirs).

## Leads
- CRASH at end of chapter 1 (2026-10-11, boarding the Raven), core
  844713: SIGABRT "free(): invalid size" on the game thread inside ReXGlue's
  XamUserWriteProfileSettings_entry (sized operator delete of a setting
  buffer), called from sub_828CD758 while the game saves its title settings
  (63E83FFF 1000 B and 63E83FFE 214 B written; 63E83FFD left 0 bytes in
  ~/.local/share/gears1_recomp/4D5307D5/profile/User). Chunk header and the
  memory around it are zero.
  ROOT CAUSE FOUND (2026-10-11, second core 66738: picking up the end-of-level
  COG tags, with --gears1_native_input=false --gears1_mouse_look=false and
  libc_malloc_debug, same abort). The game writes 63E83FFD as a BINARY setting
  with size 0 and a non-null pointer. xam_user.cpp did `bytes.resize(size);
  memcpy(bytes.data(), ptr, size)`; data() of an empty vector is null, memcpy
  is declared nonnull, so clang 22 (our SDK build) assumes size > 0 and drops
  resize's size check: operator new(0), then memset(p+1, 0, size-1 = SIZE_MAX).
  glibc's memset stores the head and tail vectors first; the tail wraps to
  p-0x100, so exactly p-0x100..p+0x100 got zeroed (seen in the core) and the
  loop bound wraps, so it returns without faulting. The free of that buffer
  then aborts. The prebuilt runtime (clang 20, Aug) keeps the size check,
  which is why earlier runs got past the chapter end: the regression is
  the locally built runtime_override, not our code.
  Fix (SDK fix/xam-profile-settings 6a2bdd0 + 044b3f1 off main, cherry-picked
  onto fix/gears1-xenos-rendering b28cfed + 2b96123, both pushed): construct
  the vector at size and only
  memcpy when ptr && size; also `virtual ~Setting()` in user_profile.h
  (AddSetting deletes the replaced BinarySetting through unique_ptr<Setting>:
  non-virtual dtor, sized delete of 0x20 on a 0x38 object, leaked vector).
  Upstream: issue rexglue/rexglue-sdk#502, PR rexglue/rexglue-sdk#503
  (fix/xam-profile-settings reworded to fix(xam): subjects, now 2905c21 +
  73543ad on c94f5eb; same content as b28cfed + 2b96123 on the work branch).
  Verified in the new disassembly: size 0 skips the allocation. CONFIRMED in
  game (2026-10-11): level completed; the 18:18 profile save wrote the 0-byte
  63E83FFD again with no crash.
- Mouse look works (2026-10-11); sensitivity 8 was a bit low at the
  user's DPI; default is now 12 (user-confirmed).
- BLOOD POOL = decal depth bias too small on the host (2026-10-11).
  `--gears1_probe_depth_bias_scale=4` (game side) and
  `--vulkan_depth_bias_scale=4` (host side) both remove the glitch. Cause:
  ReXGlue converts the guest's absolute float24 poly offset with a factor
  (2^24, draw.h kD3D10PolygonOffsetFactorFloat24) that is exact only for
  depths in [0.5, 1); Vulkan's float-depth bias unit is 2^(e-23) per
  primitive, so at smaller depths the host bias shrinks with the exponent,
  and UE3's reversed Z puts most of the scene at small depth values.
  Fix candidate (SDK, uncommitted): VK_EXT_depth_bias_control with
  FORCE_UNORM + depthBiasExact (unit is a constant 2^-24 on D32_SFLOAT, so the
  factor gives the absolute offset), x0.5 for D24FS8 because host render
  targets store float24 as [0, 0.5]. Cvar vulkan_depth_bias_force_unorm
  (default on). RTX 4080 / 615.78.08: depthBiasControl, force UNORM and
  exact supported, floatRepresentation not. Canary cde5d85ec (opt-in shader
  depth offset for decal draws) is the alternative for GPUs without it.
  First trace capture was lost (probe file opened "w"; now appends).
  CONFIRMED in game 2026-10-11: with the fix on (defaults, no bias scales)
  blood pools stay clean; bullet decals unchanged.
  Committed: SDK 7823c15 on fix/gears1-xenos-rendering (diagnostic
  vulkan_depth_bias_scale dropped); Canary ecd6163b7 on branch
  vulkan-depth-bias-control (from upstream 103257c5a). Comparison Canary
  build = local/gears1-compare (that commit + the fallthrough fix).
- Probes (2026-10-10, PROBES.md "Usage"): tools/gen_hooks.py + hook_table.py
  replaced gen_d3d_observe.py (backup: backup/2026-10-10-pre-probes/);
  src/probes.cpp adds pass/policy tracking, trigger-file draw traces, a skip
  list and a game-side depth bias override. Release build rebuilt (full,
  after the folder move) and links the overrides; not run yet (GPU busy).
  The debug build tree still needs its full rebuild.
- UE3 RHI layer found (2026-10-10): table in NATIVE_RENDERER.md "UE3 RHI
  layer". UE3 uses **reversed-Z** with the float depth surface (flag
  0x82BFA554 set by RHISetRenderTarget 8254EE20) and negates depth bias
  (RHISetRasterizerState 8254E7A0). Check this against the blood-pool
  depth-bias test: if the host applies the bias with the wrong sign or scale
  for reversed float24 depth, decals z-fight.
- Per-mesh depth bias path (2026-10-10): FMeshDrawingPolicy_SetMeshRenderState
  825C3EF8 reads the mesh element's DepthBias (+0xF0) / SlopeScaleDepthBias
  (+0xF4) and calls RHISetRasterizerStateImmediate 8257BD98, which negates
  both under reversed-Z like RHISetRasterizerState. All guest depth bias
  ends in D3DDevice_SetRenderState_DepthBias/SlopeScaleDepthBias
  (82229460/82229398): one choke point for probes/overrides (PROBES.md).
- Ghidra has no defined strings in this project; strings were pulled from
  .rdata/.data dumped via the Ghidra MCP socket into the session scratchpad
  (not in the repo). Shader type and vertex factory names are UTF-16BE.
- The 2007 Gears of War PC build (x86) is nearly the same game: load it in
  Ghidra alongside the XEX and match functions by strings/constants/call
  graphs to name things.
- Startup logs NtCreateFile failures for Movies\*.xxx; the .bik files exist.
  Probably the engine probing extensions — unconfirmed.

## Next
0. Native renderer: plan in NATIVE_RENDERER.md (D3D-level hooks, runtime
   shader capture + translation, emulated path kept as fallback/reference).
1. Play past the menu with a controller; fix whatever crashes.
2. Consider defaulting gpu_plugin=xenos in Gears1RecompApp::OnPreSetup.
3. Later: native renderer instead of Xenos emulation; macOS arm64 via
   MoltenVK; Steam Deck tuning.
4. Later (parked 2026-10-10): predicated tiling emulation bug - black
   polygons/shadows with UE3 MSAA predicated tiling on (Canary 04f85416e
   and ReXGlue). Recomp avoids it with gears1_black_shading_fix; the native
   renderer would stub tiling. Worth doing for other UE3 recomps that rely
   on tiling, or as a Canary contribution. First step: GFXR capture of
   Canary tiling on vs patched in the same scene, diff per-tile resolves.
- UPSTREAM CROSS-CHECK (2026-10-11, xenia-canary/game-compatibility#44 + Edge):
  * Fallthrough fix: Xenia Edge (wmarti/xenia-edge f145711a4, 2026-07-11,
    "[Vulkan] Replace main-loop OpSwitch case fall-through with explicit
    jump", same NVIDIA device-lost in this title) is code-identical to ours.
    Not in Canary yet (Edge backport PRs #1131, #1147 merged without it).
  * Decal Z-fighting: Canary has depth_bias_shader_offset (from Edge, opt-in,
    routes decal draws through shader depth) and Edge had
    depth_bias_decal_clamp. Nobody uses VK_EXT_depth_bias_control; ours is the
    fixed-function alternative (keeps early-Z, default on where supported).
  * Black triangles from MSAA predicated tiling: thread reports Canary
    cc4981a fixes them WITHOUT the Black Shading Fix patch via
    window_offset_in_edram=false + xenos_sample_positions=true. Check whether
    ReXGlue's tiling/window-offset path can do the same (we patch the XEX).
  * Lens flares: Canary occlusion_query=fast/strict (ZPD) fixes unoccluded
    flares. Audio stutter: use_dedicated_xma_thread=false.
- CANARY DEPTH BIAS A/B (2026-10-11, local/gears1-compare 127cf75e5 = depth
  bias a51e0ef7d + fallthrough on upstream 2372a5410; Vulkan FBO,
  window_offset_in_edram=false, xenos_sample_positions=true, no game patches;
  log confirms VK_EXT_depth_bias_control + depthBiasExact + force-UNORM).
  Note: Canary's log "CONFIG DUMP" is the config FILE, not effective values;
  command-line flags take precedence.
  * Run A (ours on, depth_bias_shader_offset off): blood pools CLEAN. One
    blood test briefly showed a black SQUARE where the grown pool would be
    (once). Random black polygons on Marcus still present (e.g. when Dom asks
    which way to go), fewer than before, so the two tiling cvars do NOT fully
    replace the Black Shading Fix patch on Vulkan FBO here.
  * Run B (ours off, depth_bias_shader_offset on): blood pools also CLEAN;
    same black-polygon occurrences as A. => both fixes work for the pools;
    the black polygons are independent of either. No noticeable performance
    difference vs A (user).
  * Run C (neither): blood pools FLICKER (control confirms the spot
    reproduces). => A/B verdict: both fixes equivalent in result and cost on
    this title; ours is default-on and fixes the bias size for every biased
    draw, the shader offset is opt-in and limited to decal-like draws.
- CHECKPOINT RELOAD DIFFERENCE (2026-10-11, user): in Canary, blood pools and
  decals are GONE after reloading a checkpoint; in our recomp they REMAIN.
  UPDATE (run C): a pre-reload blood pool also stayed once in Canary, so NOT
  recomp-only; low priority, possibly original game behaviour.
- CANARY AUDIO (2026-10-11, noticed during run C on local/gears1-compare,
  upstream 2372a5410): gunfire and voice lines play only from the LEFT
  channel; other sounds are stereo. Confirmed in all three runs (A/B/C):
  friendly + enemy voice lines and gunfire left only; everything else is
  positioned correctly (3D-panned or stereo), including the vault grunts.
  Not investigated. Unknown yet whether the recomp does the same, or whether
  it is new with 2372a5410 ("[APU] Round XMA samples instead of
  truncating"). User's output device is a 6-channel
  Scarlett 16i16 (48 kHz).
- Canary PR opened 2026-10-11: xenia-canary/xenia-canary#1289 "[Vulkan] Apply
  depth bias in constant units with VK_EXT_depth_bias_control"
  (vulkan-depth-bias-control a51e0ef7d on 2372a5410, force-pushed; body
  ends with an AI-use disclosure per the user).
  * Review round 1 (goldislead, changes requested 2026-10-12): enum inline at
    the call, comment trimmed to a pointer at kD3D10PolygonOffsetFactorFloat24,
    floatRepresentation dropped. Amended to c0edb2718, run A re-tested clean
    (log: depthBiasControl, leastRepresentableValueForceUnormRepresentation,
    depthBiasExact), force-pushed. User replies to the review personally.
- CONVENTION CLEANUP (2026-10-12/13). SDK: fix/gears1-xenos-rendering
  rewritten to 9 commits (diagnostic commits moved to local/gears1-debug,
  old tip tagged backup/gears1-xenos-rendering-2026-10-12; A/B cvars removed;
  depth bias + sample locations detected via VulkanDevice Properties/
  Extensions/functions; kXenosSamplePositions2x in draw_util; gamma transfer
  conversion gated on gamma_render_target_as_unorm16), force-pushed; then the
  duplicate xam commits were dropped from it (GPU-only, 78122c9, 7 commits).
  New integration branch gears1-main (bc18f9d) = main + the GPU topic +
  fix/xam-profile-settings; the runtime is built from it. Gears1 is a git repo
  (43de0b9 initial, dcf3798 cleanup). User-tested: input, mouse look, blood
  pools, no spikes/tint/device lost.
- UE3 PACKAGES UNPACKED (2026-10-14, tools/upk.py): CookedXenon/*.xxx are
  UE3 packages version 374 / engine 2451, big-endian. 508 uncompressed, 1224
  with LZO-compressed chunks (compression flag 2, not LZX), 13 whole-file
  compressed (Core, Engine, GameFramework, the online subsystems,
  UnrealScriptTest, WarfareGame, WarfareGame_LOC_*: little-endian chunk header
  + LZO blocks around a normal package). Export entries are 17 words (no
  trailing package flags). 448 packages carry a ShaderCache export
  "SeekFreeShaderCache" (Engine.xxx: 1.75 MB of global shaders). Scanning them
  for Xenos shader containers (flags 0x102A11xx, header size, microcode size;
  header immediately followed by the microcode) gives 18,595 unique shaders
  (18,433 PS, 162 VS). Of the 830 shaders dumped during play, 746 are
  byte-identical in the caches; the 84 others are 83 VS (D3D patches VS
  microcode in place for the vertex declaration at bind time) and 1 PS.
  => The full shader set can be translated offline (AOT), no runtime
  translation needed; VS fetch patching must be handled by the translator.
- OFFLINE SHADER TRANSLATION (2026-10-14): tools/upk.py --extract-shaders
  wrote the 18,595 shaders of the packages' shader caches (15 s) to
  gears1_captures/shader_cache; tools/translate_shaders.py ran
  rexgpu-shader-tool (gears1-main translator + the tool commit, built in a
  temporary worktree) on all of them: 18,595/18,595 translate and pass
  spirv-val (Vulkan 1.2 with --uniform-buffer-standard-layout
  --scalar-block-layout, the layout features the runtime's device enables;
  without them every module fails the Block layout rule). 15 s with 10 jobs.
  858.7 MB of SPIR-V (avg 46 KB, unoptimised); on a 60-module sample
  spirv-opt -O saves 26%, zstd -19 leaves 13%. Metal: SPIRV-Cross
  (tools_ext) converts 300/300 sampled modules at MSL 2.3 (256/300 at MSL
  2.1: demote-to-helper needs 2.3). Caveats: valid is not correct (no
  rendering check yet); only the default shader modification per shader
  (state-dependent variants not generated); vertex shaders are the
  unpatched originals (D3D patches their vfetch at bind time for the vertex
  declaration, so a native renderer must translate per declaration or make
  fetch formats dynamic).
- OFFLINE == RUNTIME TRANSLATION (2026-10-14): ran the game once with a
  plugin that dumps the Vulkan backend's translated SPIR-V (dump_shaders;
  SDK local/shader-tools tip, Vulkan had no Translation::Dump call) from
  a separate run folder (gears1_captures/dumprun, hard-linked exe + that
  plugin). At startup it translated the pipeline storage (1,834 pipelines):
  1,388 translations of 839 shaders. tools/compare_shader_translations.py
  re-translated each offline with rexgpu-shader-tool --rte (this RTX 4080's
  features = the tool's all-on set + RoundingModeRTE) and the runtime's
  modification: 1,388/1,388 byte-identical (1,280 PS from the package
  caches, 6 PS + all 102 VS from the runtime's own microcode: every VS the
  runtime used is the bind-time-patched form, none matches the packages'
  162 originals). Control without --rte: 1,388/1,388 differ. The runtime's
  ucode dump is host byte order; 748 of its 839 programs match package
  programs byte-swapped. => Offline translation reproduces the runtime
  exactly given the variant (modification) and device features; what a
  native renderer must add is the set of variants per shader and the VS
  vertex-declaration patching.
- AOT SHADER VARIANTS (2026-10-14). Runtime variants observed (pipeline
  storage, 1,388): VS vary only in the interpolator mask (= VS writes & PS
  inputs, so pair-dependent; 85 equal the VS's own writes, 17 a subset);
  every other VS field is 0 (no clip planes, point size, vertex kill,
  tessellation; host type kVertex). PS vary only in depth/stencil mode:
  kEarlyHint exactly when the shader's analysis allows implicit early Z
  (679/679), kNoModifiers otherwise (78/78), plus the float24 emulation
  modes (532) a native renderer with host depth formats doesn't need;
  centroid only with MSAA; param gen (VPOS) in 3 variants. Analysis of all
  shaders (rexgpu-shader-tool now prints it): every PS input mask is
  contiguous from interpolator 0 (18,436/18,436); no shader uses dynamic
  register addressing; 14,472 of 17,070 PS programs allow early Z.
  Rule set for a native renderer (tools/translate_shaders.py
  --native-variants): VS mask 0xFFFF (outputs are zero unless written),
  PS own input mask (so input i = location i on both sides, any pair
  links), PS kNoModifiers + kEarlyHint when allowed. Same results as the
  runtime's per-pair mask (unwritten inputs read 0 either way). All 18,595
  shaders -> 34,166 modules (162 VS, 34,004 PS), all pass spirv-val, 36 s
  with 10 jobs, 1.6 GB unoptimised SPIR-V. Still open: VS bind-time
  patching per vertex declaration (the 162 originals are unpatched; every
  VS the runtime used was patched), param gen for VPOS shaders (position
  recorded by the runtime, 3 seen).
- HOST VERTEX INPUT (2026-10-14, SDK local/shader-tools). The packages'
  vertex shaders are unpatched: every fetch is `vfetch_full rN, r0.x, vf0`
  with no format; D3D's patching at vertex declaration bind time writes
  format, offset, stride, signedness, full/mini and the destination
  swizzle (seen by diffing 90 runtime-patched VS against their originals).
  All 1,626 fetches index r0.x from vf0 before r0 is written, so the fetch
  index is the vertex index. The shader container's vertex shader record
  lists the elements (fetch instruction address, D3DDECLUSAGE, usage
  index; layout per hedge-dev/XenosRecomp): 15 usages across all 162 VS
  (POSITION0, BLENDWEIGHT0, BLENDINDICES0, NORMAL0, TANGENT0, BINORMAL0,
  COLOR0-1, TEXCOORD0-3/5-7), mapped to fixed locations 0-15 so a
  pipeline's vertex layout depends only on the declaration. The SPIR-V
  translator can now load a float4 vertex input for a mapped fetch
  (rexgpu-shader-tool --host-vertex-input; translate_shaders.py
  --native-variants uses it for every VS): 162/162 VS map every fetch, pass
  spirv-val and no longer read shared memory. Host default fill (0, 0, 1
  for missing y/z/w) matches all 478 patched fetch swizzles. Formats the
  game's declarations use: FMT_32[_32[_32[_32]]]_FLOAT and FMT_8_8_8_8
  (normalized or integer, signed or not). For the native renderer:
  byte-swap vertex buffers on upload; 8_8_8_8 integer (unnormalized, e.g.
  packed normals) has no SDL_GPU float format (no USCALED), so convert those
  elements to float (or half) on upload.
- VPOS PIXEL SHADERS (2026-10-14): the pixel shader record's word at +0xC
  holds the VPOS (PsParamGen) register in bits 8:15 (XenosRecomp's
  "svPos register") and a use flag in bit 1: 12 shaders have 0x106
  (register 1, flag set), all others 0x004/0x008/0 (flag clear). The 3
  runtime variants with param gen are exactly 3 of those 12, at register 1;
  every other shader the runtime ran had no param gen. Native rule: shaders
  with the flag always get PsParamGen at that register (param_gen_point 0;
  the game draws no points with them), keeping their contiguous input mask
  (the param gen register's input is declared but not loaded, so locations
  still match the 0xFFFF vertex shaders). Offline translation with the
  runtime's modification is byte-identical for all 3; the native variants
  are valid (they read zero-initialised VS outputs where the runtime's
  narrower pair mask used constant zeros). --native-variants: 12 shaders,
  24 modules with param gen; still 34,166 modules, all valid.
- NATIVE RENDERER SKELETON (2026-10-14, src/native_renderer.cpp,
  gears1_native_renderer, default off): post hooks on D3D device init
  (8223A840) and Swap; the main thread opens an SDL_GPU window at the
  guest back buffer size, then clears it (cycling colour) and draws an
  ImGui overlay (device, back buffer, guest/presented frames, frame time)
  once per guest frame, skipping when the previous present is still
  queued. ImGui core = the runtime's exported 1.92.5 in our own context;
  SDL_GPU backend vendored in third_party/imgui. 45 s run: Vulkan window
  1280x720 (from the game's present parameters), no errors; the emulated
  window keeps running the game (detached mode would hang: D3D waits on
  the GPU consuming its PM4).
- NATIVE TEXTURES, first cut (2026-10-14, src/native_textures.cpp): a
  texture object's GPU fetch constant is the six dwords at +0x1C (D3D
  SetTexture 82220858 reads tex+0x1C..+0x30 and merges them into the
  device's sampler state at dev+0x400+sampler*24; the texture pointer goes
  to dev+0x2FF0+sampler*4). Pre hook on SetTexture decodes each new
  texture's base level (2D only) with the runtime's exported texture
  helpers (GetSubresourcesFromFetchConstant, Untile, CopySwapBlock,
  FormatInfo; physical address via the 0xA0000000 mirror) into
  R8/R8G8/R8G8B8A8/BC1-5/RGBA16F/R32F; the main thread uploads and the
  overlay shows a paged browser (guest channel order, no swizzle) with
  counts per unsupported reason.
- NATIVE TEXTURE MIPS (2026-10-14): every level is decoded with the
  runtime's GetGuestTextureLayout (row pitch per level, mip offsets from the
  mip address) and GetPackedMipOffset (block offset inside the packed tail
  for levels >= packed_level; this includes the base level of textures of
  16 texels or less, which the first cut decoded from the wrong place).
  Level sizes are base >> level; the SDL_GPU texture gets all levels
  (missing base: levels from mip_min). Array slices of stacked textures
  beyond the first are not uploaded yet. The browser's thumbnails sample
  the mips (ImGui's sampler: linear mips, full LOD range), and clicking one
  opens every level at its own size; the overlay now gets mouse input from
  an SDL event watch on the native window.
- NATIVE OVERLAY FREEZE (2026-10-14, fixed): the mips commit's SDL event
  watch made our ImGui context current for every event. Watches run on
  whichever thread pushes the event (the runtime pushes from guest
  threads) and ImGui's current context is process-wide, so a guest thread
  could switch it while the main thread was inside the runtime's own ImGui
  frame: the main thread then hung in ImGui::Begin and the game window
  froze after a while (sooner with more input). The watch now only queues
  the native window's mouse events; Present feeds them to ImGui. Also: mouse
  look captured whichever window had keyboard focus, so it grabbed (and hid
  the cursor in) the native window; it now never captures that window.
- NATIVE VERTEX/INDEX BUFFERS (2026-10-14, src/native_buffers.cpp): UE3
  never draws with a vertex declaration: RHISetStreamSource (8254E728) and
  RHISetBoundShaderState (8254E948) both store 0 at +0x2D10, and the
  pre-draw commit (822346A8) only patches the vertex shader when it is
  non-zero. The bound vertex shaders are already patched, so the vertex
  layout comes from the shader at +0x3084: its shader record (header copy
  at +872) lists each element's fetch instruction (address in instructions
  from the program start, i.e. microcode + the record's program offset,
  after the literal constants), usage and usage index; the patched
  vfetch gives fetch constant, format, signedness, normalization, offset
  and stride (mini fetches reuse the previous full fetch's). Index buffer
  object: Common bit 31 = 32-bit, bits 29-30 endianness, +0x18 address,
  +0x1C size in bytes (sizes match the draws' index ranges); D3D ignores
  DrawIndexedVertices' BaseVertexIndex (r5). Buffers are byte-swapped,
  uploaded on first draw and re-read only after a write to their pages
  (the runtime's physical memory write watches, as the emulated GPU's
  shared memory uses them; a write unprotects and stamps its 256 KB
  block), re-uploaded if their hash changed, released after 300 unused
  frames; 8_8_8_8 integer elements get a float4 copy. SDL_GPU never enables
  primitive restart on Vulkan, so strips with reset indices will need
  splitting. In a level: no capture problems and the draw viewer's
  wireframe of Marcus is right. Capture cost in a heavy scene (3387
  draws): hashing every used buffer once per frame took 3 ms; with write
  watches 1.2-1.3 ms (9 KB re-hashed per frame, uploads only while new
  meshes stream in), and with hash maps instead of std::map for the
  buffer tables 0.9 ms, about 0.27 us per draw.
  Not yet: DrawVertices and the UP draws (BeginVertices/BeginIndexedVertices
  write vertices into the command buffer).
- IMPORT CALL COUNTS (2026-10-25): local/gears1-debug was restacked on
  gears1-main (old tip 2b96123; kept only its RoundingModeRTE cvar, frame
  stepping hotkeys and optional host-coherent flush; the shader tool lives
  on local/shader-tools) and gained 520715f: every REX_HOOK / REX_HOOK_RAW /
  REX_STUB* counts calls, first call time and up to 8 guest return
  addresses, and `--import_call_counts_file=<path>` rewrites them as TSV
  every 5 s. The Release runtime_override/ and staged plugin are now that
  build; the 2026-10-11 runtime is in runtime_override_2026-10-11/. A 45 s
  boot run hit 117 hooks: 24 of Gears' 92 xam imports, 93 of its 144 kernel
  imports. Hottest: RtlEnter/LeaveCriticalSection (2.6M each),
  XamInputGetState (357k), the spin lock / IRQL set (~300k),
  KeDelayExecutionThread (278k, one caller, 82613800). The table joining
  these with the real 17559 xam is ~/Documents/Programming/Recomp/X360/
  COVERAGE.md (outside git; tools/build.sh rebuilds it).
- NATIVE RENDER TARGETS (2026-11-07, src/native_targets.cpp;
  D3D Clear, Resolve and Swap signatures and the surface object layout in
  NATIVE_RENDERER.md "D3D API map"). Pre hooks on Clear and Resolve record
  each guest frame's operations from the bound surfaces; the main thread
  replays every recorded frame in order and presents the front buffer that
  Swap names (r4). Each guest surface is a host texture keyed by EDRAM base,
  format and size (1x: no MSAA, see "MSAA resolve" in NATIVE_RENDERER.md);
  each Resolve destination a host texture keyed by the destination fetch
  constant's address, format and size, and SetTexture skips decoding those.
  Clears draw a rectangle (rect.vert + clear.frag / clear_depth.frag, stencil
  as the pipeline reference); Resolve copies with copy.frag (nearest texel,
  red/blue swap, exponent bias scale). One title-screen frame: depth/stencil
  cleared to Z 0 (reversed Z), stencil 128; 7e3 scene colour cleared and
  resolved into a 16_16_16_16_FLOAT texture with bias -3 (x0.125); depth
  resolved into a 24_8_FLOAT texture; three resolves of a 322x182
  16_16_16_16_FLOAT target into a 16_16_16_16_EXPAND texture (bloom); the
  8888 back buffer resolved (red/blue swapped) into two textures, one of them
  the front buffer. 17 operations, 6 surfaces, 6 resolve destinations per
  frame; the front buffer arrives at the window (black while nothing draws
  natively; gears1_native_dump_dir writes it as BMPs). Not covered yet: the
  render target's own exponent bias and gamma (RB_COLOR_INFO; phase 3, with
  the shaders that write them), resolves into a slice (counted), EDRAM
  aliasing between surfaces of different formats (each is its own texture),
  sample select. Seen only on the title screen so far.
- NATIVE DRAWS, all kinds (2026-11-07, src/native_buffers.cpp): DrawVertices
  (pre hook) uses the bound streams like DrawIndexedVertices; the UP draws
  (BeginVertices / BeginIndexedVertices, post hooks) get vertex fetch
  constant 95 pointed at command buffer memory the caller fills after the
  call returns, so their data is read at the next draw or the frame's end,
  hashed and uploaded as its own buffer. Post hooks now get r1 and r3-r10
  (Gears1HookEntry). Title screen: ~115 draws a frame, 99 indexed, ~95 UP,
  none with a capture problem. Textures: stacked (2D array) and cube
  textures are decoded slice by slice; 3D textures still are not. The texture
  upload had a validation error (copies from the transfer buffer at offsets
  not a multiple of the block size); offsets are now 16-byte aligned.
- RUNS (2026-11-07): SDL_AUDIO_DRIVER=dummy breaks the game: the runtime's
  SDL has no dummy driver, audio init fails and guest thread F8000078 then
  faults in a loop (read of 0x3C), about 100 MB of log a minute. Use
  --audio_mute=true. Killing a run with SIGTERM floods the same way while
  shutting down; the log file sink rotates 20 x 5 MB and loses the boot, so
  log to /dev/stdout through a filter. --fullscreen=false keeps the window
  off the monitor under the mouse (Hyprland).

