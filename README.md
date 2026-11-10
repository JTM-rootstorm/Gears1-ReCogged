# Gears1-ReCogged

Gears of War (Xbox 360, 2006) as a native Linux program, through static
recompilation: the game's PowerPC code is recompiled ahead of time to
C++. There is no game code or data in this repository: bring your own
copy of the game, extracted from your own disc.

For now the port is built on
[ReXGlue](https://github.com/rexglue/rexglue-sdk), which recompiles the
code and runs it on Xenia's kernel and an emulated Xenos GPU. The aim is
native code at the game's own platform seams (graphics, input, audio,
files, threads), the same seams Epic replaced for the 2007 PC port, with
ReXGlue left as a scaffold ([NATIVE_PORT.md](NATIVE_PORT.md)), and then a
move onto [SwARc](https://jtm-rootstorm.github.io/SwARc.site/)'s Xbox 360
module.

## Status

In game. It plays from the menu through a new game and the intro
cutscene into gameplay, on the emulated GPU, with a controller or
keyboard and mouse (with mouse look).

Fixes to the emulated GPU found here, on the ReXGlue branch below:
- an NVIDIA device loss and random black spikes on skinned characters,
  both from the SPIR-V translator's case fallthrough;
- the red tint and wrong lighting, from a render target ownership
  transfer's gamma conversion and the host's 2x MSAA sample positions;
- decal and blood pool flicker, with depth bias in exact units
  (`VK_EXT_depth_bias_control`; also in Xenia Canary as
  [#1289](https://github.com/xenia-canary/xenia-canary/pull/1289)).

A native renderer, replacing the emulated GPU at the game's own Direct3D
level ([NATIVE_RENDERER.md](NATIVE_RENDERER.md)), opens a window of its
own next to the emulated one. It has the game's textures (every mip
level), its vertex and index buffers (re-read only when the game writes
them), its render targets with clears and resolves, and the game's
shaders translated ahead of time from its packages. It does not draw
frames on its own yet: shaders and state are the next phase.

Known issues: stray black polygons from the game's MSAA predicated
tiling, worked around by a patch that is on by default
(`gears1_black_shading_fix`). [FINDINGS.md](FINDINGS.md) has the detail
and the next steps.

## What you need

- Gears of War for Xbox 360, the disc release (title 4D5307D5), extracted
  from your own copy so that `default.xex` and the game's data are in one
  folder.
- Linux x86-64 with a Vulkan driver.
- ReXGlue built from the `gears1-main` branch of
  [JTM-rootstorm/rexglue-sdk](https://github.com/JTM-rootstorm/rexglue-sdk)
  (0.10 with the GPU and xam fixes above; stock 0.10 brings back the
  device loss and the spikes on NVIDIA), installed to a prefix, with its
  `rexglue` CLI on the path.
- Clang, CMake 3.25 or newer, Ninja and glslang (`glslangValidator`).

## Building

`gears1_recomp_manifest.toml` names the game's folder and its
`default.xex` (`game_root` and `[entrypoint] file_path`): point them at
your copy. Then generate the code and build:

```sh
REXGLUE_SDK="$HOME/rexglue"   # the SDK's install prefix

rexglue codegen gears1_recomp_manifest.toml
cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH="$REXGLUE_SDK" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset linux-amd64-release
```

Codegen writes `generated/` and skips work when nothing has changed
(`--ignore-stamp` forces a full run; `--log_level debug --log_file <new
file>` logs it, and the log appends). The presets name `clang-20`; the
compiler arguments use whichever Clang is installed (22 here).

## Running

```sh
GAME="$HOME/Games/Gears of War"   # the extracted disc

LD_LIBRARY_PATH="$REXGLUE_SDK/lib" \
  out/build/linux-amd64-release/gears1_recomp \
  --game_data_root "$GAME" --gpu_plugin xenos --fullscreen=false
```

- `--gpu_plugin xenos` is also in `config/gears1_recomp.toml`, which the
  build copies next to the program; without it the game waits forever
  for a GPU.
- Unknown flags are ignored silently, so a typo does nothing.
- `--audio_mute=true` silences it. Don't use `SDL_AUDIO_DRIVER=dummy`:
  the runtime's SDL has no dummy driver, and without audio the game's
  sound thread faults over and over.
- Under gdb, let the runtime's real-time signals through (SIG34 to
  SIG64):
  `handle SIG34 SIG35 SIG36 SIG37 SIG38 SIG39 SIG40 SIG41 SIG42 SIG43 SIG44 SIG45 SIG46 SIG47 SIG48 SIG49 SIG50 SIG51 SIG52 SIG53 SIG54 SIG55 SIG56 SIG57 SIG58 SIG59 SIG60 SIG61 SIG62 SIG63 SIG64 nostop noprint`

The port's own switches (`--name=value`):

| Switch | Default | |
|---|---|---|
| `gears1_native_input` | true | pads, keyboard and mouse through SDL directly |
| `gears1_native_input_keyboard` | true | keyboard and mouse onto player 1's pad (WASD, arrows, mouse buttons) |
| `gears1_mouse_look` | true | the camera turns with the mouse; F7 toggles capture |
| `gears1_mouse_sensitivity` | 12.0 | rotation units per mouse count (65536 = 360 degrees) |
| `gears1_mouse_invert_y` | false | invert vertical look |
| `gears1_black_shading_fix` | true | the scene as one full-screen tile, which avoids the tiling bug |
| `gears1_use_tiling_code`, `gears1_tiling_mode` | -1 | override UE3's tiling choices (-1: the game's own) |
| `gears1_native_renderer` | false | open the native renderer's window |
| `gears1_native_gpu_debug` | false | its SDL_GPU device with validation |
| `gears1_native_dump_dir` | | write its front buffer there as BMPs |
| `gears1_shader_dump`, `gears1_shader_dump_dir` | false, `shader_dump` | write every distinct bound shader |
| `gears1_d3d_observe` | false | count Xbox D3D calls, one line a frame |
| `gears1_probe` and `gears1_probe_*` | false | UE3 render pass and draw probes ([PROBES.md](PROBES.md)) |

## Repository

- `src/`: the program: game patches and hooks (`hooks_gen.cpp` is
  generated by `tools/gen_hooks.py` from `tools/hook_table.py`; never edit
  it by hand), native input and mouse look, the native renderer
  (`native_*.cpp`, its shaders in `src/shaders/`), probes and the shader
  dump.
- `gears1_recomp_manifest.toml`: codegen's manifest, with the functions
  and jump tables its analysis misses. `config/gears1_recomp.toml`: the
  runtime's settings (cvars), copied next to the program; flags override
  them.
- `tools/`: `upk.py` (unpacks the game's UE3 packages and extracts their
  shaders), `translate_shaders.py` (translates and validates them
  offline), GFXR capture, replay and bisection scripts, and SPIR-V
  experiments used to find the GPU bugs.
- `third_party/imgui`: Dear ImGui's SDL_GPU backend.
- `upstream_drafts/`: what was sent upstream (Xenia Canary, ReXGlue).
- [FINDINGS.md](FINDINGS.md): what has been learned, status and next
  steps. [NATIVE_PORT.md](NATIVE_PORT.md),
  [NATIVE_RENDERER.md](NATIVE_RENDERER.md) and [PROBES.md](PROBES.md): the
  plans.
