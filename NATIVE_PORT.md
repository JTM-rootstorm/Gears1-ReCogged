# Native port roadmap

Goal: ReXGlue becomes a scaffold, not a dependency. The recompiled CPU code
stays (it is the port); everything the game does through the Xbox 360 system
layer moves to native implementations at UE3's own platform seams, the same
seams Epic replaced for the 2007 PC port. Written 2026-10-11.

Related: NATIVE_RENDERER.md (renderer plan, D3D/RHI/pass maps), PROBES.md
(hook generator and probes), FINDINGS.md (status).

## What stays (the floor)

- The recompiled code (ReXGlue codegen output): guest 32-bit big-endian
  address space, `PPCContext` registers. Every guest pointer is a guest
  address, so guest memory and the PPC runtime headers stay. Codegen itself is
  only a build tool.
- Thread creation, synchronisation primitives and guest memory allocation:
  can be shrunk (UE3 thread/lock/event classes on host primitives) but guest
  allocations must stay in guest memory.
- Removing the floor = full decompilation to native C++. Decision deferred
  until the seams below are native (the user will decide then).

## Surface to replace

226 kernel/XAM imports (generated/default/gears1_recomp_init.cpp import
table), grouped:

| Group | Imports | Examples |
|---|---|---|
| Threads & sync | ~57 | ExCreateThread, Ke*, NtWait*, Rtl*CriticalSection |
| Users, profile, UI, Live | ~39 | XamUser*, XamShow*, XamSession*, XamNotify* |
| Network | ~33 | NetDll_* |
| Video | 20 | Vd* (ring buffer, swap, display mode) |
| Files & storage | 19 | Nt*File, XamContent*, ObCreateSymbolicLink |
| CRT/strings/debug | 16 | Rtl*String, DbgPrint |
| Memory | 13 | NtAllocateVirtualMemory, MmAllocatePhysicalMemoryEx |
| Audio / XMA | 7 | XAudioRegisterRenderDriverClient, XMACreateContext |
| Input | 4 | XamInputGetState, XamInputSetState, XamInputGetCapabilities |
| Other | ~18 | XGetLanguage, XMsg*, Interlocked*SList |

Plus statically linked XDK libraries that run on the emulated hardware:
D3D (Xenos), XAudio/XMA, Bink, the shader compiler/state machine.

## Seams (UE3 platform layer)

Found as package/class strings in .rdata (NATIVE_RENDERER.md): `XeD3D`
(RHI), `XeDrv` / `UXenonClient` (viewport + input), `XeAudio` /
`UXeAudioDevice`, `XeWinSock`, `OnlineSubsystemLive`, plus the file manager
and save system.

| # | Subsystem | Today (ReXGlue) | Native replacement point | Effort | Payoff |
|---|---|---|---|---|---|
| 1 | Input | XamInputGetState + HID | UXenonClient input path -> SDL; mouse/keyboard into UE3 input axes | small | aim without controller accel; first seam proven |
| 2 | Renderer | Xenos plugin + Vd* | RHI/D3D level (NATIVE_RENDERER.md) | large | no EDRAM/emulation bugs |
| 3 | Files/saves | Nt*File, XamContent, VFS/STFS | UE3 file manager + save system -> host files (SDL storage / pref path) | medium | most of the FS/XAM surface gone |
| 3b | Online | NetDll, Live XAM | null OnlineSubsystem / net driver | small | dead weight gone |
| 4 | Audio | XAudio driver + XMA emulation | UXeAudioDevice -> native mixer on SDL audio streams, FFmpeg XMA | medium-large | APU/XMA emulation gone |
| 5 | Movies | Bink on emulated GPU | native Bink decode (FFmpeg) into the native renderer | medium | follows #2 |
| 6 | Profile/UI | XamShow*, users, achievements | small shims | small | cleanup |
| 7 | Threads/sync/memory | ~70 imports | keep; optionally UE3 FRunnableThread/FCriticalSection/FEvent -> host | large | low |

Each seam is done when its imports are no longer called in normal play
(verify with call counts) and the replacement lives in this project, not in
the SDK.

## Platform: SDL3

Decided (2026-10-14): every native seam is built on SDL3 for portability
(Linux first, macOS/Metal possible later): SDL_GPU for the renderer, SDL
audio streams for sound, SDL gamepad/keyboard/mouse for input, SDL storage /
pref paths for saves and settings, SDL threads where host threads are needed.
No Microsoft or Xbox libraries are reimplemented for their own sake: each
seam replaces the game's engine layer (UE3's platform classes) directly.
While ReXGlue is underneath, the seams use the SDL3 it exports (3.5.0, all
SDL_* symbols incl. SDL_GPU); never link a second SDL.

## Mechanisms

- Recompiled functions: strong overrides from tools/gen_hooks.py
  (hook_table.py), one per symbol (PROBES.md).
- Kernel/XAM imports: a definition in our executable interposes the one in
  librexruntime.so (see Status), so seams switch one import at a time
  without SDK changes.
- Every native replacement gets a cvar to fall back to the ReXGlue path while
  it matures, so regressions can be A/B tested.

## Status

- Import override mechanism verified (2026-10-11): the executable's import
  table references `__imp__*` symbols left undefined in the executable and
  defined by librexruntime.so; defining one in our sources links ours
  (nm: T in the executable). The runtime's version stays reachable through
  dlsym(RTLD_NEXT). SDL3 is statically linked into librexruntime.so and its
  3317 SDL_* symbols are exported, so seams use the runtime's SDL (one event
  pump, one window) through the SDK's SDL3 headers.
- Seam 1 stage 1 working (2026-10-11): src/native_input.cpp replaces
  XamInputGetState/GetCapabilities/SetState with SDL gamepads + keyboard/
  mouse-button mapping (gears1_native_input, gears1_native_input_keyboard).
- Seam 1 stage 2 working (2026-10-11): src/mouse_look.cpp, a post hook on
  APlayerController::Tick 823CB580 (after PlayerTick ran UpdateRotation, only
  for TickType 2 = LEVELTICK_All) adds SDL relative mouse motion straight to
  AActor::Rotation (+0xD8 pitch, +0xDC yaw), so the script's look
  acceleration never sees the mouse. F7 toggles capture. Cvars
  gears1_mouse_look, gears1_mouse_sensitivity (rotation units per count,
  default 12), gears1_mouse_invert_y. Found: engine FName globals via the
  name initializer 8227FC50 (FName_Construct 82364678; each global is stored
  after the NEXT name's setup), PlayerTick = 0x82BFB940, PlayerInput =
  0x82BFBEAC, ProcessEvent = vtable +0xA4, FindFunctionChecked 822AA9B0,
  Rotation offset from ULevel_MoveActor 8239E3F8. The reference scanner
  needed ld/std and base-register tracking to see FName loads
  (scratchpad xrefs2.py).

## Order

1. Input (in progress).
2. Native renderer (phases in NATIVE_RENDERER.md).
3. Files/saves, online stub.
4. Audio, movies.
5. Profile/UI shims; thread/sync last or never.
6. Decision point: full decompilation or keep the floor.
