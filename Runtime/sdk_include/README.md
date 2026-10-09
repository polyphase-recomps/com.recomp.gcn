# Dolphin SDK headers

The headers the runtime's SDK replacement (`Runtime/guest/sdk`) compiles against: `dolphin.h`,
`dolphin/` (OS, GX, DVD, CARD, AI, AR, DSP, PAD, VI, THP, ...), `types.h`, `global.h` and the few
C library headers they use (`stdarg.h`, `stddef.h`, `string.h`). They give the console's own
layouts of the SDK's structures (`OSThread`, `DVDFileInfo`, `CARDStat`, ...), which recompiled
games read and write directly.

`gcn_build.py` puts this folder **last** on the include path of every build: a decomp's own
headers (its `include` in `gcn_game.json`) and a package's `sdk_include` come first. Recomp
packages (Pikmin, F-Zero GX, ...) need nothing else: no other decomp's headers.

Copied unchanged from [SFA-Decomp](https://github.com/zcanann/SFA-Decomp) (`include/`, commit
13491b1880), which is released under CC0 1.0 (public domain): see `LICENSE.txt`.
