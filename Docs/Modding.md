# Modding GameCube games on com.recomp.gcn

There are two layers, and they work together:

* **Mods (C)**: files in the game package's `Native/mods/`, built with the game. They see
  the decomp's headers and globals, so they can do anything the game's own code can: new
  behaviour, new objects, changed rules. They also *publish* things to scripts.
* **Scripts (Lua)**: Polyphase scripts use the global table `Gcn` to read and write the
  running game, call the mods' requests and react to their events, e.g. to build new UIs
  with Polyphase widgets on top of the game.

## C mods

Create `Native/mods/<name>.c` in the game package. It must define

```c
void mod_<name>_init(void);
```

which runs once before the game's `main()`. Rebuild the game (Target Options >
Setup Dependencies Now, or `Native/build.ps1 -Addon`) and reload the addons.

The API is `Runtime/include/gcn_mod.h`:

| Call | Use |
|---|---|
| `gcn_mod_variable(name, addr, type, count, stride, help)` | publish a variable (or array) scripts read and write by `name` |
| `gcn_mod_request(name, fn, help)` | a function scripts call with `Gcn.Request(name, ...)`; `fn(args, nargs)` returns the result |
| `gcn_mod_on_frame(fn)` | run `fn` at the start of every game frame (in the game's main thread, before it reads the controllers) |
| `gcn_mod_emit(name, args, nargs)` | send an event to scripts (`Gcn.Events()`) |
| `gcn_mod_pad_buttons()`, `gcn_mod_set_pad_buttons(b)` | controller 1 as the game sees it this frame (inside frame hooks) |
| `gcn_mod_pad_stick(&x, &y)`, `gcn_mod_set_pad_stick(x, y)` | controller 1's main stick this frame, -128..127 (up and right positive) |
| `gcn_mod_overlay(line, text)` | on-screen text, line 0..15 up from the bottom left, until changed (`""` clears); digits, letters and `.,:-+=_/%()!?` |
| `gcn_mod_log(fmt, ...)` | a line in the log |

Frame hooks run in the order of the mods' file names, each seeing the input changes of the
ones before it.

Requests and frame hooks run where the game's own frame begins, so calling game functions
from them is safe. Keep them short: they run inside the game's frame time.

Example (Star Fox Adventures, `Native/mods/example_tools.c`):

```c
#include "dolphin/types.h"
#include "gcn_mod.h"

extern f32 timeDelta;       /* game globals by their decomp names */
static int sSpeedPercent = 100;

static int speed(const int *args, int nargs)
{
    if (nargs > 0 && args[0] >= 10 && args[0] <= 400) sSpeedPercent = args[0];
    return sSpeedPercent;
}

static void frame(void)
{
    timeDelta = timeDelta * (f32)sSpeedPercent / 100.0f;
}

void mod_example_tools_init(void)
{
    gcn_mod_variable("speed_percent", &sSpeedPercent, GCN_VAR_S32, 1, 0, "game speed");
    gcn_mod_request("speed", speed, "speed(percent): 10..400");
    gcn_mod_on_frame(frame);
}
```

Bigger changes to the game itself belong in patches: edit a copy of the decomp file and
make a patch with `Runtime/tools/gcn_mkpatch.py` into the game package's `Native/patches/`
(applied to copies at build time; the decomp checkout stays untouched). Wrap port-only
code in `#ifdef PORT`.

## Lua: the `Gcn` table

| Function | Returns |
|---|---|
| `Gcn.IsRunning()`, `Gcn.Title()`, `Gcn.Frame()` | game state, title, frames shown |
| `Gcn.Read(target [, type [, index]])` | a value, or `nil` |
| `Gcn.Write(target, value [, type [, index]])` | `true` if written |
| `Gcn.Address(name)` | address and size of a global |
| `Gcn.Request(name, ...)` | request id (integer arguments), or `nil` |
| `Gcn.Result(id)` | the request's result once it ran (next frame), else `nil` |
| `Gcn.Events()` | `{ {name=, args={...}}, ... }` since the last call |
| `Gcn.Variables()`, `Gcn.Requests()` | what the mods published |
| `Gcn.HoldButtons(mask, frames)` | holds controller-1 buttons (A 0x100, B 0x200, X 0x400, Y 0x800, Start 0x1000, Z 0x10, R 0x20, L 0x40, d-pad left/right/down/up 1/2/4/8) |
| `Gcn.SetPaused(bool)`, `Gcn.IsPaused()` | pause the game (its last frame stays on screen) |

`target` is any global of the decomp by its own name (`"gameState"`, `"timeDelta"`), a
variable a mod published (`"speed_percent"`), or an address (`0x803DD804`). Without a
`type` the size of the symbol decides (1: `u8`, 2: `s16`, 4: `s32`); give `"f32"` for
floats, `"u32"`, `"str"` and so on explicitly. Values are big-endian like on the console.

Globals sit at their **original addresses** when the game was built with its decomp's
layout, so addresses from RAM maps, Dolphin memory watches and Action Replay codes work
directly:

```lua
-- an 8-bit write at an address from a RAM map
Gcn.Write(0x803DD804, 0, "u8")
```

Scripts run between two game frames (the game waits), so reads and writes never tear.

Example: `Packages/com.recomp.starfoxadventures/Scripts/SfaTools.lua`.

## Mod settings menu, `Recomp` / `Mods` Lua, resolution scaler

The mod layer every recomp runtime shares is **com.recomp.mod.base** (a dependency of this
package; see its README):

- **Tools > Recomp > Mods > Mod Map Editor**: a Mod Map (asset) lists what players can
  change or watch. **Import...** fills it from the running game, or without running
  anything from the bridge tables in the game package's `Native/` sources.
- **Tools > Recomp > Mods > Generate Mod Settings Scene...**: a gamepad settings menu built
  from the map (tabs per group, Save / Reset / Close, Display page). Generating again
  updates it and keeps your edits.
- At runtime the player's choices are written to the game, kept ("lock" entries) and
  saved (`Saves/<name>.mods`, GameCube memory card).
- Lua `Recomp.*` works on any runtime; `Mods.*` reads and changes the settings.
- The player node places its picture with the shared **resolution scaler**: Fit
  (the console's real shape), Integer, Native, Full Screen, Scale ×N, sharp / smooth, and
  window sizes on Windows.
- **Tools > Recomp > Mods > Live Variables** shows and edits the running game's
  variables: handy for finding cheats.

On GameCube the provider is `Source/GcnProvider.cpp`. A Mod Map entry can use:

- a mod's published variable (`gcn_mod_variable`);
- **any decomp global by name** (source Symbol);
- a raw GameCube address (big-endian, as the console);
- a request.

`GcnPlayer`'s old **Stretch** property still fills the screen; off, the scaler places the
picture. A 640x448 copy is shown 4:3, no longer wider.
