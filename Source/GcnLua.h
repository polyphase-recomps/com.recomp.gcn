/**
 * @file GcnLua.h
 * @brief Lua table `Gcn`: scripts read and write the running GameCube game and talk to its
 *        mods (see GcnLua.cpp for the functions).
 */
#pragma once

struct lua_State;
struct PolyphaseEngineAPI;

namespace GcnLua
{
void Register(lua_State* L, PolyphaseEngineAPI* api);
}
