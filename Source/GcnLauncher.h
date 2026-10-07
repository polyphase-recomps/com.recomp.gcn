/**
 * @file GcnLauncher.h
 * @brief The GameCube games in this addon as com.recomp.mod.base launchers (ModBaseLauncher.h):
 *        a launcher scene (RecompLauncher node, @launcher buttons, Recomp.* Lua) lets the
 *        player point a game at their own disc image and start it.
 *
 * One launcher per game module in the addon (decomp, recomp or live builds alike), registered
 * when the addon loads. A disc is checked by its header (GameCube magic, game ID and revision)
 * and, for builds made from a disc's executable, by the SHA-1 of its main.dol - the recompiled
 * code and the symbols describe one revision. What a game expects comes from
 *   <project>/Assets/Recomp/<name>/game.json   written by build_recomp.ps1 (recomp and live)
 *   Packages/<package>/Assets/game.json        "disc_id" / "disc_revision" / "dol_sha1", if set
 * The chosen disc is remembered in Saves/<package>.disc.txt, and GcnPlayer plays it (after its
 * own Disc Image property, before the discs unpacked into the project or the package).
 * StartGame (Play) restarts the game in every GcnPlayer of the package; a game scene loaded
 * after it starts its own.
 */
#pragma once

#include <string>

namespace GcnLauncher
{
// Registers one launcher per game module of the addon (OnLoad) / removes them (OnUnload).
void RegisterAll();
void UnregisterAll();

// The disc the player chose for a game package ("" = none): an image file or an unpacked disc.
std::string ChosenDisc(const std::string& package);

// Checks a disc (.iso / .gcm / .nkit.iso image, or a folder gcn_disc.py unpacked) for a game
// package. `resolved` gets the absolute path to play. False, with message saying why, when it
// won't do.
bool CheckDisc(const std::string& package, const std::string& path, std::string& message, std::string& resolved);
}
