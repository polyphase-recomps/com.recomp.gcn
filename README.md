# com.recomp.gcn: GameCube games, natively, in Polyphase

A runtime that builds GameCube games from their **decompilations** and runs them inside
Polyphase (the `GcnPlayer` node), scriptable from Lua (`Gcn`) and moddable in C. Each game
lives in its own small package (for example
[com.recomp.starfoxadventures](../com.recomp.starfoxadventures)) that holds only its build
config, patches, mods and `game.json`.

**No game is included.** Every user builds the game from the decomp and their own disc
image. Nothing this package or a game package generates from a disc (`Source/Guest/`,
`Native/build/`) may be published: the `.gitignore` files keep it out of git.

## How it works

```
decomp C ──clang (big-endian ARM front end: PowerPC EABI layout)──▶ LLVM IR
        ──gcn_ir.py: every load/store byte-swapped, varargs, K&R calls, register leaks──▶
        ──wasm32 compile + wasm-ld──▶ game.wasm ──gcn_place.py: globals at their DOL addresses──▶
        ──wasm2c──▶ portable C (Source/Guest/<game>) ──▶ compiled into this addon
```

* **Big-endian semantics on any host.** The game sees memory exactly as on the console
  (pointers are 32-bit, data big-endian), so the decomp needs no endian fixes.
* **Original memory layout.** `gcn_place.py` moves every decomp global it can identify
  (by name from the decomp's dtk `symbols.txt`, else by contents within its unit) to its
  address in the original DOL, and the guest loads the DOL's data sections from the disc.
  Matching decomps rely on that layout (structs laid over neighbouring globals, strings
  reached by offset); it also means RAM maps and cheat-code addresses of the original game
  apply to the port.
* **SDK in C.** The Dolphin SDK is replaced by C (`Runtime/guest/sdk`: OS, threads, DVD,
  VI, PAD, memory card, audio front end, THP movie decoder); the decomp's own GX library
  is compiled as part of the game and talks to an emulated GPU (`Source/Gcn/gcn_gpu.c`:
  command processor, display lists built at run time, EFB copies to the display and to
  textures; software rasteriser `gcn_raster.c`: transform and lighting, TEV, texture
  formats). Audio: the game's MusyX DSP mixer is reimplemented in C (`musyx_dsp.c`), with
  AX reverb and DVD audio streaming.
* **Threads** are coroutines (fibers / ucontext) on one game thread; interrupts are
  delivered at scheduling points.

## Rendering and display settings

On Windows and Linux the picture is drawn by the host's GPU (`Source/Gcn/gcn_vk.c`, Vulkan 1.2:
the TEV as one shader, vertex transform on the GPU), else by the software rasteriser. The mod
settings' Display group drives it (`GcnPlayer`, from `com.recomp.mod.base`'s display settings):

| Setting | What |
|---|---|
| Resolution | the frame buffer at 1-4x the console's 640x528 (copies into textures are averaged back down) |
| Anti-aliasing | SMAA 1x on the picture the game copies to the display |
| Upscaler | FSR 1 (EASU): that picture upscaled to its size on screen, from the Resolution above |
| Sharpness | FSR 1 RCAS (low, medium, high), with or without the upscaler |
| Textures | trilinear or anisotropic 2x-16x for the world's textures (mipmapped by the game, or linear-filtered power-of-two ones of 8x8 and up; mipmaps made on the GPU from the full-size texture); nearest-filtered textures and copies of the frame buffer keep the game's sampling |

The post-processing runs as compute passes on the display copy only (`gcn_vk_present`), so
copies into textures (reflections, menus drawn from the frame buffer) are unaffected. Shaders:
`Runtime/tools/gpu/*.vert|frag|comp`, compiled into `Source/Gcn/gcn_vk_spv.h` (and SMAA's lookup
textures into `gcn_vk_post_tex.h`) by `gen_spv.py` (needs the Vulkan SDK's glslc).
Third-party code (MIT, licences alongside): `Runtime/tools/gpu/third_party/fsr1` (AMD FidelityFX
Super Resolution 1) and `third_party/smaa` (SMAA, Jimenez et al.).
Runner: `GCN_GPU=1`, `GCN_RENDER_SCALE=n`, `GCN_SMAA=1`, `GCN_FSR=<w>x<h>`, `GCN_SHARPNESS=n`,
`GCN_TEXTURES=n` (0-5).

## Layout

| Path | What |
|---|---|
| `Source/` | the addon: `GcnPlayer`, `GcnGuestHost` (game thread, input, audio, saves), `GcnLua` (`Gcn` table), `GcnDependencies` (Target Options / pre-build) |
| `Source/Gcn/` | runtime host side in C: wasm2c support, memory model, GPU, rasteriser |
| `Source/Guest/<game>/` | **generated** by the game package's build: the game as C (git-ignored) |
| `Runtime/tools/` | `gcn_build.py` (driver), `gcn_ir.py`, `gcn_place.py`, `gcn_wasm_to_c.py`, disc tools, patch tools |
| `Runtime/guest/` | C compiled with the game: SDK replacement, libc, mod API (`bridge.c`) |
| `Runtime/include/` | headers for game code: `gcn_mod.h` (mods), `gcn_guest.h` (host imports) |
| `Runtime/sdk_include/` | the Dolphin SDK headers the runtime builds against (from SFA-Decomp, CC0), last on every build's include path: recomp packages need no other decomp's headers |
| `Runtime/host/` | standalone test runner (`<game>_runner.exe`): headless, frame dumps, scripted input, watch/RAM debugging |
| `Docs/Modding.md` | mods (C) and scripts (Lua) |

## Building a game

Requirements: Python 3, Visual Studio 2022 with its clang (Windows) or clang (Linux),
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) and [wabt](https://github.com/WebAssembly/wabt)
unpacked in a `Tools/` folder above the project (or `GCN_WASI_SDK` / `GCN_WABT`), the
game's decomp checkout and your disc image. Then either

* Polyphase: **Tools > Recomp > GameCube > Pre Process Rom** (pick your disc image and the
  decomp; it translates the game and unpacks the disc into the game package's `Assets/Disc`),
  then **Reload Native Addons**. Packaging runs the same setup first unless the profile turns
  it off (Packaging > Target Options > GCN Recomp), or
* by hand: `Packages/<game>/Native/build.ps1 -Addon` (Windows) or `sh build.sh --addon` (Linux).

The translated game is portable C, so one build serves every platform the project is
packaged for. Add a `GcnPlayer` node to a scene and set **Game** to the game package id.

**Recomp mode** (Windows x64): a game package with a `Recomp/` folder can instead run
recompiled from the machine code on your own disc (Build mode **Recomp** in the GCN Recomp
Target Options, or `Runtime/tools/recomp/build_recomp.ps1`), ahead of time or, with **Recomp
Live**, when the game starts (no game code in the build). Its disc is unpacked into the
project's `Assets/Recomp/<name>/Disc`; mods work in both. See [Docs/Recomp.md](Docs/Recomp.md).

**Launcher**: every game is a com.recomp.mod.base launcher (`GcnLauncher`): a launcher scene
(Tools > Recomp > Mods > Launcher) lets the player pick their own disc, checks it (game ID,
revision, the executable's SHA-1 for recomp builds) and starts the game.

### Controls (default)

Keyboard: arrows = stick, X = A, Z = B, S = X, A = Y, Q = L, W = R, E = Z, I J K L = C-stick,
1-4 = d-pad, Enter = Start. Gamepads map by position (right bumper = Z, triggers = L / R).

## Debugging a port

`Packages/<game>/Native/run.ps1` builds and runs the standalone runner. Useful options of
`build/runner/<game>_runner.exe`: `--frames N --dump DIR --every N --dump-from F` (PPM frames),
`--script "F:BUTTONS[:DURATION],..."` (pad input by frame, hex `PAD_BUTTON_*` bits),
`--map build/Release/<game>.syms --watch name,0xADDR --watch-every N` (guest variables),
`--stack-at F`, `--dump-ram FILE`, `--saves DIR`. Environment: `GCN_TRACE_PRIMS=N`
(+`GCN_TRACE_VERTS=1`) traces every primitive of display copy N, `GCN_RASTER_THREADS=n`
sets the rasteriser's threads (default: all cores; 1 = draw immediately), `GCN_NO_RASTER=1` skips drawing, `GCN_BREAK_ON_LOG=text`
stops at the first log line containing text, `GCN_TRACE_MMIO=1`, `GCN_TRACE_PIXEL=x,y` (with
`GCN_TRACE_PRIMS`: what last wrote that pixel, with its TEV setup), `GCN_TRACE_COPIES=1`
(every EFB copy to a texture), `GCN_DUMP_COPIES=dir:first:count` (those copies as PPM),
`--wav FILE` (record the audio). `run.ps1 -WatchWrites`
builds a runner with a store watchpoint (`GCN_WATCH_WRITE=lo:hi[:value]`,
`GCN_WATCH_SKIP=n`). Game diagnostics the retail build drops (`logPrintf`) appear as
`game: ...` lines when the game package's patches forward them.

## Platforms

Windows and Linux hosts work today (the editor and packaged games). The runtime's host
interface (`Source/Gcn/gcn_platform.h`) is plain C so that console and handheld hosts
(PS2, PSP, Wii, ...) can implement it; their memory budgets need a smaller RAM image and
a hardware renderer instead of the software rasteriser, which is the planned next step.

## Status

See the game package's README for what works in each game.
