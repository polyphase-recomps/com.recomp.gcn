/**
 * @file GcnDependencies.h
 * @brief Builds the GameCube game packages from the editor.
 *
 * A game package (e.g. com.recomp.starfoxadventures) carries only its build config
 * (Native/gcn_game.json), patches and mods. Its Native/build.ps1 (Windows) or build.sh
 * (Linux) compiles the decomp and writes the game as portable C into this addon
 * (Source/Guest/<name>), which the addon then compiles for every platform it is packaged
 * for, and unpacks the user's disc into the game package (Assets/Disc). Before packaging
 * (unless the profile turns it off), from the profile's Target Options and from
 * Tools > Recomp > GameCube > Pre Process Rom, this runs those scripts.
 */
#pragma once

#include <cstdint>

struct EditorUIHooks;
struct PolyphaseBuildContext;

namespace GcnDependencies
{
// build profile setting: "0" turns the setup before packaging off
constexpr const char* kSetupOption = "gcn.setupDependencies";
// build profile setting: the decomp checkout (empty: gcn_game.json's "decomp")
constexpr const char* kDecompOption = "gcn.decompDir";
// build profile setting: how game packages are built
//   "auto"    (default) what the game was last built as (Source/Guest/<name>_recomp/mode.txt),
//             else the decomp when the package has one, else recomp
//   "decomp"  Native/build.ps1: the decomp compiled to portable C (every platform)
//   "recomp"  Runtime/tools/recomp/build_recomp.ps1: the disc's code recompiled (Windows x64),
//             for packages with a Recomp/ folder
//   "live"    build_recomp.ps1 -Live: no game code in the build, recompiled from the disc when
//             the game starts (Windows x64)
constexpr const char* kModeOption = "gcn.buildMode";
// build profile setting: "0" leaves the disc unpacked into the project (Assets/Recomp/<game>/Disc)
// out of the package: the packaged game asks the player for their disc (launcher scene)
constexpr const char* kPackageDiscOption = "gcn.packageDisc";

void SetBuildMode(const char* mode);
void SetPackageDisc(bool package);

bool SetupAll(const char* decompDir);
void SetupAllAsync(const char* decompDir);
void Tick();
// Warns about game packages that are not built into the addon yet.
void CheckReady();
void DrawTargetOptions(const PolyphaseBuildContext* ctx);
// Tools > Recomp > GameCube > Pre Process Rom: pick the disc image and the decomp, translate, unpack.
void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId);
void OpenPreprocessWindow();
// Stops a running setup (Windows: the build and everything it started).
void Cancel();
}
