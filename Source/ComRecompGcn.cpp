/**
 * @file ComRecompGcn.cpp
 * @brief Native addon: com.recomp.gcn
 *
 * Shared runtime for GameCube games compiled natively from their decompilations (see
 * ../Runtime). Exposes the GcnPlayer node and the Lua table `Gcn`; each game package
 * (e.g. com.recomp.starfoxadventures) only carries its build config, patches, mods and
 * game.json, and is translated into Source/Guest/<name> here. In the editor it also builds
 * the game packages (GcnDependencies): before every packaging, and from the build
 * profile's Target Options.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"
#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#endif

#include "GcnDependencies.h"
#include "GcnLua.h"
#include "GcnPlayer.h"
#include "GcnProvider.h"

static PolyphaseEngineAPI* sEngineAPI = nullptr;

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    GcnPlayer::SetEngineAPI(api);
    FORCE_LINK_CALL(GcnPlayer);
    // com.recomp.mod.base (mod settings, Recomp / Mods Lua, Mods windows) sees the game
    Recomp_RegisterProvider(&GcnProvider::Get());
    if (api && api->LogDebug)
    {
        api->LogDebug("com.recomp.gcn loaded!");
    }
    return 0;
}

static void OnUnload()
{
    // the game thread runs this module's code: stop it first
    GcnPlayer::ShutdownAll();
    Recomp_UnregisterProvider(&GcnProvider::Get());
    GcnPlayer::SetEngineAPI(nullptr);
    if (sEngineAPI && sEngineAPI->LogDebug)
    {
        sEngineAPI->LogDebug("com.recomp.gcn unloaded.");
    }
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* nodeFactory)
{
    (void)nodeFactory;
}

static void RegisterScriptFuncs(lua_State* L)
{
    GcnLua::Register(L, sEngineAPI);
}

#if EDITOR
static EditorUIHooks* sHooks = nullptr;

// Builds the game packages before packaging unless the profile turned it off (Target
// Options); a failure cancels the build.
static bool OnPreBuild(int32_t platform, void* userData)
{
    (void)platform;
    (void)userData;
    char value[8] = "";
    char decomp[512] = "";
    if (sHooks != nullptr && sHooks->GetBuildSetting != nullptr)
    {
        if (sHooks->GetBuildSetting(GcnDependencies::kSetupOption, value, sizeof(value)) && value[0] == '0')
        {
            return true;
        }
        sHooks->GetBuildSetting(GcnDependencies::kDecompOption, decomp, sizeof(decomp));
        char mode[16] = "";
        if (sHooks->GetBuildSetting(GcnDependencies::kModeOption, mode, sizeof(mode)))
        {
            GcnDependencies::SetBuildMode(mode);
        }
    }
    if (!GcnDependencies::SetupAll(decomp))
    {
        if (sEngineAPI && sEngineAPI->LogError)
        {
            sEngineAPI->LogError("[gcn] Setup Dependencies failed, packaging cancelled (see the log)");
        }
        return false;
    }
    return true;
}

static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    if (hooks->AddTargetOptions != nullptr)
    {
        hooks->AddTargetOptions(hookId, "GCN Recomp",
            [](const PolyphaseBuildContext* ctx, void*) { GcnDependencies::DrawTargetOptions(ctx); }, nullptr);
    }
    else
    {
        hooks->AddMenuItem(hookId, "Developer", "GCN/Setup Dependencies",
            [](void*) { GcnDependencies::SetupAllAsync(""); }, nullptr, nullptr);
    }
    hooks->RegisterOnPreBuild(hookId, OnPreBuild, nullptr);
    GcnDependencies::RegisterEditorUI(hooks, hookId);
    GcnDependencies::CheckReady();
}

static void TickEditor(float deltaTime)
{
    (void)deltaTime;
    GcnDependencies::Tick();
}
#endif

static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "com.recomp.gcn";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
    desc->TickEditor = TickEditor;
#else
    desc->RegisterEditorUI = nullptr;
    desc->TickEditor = nullptr;
#endif
    desc->OnEditorPreInit = nullptr;
    desc->OnEditorReady = nullptr;
    return 0;
}

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#else
extern "C" int PolyphasePlugin_GetDesc_com_recomp_gcn(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#endif
