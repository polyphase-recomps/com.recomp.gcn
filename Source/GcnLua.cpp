/**
 * @file GcnLua.cpp
 * @brief Lua access to the running GameCube game (GcnGuestHost).
 *
 * Global table `Gcn`:
 *   Gcn.IsRunning()                 true while a game runs
 *   Gcn.Title()                     the running game's title, or nil
 *   Gcn.Frame()                     frames the game has shown
 *   Gcn.Read(target [, type [, i]]) a value of game memory, or nil. target: a game global
 *                                   by its decomp name ("gameState"), a variable a mod
 *                                   published, or an address (0x803DD804). type: "u8",
 *                                   "s8", "u16", "s16", "u32", "s32", "f32" or "str"
 *                                   (default: from the symbol's size / the variable's
 *                                   type); i: element index (arrays)
 *   Gcn.Write(target, value [, type [, i]])   writes it (big-endian, like the console)
 *   Gcn.Address(name)               address, size of a global (nil if unknown)
 *   Gcn.Request(name, ...)          runs a mod's request at the start of the next game
 *                                   frame (integer arguments); returns its id or nil
 *   Gcn.Result(id)                  the request's result once it ran, else nil
 *   Gcn.Events()                    { {name=, args={...}}, ... } mods emitted since the
 *                                   last call
 *   Gcn.Variables(), Gcn.Requests() what the game's mods published
 *   Gcn.HoldButtons(mask, frames)   holds controller-1 buttons (PAD_BUTTON_* bits: A
 *                                   0x100, B 0x200, X 0x400, Y 0x800, Start 0x1000, Z 0x10,
 *                                   R 0x20, L 0x40, d-pad 1/2/4/8) for that many frames
 *   Gcn.SetPaused(bool), Gcn.IsPaused()
 *
 * Globals of games built with their decomp's layout (gcn_place.py) sit at their original
 * addresses, so addresses from existing RAM maps and cheat codes work too. Reads and
 * writes happen between two game frames (the game is parked while scripts run).
 *
 * Every Lua call goes through the engine's Lua_* wrappers (PolyphaseEngineAPI): the
 * addon must not use its own copy of the Lua library on the engine's state.
 */

#include "GcnLua.h"

#include "Constants.h"

#if LUA_ENABLED

#include "GcnGuestHost.h"
#include "GcnPlayer.h"
#include "Plugins/PolyphaseEngineAPI.h"

extern "C" {
#include "Gcn/gcnw_backend.h"
#include "Gcn/gcnw_module.h"
}

#include <cstring>
#include <string>
#include <vector>

namespace
{
PolyphaseEngineAPI* sApi = nullptr;

typedef int (*LuaFunction)(lua_State* L);
struct LuaReg
{
    const char* name;
    LuaFunction func;
};

const int kLuaTypeNumber = 3; // LUA_TNUMBER

const char* TypeName(int type)
{
    switch (type)
    {
    case GCNW_VAR_U8: return "u8";
    case GCNW_VAR_S8: return "s8";
    case GCNW_VAR_U16: return "u16";
    case GCNW_VAR_S16: return "s16";
    case GCNW_VAR_U32: return "u32";
    case GCNW_VAR_S32: return "s32";
    case GCNW_VAR_F32: return "f32";
    case GCNW_VAR_STR: return "str";
    default: return "?";
    }
}

int TypeFromName(const char* name, int fallback)
{
    static const char* kNames[] = {"u8", "s8", "u16", "s16", "u32", "s32", "f32", "str"};
    for (int i = 0; i < 8; ++i)
    {
        if (strcmp(name, kNames[i]) == 0) return i + 1;
    }
    return fallback;
}

int TypeBytes(int type)
{
    switch (type)
    {
    case GCNW_VAR_U8: case GCNW_VAR_S8: case GCNW_VAR_STR: return 1;
    case GCNW_VAR_U16: case GCNW_VAR_S16: return 2;
    default: return 4;
    }
}

bool HasArg(lua_State* L, int arg)
{
    return sApi->Lua_gettop(L) >= arg && !sApi->Lua_isnil(L, arg);
}

// target (name or number) -> address and default type; false if unknown
bool ResolveTarget(lua_State* L, int arg, uint32_t& addr, int& type, int& stride, int& count)
{
    uint32_t size = 0;
    std::string name;

    if (sApi->Lua_type(L, arg) == kLuaTypeNumber)
    {
        addr = (uint32_t)(int64_t)sApi->Lua_tonumber(L, arg);
        type = GCNW_VAR_S32;
        stride = 4;
        count = 1;
        return true;
    }
    name = sApi->LuaL_checkstring(L, arg);
    return GcnGuestHost::Resolve(name, addr, size, type, count, stride);
}

bool Running()
{
    return GcnGuestHost::GetState() == GcnGuestHost::State::Running;
}

int IsRunning(lua_State* L)
{
    sApi->Lua_pushboolean(L, Running());
    return 1;
}

int Title(lua_State* L)
{
    const GcnwModule* module = GcnGuestHost::GetModule();
    if (module != nullptr)
        sApi->Lua_pushstring(L, module->title);
    else
        sApi->Lua_pushnil(L);
    return 1;
}

int Frame(lua_State* L)
{
    sApi->Lua_pushinteger(L, (long long)GcnGuestHost::FrameCount());
    return 1;
}

int Read(lua_State* L)
{
    uint32_t addr = 0;
    int type = 0, stride = 0, count = 0;

    if (!Running() || !ResolveTarget(L, 1, addr, type, stride, count))
    {
        sApi->Lua_pushnil(L);
        return 1;
    }
    if (HasArg(L, 2))
    {
        const int explicitType = TypeFromName(sApi->LuaL_checkstring(L, 2), type);
        if (explicitType != type)
        {
            type = explicitType;
            stride = TypeBytes(type);
        }
    }
    const int index = HasArg(L, 3) ? (int)sApi->LuaL_checkinteger(L, 3) : 0;
    addr += (uint32_t)(index * stride);
    if (type == GCNW_VAR_STR)
    {
        sApi->Lua_pushstring(L, GcnGuestHost::ReadString(addr, stride > 1 ? (uint32_t)stride : 256u).c_str());
        return 1;
    }
    const uint32_t raw = GcnGuestHost::Read(addr, TypeBytes(type));
    switch (type)
    {
    case GCNW_VAR_S8: sApi->Lua_pushinteger(L, (int8_t)raw); break;
    case GCNW_VAR_S16: sApi->Lua_pushinteger(L, (int16_t)raw); break;
    case GCNW_VAR_S32: sApi->Lua_pushinteger(L, (int32_t)raw); break;
    case GCNW_VAR_F32:
    {
        float f;
        memcpy(&f, &raw, 4);
        sApi->Lua_pushnumber(L, f);
        break;
    }
    default: sApi->Lua_pushinteger(L, (long long)raw); break;
    }
    return 1;
}

int Write(lua_State* L)
{
    uint32_t addr = 0;
    int type = 0, stride = 0, count = 0;

    if (!Running() || !ResolveTarget(L, 1, addr, type, stride, count))
    {
        sApi->Lua_pushboolean(L, 0);
        return 1;
    }
    if (HasArg(L, 3))
    {
        const int explicitType = TypeFromName(sApi->LuaL_checkstring(L, 3), type);
        if (explicitType != type)
        {
            type = explicitType;
            stride = TypeBytes(type);
        }
    }
    const int index = HasArg(L, 4) ? (int)sApi->LuaL_checkinteger(L, 4) : 0;
    addr += (uint32_t)(index * stride);
    if (type == GCNW_VAR_STR)
    {
        const std::string text = sApi->LuaL_checkstring(L, 2);
        const size_t cap = stride > 1 ? (size_t)stride : text.size() + 1;
        for (size_t i = 0; i < cap; ++i)
        {
            GcnGuestHost::Write(addr + (uint32_t)i, i < text.size() ? (uint8_t)text[i] : 0, 1);
        }
    }
    else if (type == GCNW_VAR_F32)
    {
        const float f = (float)sApi->LuaL_checknumber(L, 2);
        uint32_t raw;
        memcpy(&raw, &f, 4);
        GcnGuestHost::Write(addr, raw, 4);
    }
    else
    {
        GcnGuestHost::Write(addr, (uint32_t)(int64_t)sApi->LuaL_checknumber(L, 2), TypeBytes(type));
    }
    sApi->Lua_pushboolean(L, 1);
    return 1;
}

int Address(lua_State* L)
{
    uint32_t addr = 0, size = 0;
    int type = 0, count = 0, stride = 0;
    if (!GcnGuestHost::Resolve(sApi->LuaL_checkstring(L, 1), addr, size, type, count, stride))
    {
        sApi->Lua_pushnil(L);
        return 1;
    }
    sApi->Lua_pushinteger(L, (long long)addr);
    sApi->Lua_pushinteger(L, (long long)size);
    return 2;
}

int Request(lua_State* L)
{
    const std::string name = sApi->LuaL_checkstring(L, 1);
    std::vector<int> args;
    const int top = sApi->Lua_gettop(L);
    for (int i = 2; i <= top; ++i)
    {
        args.push_back((int)sApi->LuaL_checkinteger(L, i));
    }
    const int id = GcnGuestHost::BridgeRequest(name, args);
    if (id > 0)
        sApi->Lua_pushinteger(L, id);
    else
        sApi->Lua_pushnil(L);
    return 1;
}

int RequestResult(lua_State* L)
{
    int result = 0;
    if (GcnGuestHost::BridgeResult((int)sApi->LuaL_checkinteger(L, 1), result))
        sApi->Lua_pushinteger(L, result);
    else
        sApi->Lua_pushnil(L);
    return 1;
}

void SetString(lua_State* L, const char* field, const std::string& value)
{
    sApi->Lua_pushstring(L, value.c_str());
    sApi->Lua_setfield(L, -2, field);
}

int Events(lua_State* L)
{
    const std::vector<GcnGuestHost::BridgeEvent> events = GcnGuestHost::BridgeEvents();
    sApi->Lua_createtable(L, (int)events.size(), 0);
    for (size_t i = 0; i < events.size(); ++i)
    {
        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 2);
        SetString(L, "name", events[i].name);
        sApi->Lua_createtable(L, (int)events[i].args.size(), 0);
        for (size_t a = 0; a < events[i].args.size(); ++a)
        {
            sApi->Lua_pushinteger(L, (long long)a + 1);
            sApi->Lua_pushinteger(L, events[i].args[a]);
            sApi->Lua_rawset(L, -3);
        }
        sApi->Lua_setfield(L, -2, "args");
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}

int Variables(lua_State* L)
{
    const std::vector<GcnGuestHost::BridgeVar> vars = GcnGuestHost::BridgeVariables();
    sApi->Lua_createtable(L, (int)vars.size(), 0);
    for (size_t i = 0; i < vars.size(); ++i)
    {
        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 4);
        SetString(L, "name", vars[i].name);
        SetString(L, "type", TypeName(vars[i].type));
        sApi->Lua_pushinteger(L, vars[i].count);
        sApi->Lua_setfield(L, -2, "count");
        SetString(L, "help", vars[i].help);
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}

int Requests(lua_State* L)
{
    const std::vector<GcnGuestHost::BridgeRequestInfo> requests = GcnGuestHost::BridgeRequests();
    sApi->Lua_createtable(L, (int)requests.size(), 0);
    for (size_t i = 0; i < requests.size(); ++i)
    {
        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 2);
        SetString(L, "name", requests[i].name);
        SetString(L, "help", requests[i].help);
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}

int HoldButtons(lua_State* L)
{
    const int mask = (int)sApi->LuaL_checkinteger(L, 1);
    const int frames = HasArg(L, 2) ? (int)sApi->LuaL_checkinteger(L, 2) : 1;
    GcnGuestHost::HoldButtons((uint16_t)mask, frames);
    return 0;
}

int SetPaused(lua_State* L)
{
    GcnPlayer::SetPaused(sApi->Lua_toboolean(L, 1) != 0);
    return 0;
}

int IsPaused(lua_State* L)
{
    sApi->Lua_pushboolean(L, GcnPlayer::IsPaused());
    return 1;
}
}

void GcnLua::Register(lua_State* L, PolyphaseEngineAPI* api)
{
    if (L == nullptr || api == nullptr || api->Lua_createtable == nullptr || api->LuaL_setfuncs == nullptr ||
        api->Lua_setglobal == nullptr)
    {
        return;
    }
    sApi = api;
    static const LuaReg kFuncs[] = {
        {"IsRunning", IsRunning}, {"Title", Title},         {"Frame", Frame},
        {"Read", Read},           {"Write", Write},         {"Address", Address},
        {"Request", Request},     {"Result", RequestResult},       {"Events", Events},
        {"Variables", Variables}, {"Requests", Requests},   {"HoldButtons", HoldButtons},
        {"SetPaused", SetPaused}, {"IsPaused", IsPaused},   {nullptr, nullptr},
    };
    sApi->Lua_createtable(L, 0, 14);
    sApi->LuaL_setfuncs(L, kFuncs, 0);
    sApi->Lua_setglobal(L, "Gcn");
}

#else

void GcnLua::Register(lua_State*, PolyphaseEngineAPI*)
{
}

#endif
