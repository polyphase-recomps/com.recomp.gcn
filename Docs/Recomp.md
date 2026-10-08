# Recomp mode: GameCube games recompiled from your disc

Besides the decomp build (a game package's decompilation compiled to portable C), a game
package with a `Recomp/` folder can run **recompiled from the machine code on your own disc**,
the way com.recomp.n64 and com.recomp.ps1 do it. Windows x64 only (consoles keep the decomp
builds). Two flavours:

| Build mode | What the build holds | When the game's code is recompiled |
|---|---|---|
| **Recomp** (AOT) | the game recompiled to C, compiled into `Lib/Windows/<name>_recomp.lib` | when you build (needs your disc and a compiler) |
| **Recomp Live** | the runtime and the game's symbols only (no game code) | when the game starts, from the disc it runs from (~0.3 s, sljit; no compiler) |

## How it works

| Piece | Where | What |
|---|---|---|
| GcnRecomp | `Runtime/recomp/tool` | the recompiler (C++): the DOL's Gekko code, every instruction (paired singles too) -> one C function per guest function. `GCNR_*` options in `Runtime/recomp/include/gcn_recomp.h` |
| Live generator | `Runtime/recomp/live` | the same functions as native code at run time (sljit, `ThirdParty/sljit`): common instructions inline, the rest through `gcnl_exec`, which has the C generator's semantics one instruction at a time |
| Symbols | `<package>/Recomp/syms.txt` | function names and sizes (dtk `symbols.txt`), switch tables, the small-data bases, the static constructors, the HLE list, the decomp's names of the game's globals (`data`). Names only: no game code |
| HLE module | `Runtime/tools/recomp/gcn_hle_build.py` | the runtime's SDK replacement (`Runtime/guest`: threads, DVD, VI, PAD, CARD, AI/ARAM/DSP, THP) and the package's mods, compiled alone through the decomp build's pipeline into a wasm2c module (`<name>recomp`), its data above the game's 24 MB |
| Glue | `Runtime/tools/recomp/gen_hle_glue.py` | `hle_<name>` wrappers (PowerPC registers -> the module's functions) and the module's calls back into the game (`main`, `sprintf`, what mods call, ...) |
| Runtime | `Runtime/recomp/recomp_gcn.c` | function lookup, callbacks / thread entries into recompiled code, hardware registers, loop checks; `live/recomp_live.cpp` fills its tables in Live builds |

Everything else, the game itself, GX, the C library and MusyX, is the disc's own code. The HLE
module's `gcn_game_entry` loads the DOL from the disc and boots like the decomp build, so
GcnPlayer, GcnGuestHost, the Lua `Gcn` table and com.recomp.mod.base run it unchanged.

Which functions the runtime supplies (the `hle` lines): exactly those the decomp build takes from
the runtime, from its link map (`gcn_syms.py --map`): defined in `runtime_*` objects and in no
decomp or game object.

## Building

Editor: Packaging > Target Options > GCN Recomp, **Build mode Recomp** or **Recomp Live**, then
Setup Dependencies (or Tools > Recomp > GameCube > Pre Process Rom, which has the same switch).
Auto builds each game as it was last built, else from its decomp. By hand:

```
powershell -File Runtime\tools\recomp\build_recomp.ps1 -Package <Packages\com.recomp.game> [-Disc <your disc>] [-Decomp <decomp>] [-DebugCrt] [-Live]
```

It:
1. unpacks your disc into the **project's** `Assets/Recomp/<name>/Disc` (what the game reads at
   run time in the editor; added to the project's `.gitignore`) and checks `main.dol` against
   `Recomp/game.json`; copies `game.json` to `Assets/Recomp/<name>/game.json` (the launcher's
   disc check). Live builds don't need a disc;
2. AOT: builds GcnRecomp (once, `Runtime/build/gcnrecomp`) and recompiles the DOL;
3. builds the HLE module (with the package's mods) and the glue (the SDK replacement compiles
   against the decomp's headers: the decomp checkout is still needed to build, not to run);
   Live: builds the symbols in (`gcn_live_syms.py`);
4. compiles everything with clang into `Lib/Windows/<name>_recomp.lib` and writes
   `Source/Guest/<name>_recomp/` (registers it with GcnPlayer, `mode.txt` = `recomp` /
   `recomp-live`). Both are git-ignored (an AOT library is your disc's code).

The recomp and decomp builds of a game replace each other (`Source/Guest/<name>` or
`<name>_recomp`). Both use the same saves; the player prefers the disc chosen in a launcher, then
the one in `Assets/Recomp/<name>/Disc`, then the package's.

**Packaging**: the unpacked disc goes into the package (raw assets) unless **Package the
unpacked disc** is off (Target Options). With Recomp Live and that option off, the packaged game
holds no game code and no game data: the player points it at their own disc in a launcher scene.

## Launcher

Every game of the addon is a com.recomp.mod.base launcher (`Source/GcnLauncher.cpp`): a scene
made with Tools > Recomp > Mods > Launcher (RecompLauncher node, `@launcher:` buttons,
`Recomp.SetRomLocation` / `StartGame` in Lua) lets the player pick their disc and start the game.
A disc (an `.iso` / `.gcm` / `.nkit.iso` image or an unpacked folder) is checked by its header
(GameCube magic, game ID, revision) and, for recomp builds, the SHA-1 of its `main.dol`; a wrong
one is refused with what it is ("GSAP01 rev 1, not GSAE01", "another main.dol", "a compressed
image: convert it to .iso"). The choice is kept in `Saves/<package>.disc.txt`; Play restarts the
game in every GcnPlayer of that package.

## Mods

`Native/mods/*.c` run in recomp builds too, compiled into the HLE module; see
[Modding.md](Modding.md#mods-in-recomp-builds).

## Testing

| Tool | |
|---|---|
| `Runtime/recomp/tool/test/compare_objdump.py` | the decoder against `powerpc-eabi-objdump -M gekko,raw` over a whole DOL (SFA: 717,101 instructions, 0 differences) |
| `Runtime/recomp/host/CMakeLists.txt` (`GCN_RECOMP_RUNNER=ON`, `GCN_RECOMP_LIVE=ON`) | `<name>recomp_runner.exe`: the decomp runner's command line on the recompiled game (AOT or Live) |
| `--fixed-clock` (both runners) | guest time from the retrace count: builds frame-pace the same, so their `--dump` frames compare |
| `GCNR_FMA=OFF` | multiply-add not fused, like the decomp build (`-ffp-contract=off`) and Live builds |
| `GCNL_NO_INLINE=1` | Live: every non-branch instruction through `gcnl_exec` (tells a generator bug from the rest) |
| `GCN_LIVE_CHECKS=ON` | Live: sljit's argument checks and assertions |
| `GCNR_TRACE=N` | logs the first N calls into the HLE |
| `GCNR_WATCH=ON` + `GCNR_WATCH=<hex>` | store watchpoint with a native backtrace |
| `GcnRecomp --trace A,B --check-sp-all` | log arguments on entry; check every call gives r1 back |
| `tool/test/call_test.c` | calls one recompiled function on given arguments (math, ...) |

Star Fox Adventures, recomp (FMA off) against the decomp runner, both `--fixed-clock`, 600
frames: the same primitive / vertex / display-list counts at every checkpoint, 0.1-1.5% of pixels
different (triangle edges, lighting: the original's float math against the decomp's C), about the
same speed. Live against AOT (FMA off): **identical** frames and counts over 4000 frames of menus
and 5000 frames into a new game (name entry, the intro, the CloudRunner storm cutscene); 97% of
the instructions inline; recompiling takes ~0.3 s.

## Things the recompiler handles that are worth knowing

- **Static constructors**: MWCC fills some tables at startup (`.ctors`); they run before `main`.
- **Hand-written assembly** that calls subroutines inside itself and leaves through another
  routine's epilogue: such functions run in "LR mode" (LR-driven returns inside the body).
- **Hardware registers through pointers** (GX's `__cpReg` & co): every access the recompiler
  couldn't prove checks for the 0xCC page at run time (Live: everything from 0xC0000000 goes
  through the helpers).
- **Busy-wait loops** (no stores, no calls, nothing advancing) let pending interrupts and
  retraces happen after 2048 rounds.
- **Hand-written code the recompiler cannot express** (a thread switch that swaps the stack
  pointer and return address): a package's `Recomp/names.txt` line `native <name>` hands the
  function to the recomp runtime's own implementation (`Runtime/recomp/recomp_native.c`), which
  works on the guest registers directly. The GS engine's cooperative threads (Pokemon Colosseum,
  Pokemon XD: `threadExecute`, `_threadSwitch`) run on a host coroutine each that way.
- **REL modules without symbols** (Live): a module the decomp does not describe is still
  recompiled when it links; its functions start at its prolog / epilog / unresolved, at its
  relocations into its own code and at its `bl` targets.
- **Runtime stand-ins**: the runtime's `FALLBACK` definitions (e.g. `THPAudioDecode`, MSL's
  `__sys_alloc`) are placeholders for library code games link themselves; the game's own code
  wins in recomp builds as the linker makes it win in decomp builds.
- **Idle threads**: a priority-31 thread that never waits (OSSetIdleFunction's) lets time pass
  where it re-enables interrupts: on the console it only runs while everyone else waits.
- **Disc images**: `.iso`, `.gcm`, `.nkit.iso` and `.ciso` (read directly and unpacked).
