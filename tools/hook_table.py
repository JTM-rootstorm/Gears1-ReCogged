"""Probe hook table for tools/gen_hooks.py (PROBES.md).

Each entry: (address, name, kind, extra). Names match the Ghidra project and
are what gears1_probe_skip and the trace use. Kinds (Gears1ProbeKind):
  pass   scene renderer pass / sub-pass: traced ENTER/LEAVE, skippable
  scope  tracked but not traced (DrawMesh etc.), skippable
  policy drawing-policy DrawShared: tags the following draws, skippable
         (skipping a policy skips the meshes it draws, not DrawShared)
  draw   D3D draw: traced with a state snapshot
  event  other call traced with its raw arguments
  frame  frame boundary (after the original returns)
  hook   no probe, only the pre/post hooks in extra
extra keys:
  pre    list of C++ functions `void f(PPCContext&, uint8_t*)` called before
         the original (and before the probe)
  post   list of C++ functions
         `void f(const Gears1HookEntry&, PPCContext&, uint8_t*)` called after
         the original, with the argument registers it was entered with
"""

PROBES = [
    # Frame boundary: D3D Swap (one call per presented frame; also ends the
    # gears1_d3d_observe line)
    (0x8223E860, 'Swap', 'frame', {}),

    # Scene renderer (NATIVE_RENDERER.md "Scene renderer map")
    (0x825550A8, 'FDrawSceneCommand_Execute', 'pass', {}),
    (0x82553258, 'FSceneRenderer_Render', 'pass', {}),
    (0x82553EA0, 'RenderPrePass', 'pass', {}),
    (0x82491DD8, 'RenderOcclusionQueries', 'pass', {}),
    (0x82554588, 'RenderEmissive', 'pass', {}),
    (0x825C2138, 'RenderLights', 'pass', {}),
    (0x825C28B0, 'RenderModulatedShadows', 'pass', {}),
    (0x825C0468, 'RenderShadowVolumes', 'pass', {}),
    (0x825C0A68, 'RenderModulatedShadowVolumes', 'pass', {}),
    (0x825BD9E8, 'RenderProjectedShadows', 'pass', {}),
    (0x825BBC40, 'ShadowRenderDepth', 'pass', {}),
    (0x825BBFD8, 'ShadowRenderProjection', 'pass', {}),
    (0x825E2680, 'RenderLightFunction', 'pass', {}),
    (0x825D94A0, 'RenderFog', 'pass', {}),
    (0x825D7438, 'RenderDistortion', 'pass', {}),
    (0x825D6E08, 'DrawDistortionApplyScreen', 'pass', {}),
    (0x825D4868, 'RenderTranslucency', 'pass', {}),
    (0x825D9F78, 'RenderGammaCorrection', 'pass', {}),
    (0x825205F8, 'PostDOFAndBloom', 'pass', {}),
    (0x82544148, 'PostUber', 'pass', {}),
    (0x82521FF8, 'PostDOF', 'pass', {}),
    (0x825307D0, 'PostMotionBlur', 'pass', {}),
    (0x82511180, 'PostBloom', 'pass', {}),
    (0x8254CFA0, 'FBatchedElements_Draw', 'pass', {}),
    (0x825A90D0, 'RHICopyToResolveTarget', 'pass', {}),
    (0x825A9818, 'RHICopyFromResolveTarget', 'pass', {}),

    # Mesh drawing
    (0x825C3F88, 'DrawMesh', 'scope', {}),
    (0x825D1268, 'DrawEmissiveMesh', 'scope', {}),
    (0x825D3F70, 'DrawTranslucentMesh', 'scope', {}),

    # Drawing policies (DrawShared)
    (0x82519318, 'LightDirectional_NoStaticShadowing', 'policy', {}),
    (0x82519750, 'LightDirectional_ShadowTexture', 'policy', {}),
    (0x82519B88, 'LightDirectional_ShadowVertexBuffer', 'policy', {}),
    (0x825339A0, 'LightPoint_NoStaticShadowing', 'policy', {}),
    (0x82533CD8, 'LightPoint_ShadowTexture', 'policy', {}),
    (0x825340E0, 'LightPoint_ShadowVertexBuffer', 'policy', {}),
    (0x8253CC48, 'LightSpot_NoStaticShadowing', 'policy', {}),
    (0x8253D388, 'LightSpot_ShadowTexture', 'policy', {}),
    (0x8253DB28, 'LightSpot_ShadowVertexBuffer', 'policy', {}),
    (0x825CE600, 'DepthOnly', 'policy', {}),
    (0x825CE198, 'HitProxy', 'policy', {}),
    (0x825CF8D0, 'Velocity', 'policy', {}),
    (0x825D8408, 'DistortionMesh', 'policy', {}),
    (0x825BB0A0, 'ShadowDepth', 'policy', {}),
    (0x825D4A58, 'TranslucentBlend', 'policy', {}),
    (0x825D4B30, 'TranslucentAdditive', 'policy', {}),
    (0x825D4C80, 'TranslucentModulate', 'policy', {}),

    # D3D draws (r3 = device)
    (0x8222DE50, 'DrawIndexedVertices', 'draw', {}),
    (0x8222DA48, 'DrawVertices', 'draw', {}),
    (0x8222CFF8, 'BeginVertices', 'draw', {}),
    (0x8222D4F8, 'BeginIndexedVertices', 'draw', {}),

    # D3D events
    (0x82235528, 'Resolve', 'event', {}),
    (0x8222CC48, 'Clear', 'event', {}),
    (0x8222B068, 'SetRenderTarget', 'event', {}),
    (0x8222B398, 'SetDepthStencilSurface', 'event', {}),
    (0x82229460, 'SetDepthBias', 'event', {'pre': ['Gears1ProbeDepthBiasPre']}),
    (0x82229398, 'SetSlopeScaleDepthBias', 'event', {'pre': ['Gears1ProbeDepthBiasPre']}),

    # APlayerController::Tick: native mouse look (src/mouse_look.cpp)
    (0x823CB580, 'PlayerControllerTick', 'hook', {'post': ['Gears1MouseLookAfterTick']}),

    # Shader binds (shader dump + hash cache)
    (0x82222B98, 'SetVertexShader', 'hook', {'pre': ['Gears1OnSetVertexShader']}),
    (0x82222808, 'SetPixelShader', 'hook', {'pre': ['Gears1OnSetPixelShader']}),
]
