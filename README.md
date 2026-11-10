# Gears1-ReCogged

Gears of War (Xbox 360, 2006) as a native Linux program, through static
recompilation: the game's PowerPC code is recompiled ahead of time to
C++. There is no game code or data in this repository: bring your own
copy of the game, extracted from your own disc.

For now the port is built on
[ReXGlue](https://github.com/rexglue/rexglue-sdk) 0.10, which recompiles
the code and runs it on Xenia's kernel and an emulated Xenos GPU. The
aim is native code at the game's own platform seams (graphics, input,
audio, files, threads), with ReXGlue left as a scaffold
([NATIVE_PORT.md](NATIVE_PORT.md), [NATIVE_RENDERER.md](NATIVE_RENDERER.md)),
and then a move onto [SwARc](https://github.com/JTM-rootstorm/SwARc)'s
Xbox 360 module.

## Status

Plays from the menu through a new game and the intro cutscene into
gameplay, with a controller or keyboard and mouse, on the emulated GPU.
A native renderer that replaces it at the game's Direct3D level is in
progress and does not draw frames on its own yet. Stray black polygons
from the game's MSAA predicated tiling are worked around by a patch
that is on by default. [FINDINGS.md](FINDINGS.md) has the detail.

## What you need

- Gears of War for Xbox 360, the disc release (title 4D5307D5),
  extracted from your own copy so that `default.xex` and the game's
  data are in one folder.
- Linux x86-64 with a Vulkan driver.
- The ReXGlue SDK 0.10 (linux-amd64) and its `rexglue` CLI.
- Clang, CMake, Ninja and glslang (`glslangValidator`).

## Building

`gears1_recomp_manifest.toml` names the game's folder and its
`default.xex` (`game_root`, `file_path`): point them at your copy.
Then generate the code and build:

```sh
rexglue codegen gears1_recomp_manifest.toml
cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH="$REXGLUE_SDK" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset linux-amd64-release
```

The presets name `clang-20`; the compiler arguments use whichever Clang
is installed (22 is used here). Codegen writes `generated/`. It skips
work when nothing changed; pass `--ignore-stamp` to force a full run.

## Running

```sh
LD_LIBRARY_PATH="$REXGLUE_SDK/lib" \
  out/build/linux-amd64-release/gears1_recomp \
  --game_data_root "$GAME" --gpu_plugin xenos
```

Without `--gpu_plugin xenos` the game waits forever for a GPU. Unknown
flags are ignored silently. `--mnk_mode=true` plays with keyboard and
mouse.
