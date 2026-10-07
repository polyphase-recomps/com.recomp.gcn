# Recomp mode: GameCube games recompiled from your disc

Besides the decomp build (a game package's decompilation compiled to portable C), a game
package with a `Recomp/` folder can run **recompiled from the machine code on your own disc**,
the way com.recomp.n64 and com.recomp.ps1 do it. Windows x64 only (consoles keep the decomp
builds).

## How it works

| Piece | Where | What |
|---|---|---|
| GcnRecomp | `Runtime/recomp/tool` | the recompiler (C++): the DOL's Gekko code, every instruction (paired singles too) -> one C function per guest function. `GCNR_*` options in `Runtime/recomp/include/gcn_recomp.h` |
| Symbols | `<package>/Recomp/syms.txt` | function names and sizes (dtk `symbols.txt`), switch tables, the small-data bases, the static constructors, and the HLE list. Names only: no game code |
| HLE module | `Runtime/tools/recomp/gcn_hle_build.py` | the runtime's SDK replacement (`Runtime/guest`: threads, DVD, VI, PAD, CARD, AI/ARAM/DSP, THP) compiled alone through the decomp build's pipeline into a wasm2c module (`<name>recomp`), its data above the game's 24 MB |
| Glue | `Runtime/tools/recomp/gen_hle_glue.py` | `hle_<name>` wrappers (PowerPC registers -> the module's functions) and the module's calls back into the game (`main`, `sprintf`, ...) |
| Runtime | `Runtime/recomp/recomp_gcn.c` | function lookup, callbacks / thread entries into recompiled code, hardware registers, loop checks |

Everything else, the game itself, GX, the C library and MusyX, is the disc's own code. The HLE
module's `gcn_game_entry` loads the DOL from the disc and boots like the decomp build, so
GcnPlayer, GcnGuestHost, the Lua `Gcn` table and com.recomp.mod.base run it unchanged.

Which functions the runtime supplies (the `hle` lines): exactly those the decomp build takes from
the runtime, from its link map (`gcn_syms.py --map`): defined in `runtime_*` objects and in no
decomp or game object.

## Building

Editor: Packaging > Target Options > GCN Recomp, **Build mode Recomp**, then Setup Dependencies
(or Tools > Recomp > GameCube > Pre Process Rom, which has the same switch). Auto builds each game
as it was last built, else from its decomp. By hand:

```
powershell -File Runtime\tools\recomp\build_recomp.ps1 -Package <Packages\com.recomp.game> -Disc <your disc> [-Decomp <decomp>] [-DebugCrt]
```

It:
1. unpacks your disc into the **project's** `Assets/Recomp/<name>/Disc` (what the game reads at
   run time; added to the project's `.gitignore`; the editor registers the files as raw assets so
   packaging takes them along) and checks `main.dol` against `Recomp/game.json`;
2. builds GcnRecomp (once, `Runtime/build/gcnrecomp`) and recompiles the DOL;
3. builds the HLE module and the glue (the SDK replacement compiles against the decomp's
   headers: the decomp checkout is still needed to build, not to run);
4. compiles everything with clang into `Lib/Windows/<name>_recomp.lib` and writes
   `Source/Guest/<name>_recomp/` (registers it with GcnPlayer, `mode.txt`). Both are git-ignored:
   they are your disc's code.

The recomp and decomp builds of a game replace each other (`Source/Guest/<name>` or
`<name>_recomp`). Both use the same saves (`Saves/<name>`); the player prefers the disc in
`Assets/Recomp/<name>/Disc` over the package's.

## Testing

| Tool | |
|---|---|
| `Runtime/recomp/tool/test/compare_objdump.py` | the decoder against `powerpc-eabi-objdump -M gekko,raw` over a whole DOL (SFA: 717,101 instructions, 0 differences) |
| `Runtime/recomp/host/CMakeLists.txt` (`GCN_RECOMP_RUNNER=ON`) | `<name>recomp_runner.exe`: the decomp runner's command line on the recompiled game |
| `--fixed-clock` (both runners) | guest time from the retrace count: the decomp and recomp builds frame-pace the same, so their `--dump` frames compare |
| `GCNR_FMA=OFF` | multiply-add not fused, like the decomp build (`-ffp-contract=off`) |
| `GCNR_TRACE=N` | logs the first N calls into the HLE |
| `GCNR_WATCH=ON` + `GCNR_WATCH=<hex>` | store watchpoint with a native backtrace |
| `GcnRecomp --trace A,B --check-sp-all` | log arguments on entry; check every call gives r1 back |
| `tool/test/call_test.c` | calls one recompiled function on given arguments (math, ...) |

Star Fox Adventures, 600 frames, recomp (FMA off) against the decomp runner, both `--fixed-clock`:
the same primitive / vertex / display-list counts at every checkpoint, 0.1-1.5% of pixels
different (triangle edges, lighting: the original's float math against the decomp's C), about
the same speed.

## Things the recompiler handles that are worth knowing

- **Static constructors**: MWCC fills some tables at startup (`.ctors`); they run before `main`.
- **Hand-written assembly** that calls subroutines inside itself and leaves through another
  routine's epilogue: such functions run in "LR mode" (LR-driven returns inside the body).
- **Hardware registers through pointers** (GX's `__cpReg` & co): every access the recompiler
  couldn't prove checks for the 0xCC page at run time.
- **Busy-wait loops** (no stores, no calls, nothing advancing) let pending interrupts and
  retraces happen after 2048 rounds.

## Not yet

Live mode (recompiling at load, no compiler), mods in recomp mode, a mod.base launcher for it.
