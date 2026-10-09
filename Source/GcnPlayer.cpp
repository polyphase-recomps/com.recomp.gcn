/**
 * @file GcnPlayer.cpp
 * @brief Runs a GameCube game in-process (GcnGuestHost) and shows it (see GcnPlayer.h).
 */

#include "GcnPlayer.h"

#include "AssetManager.h"
#include "Engine.h"
#include "Input/Input.h"
#include "Input/InputTypes.h"
#include "Log.h"
#include "Nodes/Widgets/Quad.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include "GcnGuestHost.h"
#include "GcnLauncher.h"
#include "GcnProvider.h"

// com.recomp.mod.base: mod settings, resolution scaler, shared menus
#include "ModBaseDisplay.h"
#include "ModBaseSettings.h"
#include "ModBaseProvider.h"

extern "C" {
#include "Gcn/gcn_disc.h"
#include "Gcn/gcn_overlay.h"
#include "Gcn/gcn_gpu.h"
#include "Gcn/gcn_vk.h"
#include "Gcn/gcnw_module.h"
}

#if PLATFORM_DOLPHIN
#include <ogc/gx.h>

#include "Graphics/GraphicsTypes.h" // TextureResource: the GX texture's tiled buffer
#include "Renderer.h"                // the engine's clear color: the game's
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif PLATFORM_3DS
#include <sys/stat.h>
#endif

FORCE_LINK_DEF(GcnPlayer);
DEFINE_NODE(GcnPlayer, Node3D);

PolyphaseEngineAPI* GcnPlayer::sAPI = nullptr;

static std::vector<GcnPlayer*> sLivePlayers;
static GcnPlayer* sOwner = nullptr; // the player whose game runs (one game at a time)
static bool sPaused = false;

void GcnPlayer::SetEngineAPI(PolyphaseEngineAPI* api)
{
    sAPI = api;
}

void GcnPlayer::ShutdownAll()
{
    for (GcnPlayer* player : sLivePlayers)
    {
        player->StopGame();
    }
    GcnGuestHost::Stop();
}

void GcnPlayer::SetPaused(bool paused)
{
    sPaused = paused;
}

bool GcnPlayer::IsPaused()
{
    return sPaused;
}

void GcnPlayer::RestartGame(const std::string& package)
{
    for (GcnPlayer* player : sLivePlayers)
    {
        if (player->mGame == package)
        {
            player->StopGame();
            player->mStartAttempted = false; // the next tick starts it
        }
    }
}

GcnPlayer::GcnPlayer()
{
}

GcnPlayer::~GcnPlayer()
{
}

void GcnPlayer::Create()
{
    Node3D::Create();
    SetName("GcnPlayer");
    EnsureDisplayQuad();
    sLivePlayers.push_back(this);
}

void GcnPlayer::Destroy()
{
    StopGame();
    for (size_t i = 0; i < sLivePlayers.size(); ++i)
    {
        if (sLivePlayers[i] == this)
        {
            sLivePlayers.erase(sLivePlayers.begin() + i);
            break;
        }
    }
    // Only through the WeakPtr: an auto-created child may already be gone here.
    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(nullptr);
    }
    mDisplayQuad = nullptr;
    mBoundQuad = WeakPtr<Quad>();
    mFrameTexture = nullptr;
    Node3D::Destroy();
}

void GcnPlayer::GatherProperties(std::vector<Property>& outProps)
{
    Node3D::GatherProperties(outProps);
    outProps.push_back(Property(DatumType::String, "Game", this, &mGame));
    outProps.push_back(Property(DatumType::String, "Disc Image", this, &mDiscPath));
    outProps.push_back(Property(DatumType::String, "Save Folder", this, &mSaveDir));
    outProps.push_back(Property(DatumType::Bool, "Stretch", this, &mStretch));
}

void GcnPlayer::SaveStream(Stream& stream, Platform platform)
{
    Node3D::SaveStream(stream, platform);
    stream.WriteString(mGame);
    stream.WriteString(mDiscPath);
    stream.WriteString(mSaveDir);
    stream.WriteBool(mStretch);
}

void GcnPlayer::LoadStream(Stream& stream, Platform platform, uint32_t version)
{
    Node3D::LoadStream(stream, platform, version);
    stream.ReadString(mGame);
    stream.ReadString(mDiscPath);
    stream.ReadString(mSaveDir);
    mStretch = stream.ReadBool();
}

std::string GcnPlayer::ResolvePath(const std::string& path) const
{
    const bool isAbsolute = path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
    std::string full = isAbsolute ? path : GetEngineState()->mProjectDirectory + path;
#if PLATFORM_WINDOWS
    for (char& c : full)
    {
        if (c == '/') c = '\\';
    }
    char absolute[MAX_PATH];
    if (GetFullPathNameA(full.c_str(), sizeof(absolute), absolute, nullptr) != 0)
    {
        full = absolute;
    }
#elif PLATFORM_3DS
    // packaged content is in the application's RomFS; what isn't there (saves) on the SD
    // card - the order the engine looks in for its own files
    struct stat info;
    if (!isAbsolute && stat(full.c_str(), &info) != 0)
    {
        const std::string romfs = "romfs:/" + full;
        if (stat(romfs.c_str(), &info) == 0)
        {
            return romfs;
        }
    }
#endif
    return full;
}

// Value of a top-level string field in a flat JSON object (game.json); false if absent.
static bool ReadJsonString(const std::string& text, const char* key, std::string& out)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t pos = text.find(quoted);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + quoted.size());
    if (pos == std::string::npos) return false;
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    out.clear();
    for (size_t i = pos + 1; i < text.size() && text[i] != '"'; ++i)
    {
        if (text[i] == '\\' && i + 1 < text.size())
        {
            ++i;
        }
        out += text[i];
    }
    return true;
}

static bool FileExists(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return (bool)file;
}

void GcnPlayer::ResolveGameDefaults(std::string& disc, std::string& saves) const
{
    disc = mDiscPath;
    saves = mSaveDir;
    if (mGame.empty())
    {
        return;
    }
    // game.json in the game package's Assets/ (packaged with the game): disc paths are
    // relative to that folder, saves to the project
    const std::string packageDir = "Packages/" + mGame + "/Assets/";
    std::ifstream file(ResolvePath(packageDir + "game.json"), std::ios::binary);
    if (!file)
    {
        LogWarning("GcnPlayer: no game.json in %s", ResolvePath(packageDir).c_str());
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    std::string value;

    if (disc.empty())
    {
        // "disc": the disc unpacked into the package by its build (packaged with the game),
        // "disc_image": an image put there by hand, "disc_dev": next to the decomp
        for (const char* key : {"disc", "disc_image", "disc_dev"})
        {
            if (ReadJsonString(text, key, value) && !value.empty())
            {
                const std::string path = ResolvePath(packageDir + value);
                if (gcn_disc_is_unpacked(path.c_str()) || FileExists(path))
                {
                    disc = packageDir + value;
                    break;
                }
            }
        }
    }
    if (saves.empty() && ReadJsonString(text, "saves", value) && !value.empty())
    {
        saves = value;
    }
}

bool GcnPlayer::StartGame()
{
    if (sOwner != nullptr && sOwner != this)
    {
        LogWarning("GcnPlayer: another GcnPlayer runs a game; one game at a time");
        return false;
    }
    const GcnwModule* module = gcnw_find_module(mGame.c_str());
    if (module == nullptr)
    {
        LogError("GcnPlayer: %s is not built into com.recomp.gcn yet: Packaging > Target Options > GCN Recomp > "
                 "Setup Dependencies Now (or the game package's Native/build script), then Reload Native Addons",
                 mGame.c_str());
        ShowStatus({std::string(mGame) + " is not built into com.recomp.gcn yet.",
                    "Packaging > Target Options > GCN Recomp > Setup Dependencies Now,",
                    "then Reload Native Addons (or package again)."});
        return false;
    }
    // the game's build name: a recomp build's module is "<name>recomp" (build_recomp.ps1); both
    // builds of a game share its unpacked disc and its saves
    std::string baseName = module->name;
    if (baseName.size() > 6 && baseName.compare(baseName.size() - 6, 6, "recomp") == 0)
    {
        baseName.resize(baseName.size() - 6);
    }
    std::string disc, saves;
    ResolveGameDefaults(disc, saves);
    if (mDiscPath.empty())
    {
        // the disc the player chose in the launcher (GcnLauncher), then the one the recomp build
        // unpacked into the project (Assets/Recomp/<name>/Disc), then the package's own (Disc
        // Image set by hand still wins)
        const std::string chosen = GcnLauncher::ChosenDisc(mGame);
        const std::string projectDisc = "Assets/Recomp/" + baseName + "/Disc";
        if (!chosen.empty())
        {
            disc = chosen;
        }
        else if (gcn_disc_is_unpacked(ResolvePath(projectDisc).c_str()))
        {
            disc = projectDisc;
        }
    }
    if (disc.empty())
    {
        const std::string where = ResolvePath("Packages/" + mGame + "/Assets/Disc");
        LogError("GcnPlayer: no disc for %s: choose it in a launcher scene (GcnLauncher), run Setup Dependencies (it "
                 "unpacks your disc into %s), or set Disc Image",
                 module->title, where.c_str());
        ShowStatus({std::string("No game disc for ") + module->title + ".",
                    "Choose your disc image in the game's launcher scene,",
                    "or Packaging > Target Options > GCN Recomp > Setup Dependencies Now",
                    "unpacks your own disc image into the project, then package again.",
                    "Looked in:", where});
        return false;
    }
    if (saves.empty())
    {
        saves = std::string("Saves/") + baseName;
    }
#if !PLATFORM_DOLPHIN && !PLATFORM_3DS && !PLATFORM_ANDROID
    // the host's GPU draws (Vulkan, gcn_vk.c): software rasteriser if it cannot, GCN_GPU=0 forces it
    gcn_gpu_use_host_gpu(1);
#endif
    if (!GcnGuestHost::Start(module, ResolvePath(disc), ResolvePath(saves)))
    {
        ShowStatus({std::string(module->title) + " did not start.", "Cannot open the disc or set up the game:",
                    ResolvePath(disc)});
        return false;
    }
    sOwner = this;
    mRunning = true;
    mReportedExit = false;
    mLastSerial = 0;
    mFrameTime = 0.0f;
    LogDebug("GcnPlayer: %s started (disc %s, saves %s)", module->title, ResolvePath(disc).c_str(),
             ResolvePath(saves).c_str());
#if !PLATFORM_DOLPHIN && !PLATFORM_3DS && !PLATFORM_ANDROID
    if (gcn_gpu_host_gpu_active())
    {
        LogDebug("GcnPlayer: drawing on the GPU (%s), Resolution up to %dx", gcn_vk_status(), gcn_gpu_max_render_scale());
    }
    else
    {
        LogWarning("GcnPlayer: drawing with the software rasteriser (%s), Resolution up to %dx", gcn_vk_status(),
                   gcn_gpu_max_render_scale());
    }
#endif
    return true;
}

void GcnPlayer::StopGame()
{
    if (!mRunning)
    {
        return;
    }
    GcnGuestHost::Stop();
    if (mAudioStream && sAPI && sAPI->Audio_CloseStream)
    {
        sAPI->Audio_CloseStream(mAudioStream);
    }
    mAudioStream = 0;
    mAudioRate = 0;
    mRunning = false;
    if (sOwner == this)
    {
        sOwner = nullptr;
    }
}

static int8_t StickValue(float v)
{
    float s = v * 100.0f; // GameCube sticks reach about +-100
    if (s > 127.0f) s = 127.0f;
    if (s < -128.0f) s = -128.0f;
    return (int8_t)s;
}

void GcnPlayer::ReadPad(int port, GcnPad& pad) const
{
    memset(&pad, 0, sizeof(pad));
    pad.connected = port == 0;
    float lx = 0.0f, ly = 0.0f, cx = 0.0f, cy = 0.0f, lt = 0.0f, rt = 0.0f;

    // keyboard on port 0: arrows = stick, X A / Z B / S X / A Y, Q L, W R, E Z,
    // I J K L = C-stick, Enter Start
    if (port == 0 && sAPI && sAPI->IsKeyDown)
    {
        auto down = [](int32_t key) { return sAPI->IsKeyDown(key); };
        if (down(POLYPHASE_KEY_LEFT)) lx -= 1.0f;
        if (down(POLYPHASE_KEY_RIGHT)) lx += 1.0f;
        if (down(POLYPHASE_KEY_UP)) ly += 1.0f;
        if (down(POLYPHASE_KEY_DOWN)) ly -= 1.0f;
        if (down(POLYPHASE_KEY_J)) cx -= 1.0f;
        if (down(POLYPHASE_KEY_L)) cx += 1.0f;
        if (down(POLYPHASE_KEY_I)) cy += 1.0f;
        if (down(POLYPHASE_KEY_K)) cy -= 1.0f;
        if (down(POLYPHASE_KEY_X)) pad.buttons |= GCN_PAD_A;
        if (down(POLYPHASE_KEY_Z)) pad.buttons |= GCN_PAD_B;
        if (down(POLYPHASE_KEY_S)) pad.buttons |= GCN_PAD_X;
        if (down(POLYPHASE_KEY_A)) pad.buttons |= GCN_PAD_Y;
        if (down(POLYPHASE_KEY_Q)) lt = 1.0f;
        if (down(POLYPHASE_KEY_W)) rt = 1.0f;
        if (down(POLYPHASE_KEY_E)) pad.buttons |= GCN_PAD_Z;
        if (down(POLYPHASE_KEY_ENTER)) pad.buttons |= GCN_PAD_START;
        if (down(POLYPHASE_KEY_1)) pad.buttons |= GCN_PAD_LEFT;
        if (down(POLYPHASE_KEY_2)) pad.buttons |= GCN_PAD_RIGHT;
        if (down(POLYPHASE_KEY_3)) pad.buttons |= GCN_PAD_UP;
        if (down(POLYPHASE_KEY_4)) pad.buttons |= GCN_PAD_DOWN;
    }

    if (INP_IsGamepadConnected(port))
    {
        pad.connected = 1;
        auto pdown = [port](int32_t button) { return INP_IsGamepadButtonDown(button, port); };
        lx += INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, port);
        ly += INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_Y, port);
        cx += INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_X, port);
        cy += INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_Y, port);
        const float l = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTRIGGER, port);
        const float r = INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTRIGGER, port);
        if (l > lt) lt = l;
        if (r > rt) rt = r;
        if (pdown(GAMEPAD_A)) pad.buttons |= GCN_PAD_A;
        if (pdown(GAMEPAD_B)) pad.buttons |= GCN_PAD_B;
        if (pdown(GAMEPAD_X)) pad.buttons |= GCN_PAD_X;
        if (pdown(GAMEPAD_Y)) pad.buttons |= GCN_PAD_Y;
        if (pdown(GAMEPAD_START)) pad.buttons |= GCN_PAD_START;
        if (pdown(GAMEPAD_UP)) pad.buttons |= GCN_PAD_UP;
        if (pdown(GAMEPAD_DOWN)) pad.buttons |= GCN_PAD_DOWN;
        if (pdown(GAMEPAD_LEFT)) pad.buttons |= GCN_PAD_LEFT;
        if (pdown(GAMEPAD_RIGHT)) pad.buttons |= GCN_PAD_RIGHT;
        if (INP_GetGamepadType(port) == GamepadType::GameCube)
        {
            if (pdown(GAMEPAD_Z)) pad.buttons |= GCN_PAD_Z;
        }
        else if (pdown(GAMEPAD_R1))
        {
            pad.buttons |= GCN_PAD_Z; // the GameCube's Z on the right bumper
        }
        if (pdown(GAMEPAD_L1)) lt = 1.0f;
    }

    pad.stick_x = StickValue(lx);
    pad.stick_y = StickValue(ly);
    pad.substick_x = StickValue(cx);
    pad.substick_y = StickValue(cy);
    pad.trigger_l = (uint8_t)(lt > 1.0f ? 255 : lt * 255.0f);
    pad.trigger_r = (uint8_t)(rt > 1.0f ? 255 : rt * 255.0f);
    // full presses click the digital L / R
    if (lt > 0.9f) pad.buttons |= GCN_PAD_L;
    if (rt > 0.9f) pad.buttons |= GCN_PAD_R;
}

void GcnPlayer::EnsureDisplayQuad()
{
    if (mDisplayQuad != nullptr)
    {
        return;
    }
    mDisplayQuad = CreateChild<Quad>("GCN Display");
    mDisplayQuad->SetAnchorMode(AnchorMode::FullStretch);
    mDisplayQuad->SetSize(1.0f, 1.0f);
    mDisplayQuad->SetObjectFit(mStretch ? ObjectFit::Fill : ObjectFit::Contain);
    mBoundQuad = ResolveWeakPtr<Quad>(mDisplayQuad);
}

void GcnPlayer::UpdateDisplayTexture(const uint8_t* pixels, int width, int height, int scale, int logicalW, int logicalH)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (scale < 1) scale = 1;
    // the game's own size (the fit modes and window presets go by it, not the render resolution
    // nor an upscaled picture's size)
    if (logicalW <= 0 || logicalH <= 0)
    {
        logicalW = width / scale;
        logicalH = height / scale;
    }
    EnsureDisplayQuad();
    Texture* texture = mFrameTexture.Get<Texture>();
    if (texture == nullptr || (int)texture->GetWidth() != width || (int)texture->GetHeight() != height)
    {
        // Must be a transient asset: Quad drops textures the AssetManager does not know.
        texture = NewTransientAsset<Texture>();
        texture->SetName("T_GcnFrame");
        texture->SetMipmapped(false);
        texture->SetFilterType(Recomp_DisplayFilterLinear(true) ? FilterType::Linear : FilterType::Nearest);
        texture->SetWrapMode(WrapMode::Clamp);
        texture->Init((uint32_t)width, (uint32_t)height, (uint8_t*)pixels);
        texture->Create();
        mFrameTexture = texture;
    }
    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(texture);
        if (mStretch || quad != mDisplayQuad)
        {
            // Stretch (the old property) fills the screen; a Quad the user bound keeps its layout
            quad->SetObjectFit(mStretch ? ObjectFit::Fill : ObjectFit::Contain);
        }
        else if (Recomp_DisplayApply(quad, texture, logicalW, logicalH, 4.0f / 3.0f, true))
        {
            // the resolution scaler (mod settings "Screen"): the console's 4:3 picture, also
            // for 640x448 copies; a changed filter needs a new texture
            mFrameTexture = nullptr;
        }
#if !PLATFORM_DOLPHIN && !PLATFORM_3DS && !PLATFORM_ANDROID
        {
            // the picture's size on screen, in pixels: what the Upscaler (FSR) draws it at
            glm::vec2 sc = quad->GetAbsoluteScale();
            if (sc.x <= 0.0f) sc.x = 1.0f;
            if (sc.y <= 0.0f) sc.y = 1.0f;
            gcn_gpu_set_output_size(int(quad->GetWidth() * sc.x + 0.5f), int(quad->GetHeight() * sc.y + 0.5f));
        }
#endif
    }
    texture->UpdatePixels(pixels, size_t(width) * size_t(height) * 4);
    GcnProvider::Get().SetFrame(logicalW, logicalH);
    Recomp_DisplayApplyWindow(logicalW, logicalH);
}

void GcnPlayer::ShowStatus(const std::vector<std::string>& lines)
{
    const int w = 640, h = 480, perLine = 100; // 6-pixel characters
    std::vector<uint32_t> image(size_t(w) * size_t(h), 0xFF201010u);
    int y = 40;

    for (const std::string& line : lines)
    {
        for (size_t at = 0; at < line.size() || (at == 0 && line.empty()); at += perLine)
        {
            const std::string part = line.substr(at, perLine);
            gcn_overlay_text(image.data(), w, w, h, 20, y, 1, 0xFFFFFFFFu, part.c_str());
            y += 14;
            if (line.empty()) break;
        }
    }
    UpdateDisplayTexture((const uint8_t*)image.data(), w, h);
}

#if PLATFORM_DOLPHIN
// Wii / GameCube: the game draws with the console's own GPU (gcn_gpu.c passthrough); its
// copies to the external frame buffer go to this texture, which the display Quad shows.
void GcnPlayer::EnsureConsoleDisplay()
{
    Texture* texture = mFrameTexture.Get<Texture>();
    if (texture != nullptr && texture == mConsoleTexture)
    {
        return;
    }
    const int w = 640, h = 480;
    std::vector<uint32_t> black(size_t(w) * size_t(h), 0xFF000000u);
    UpdateDisplayTexture((const uint8_t*)black.data(), w, h);
    texture = mFrameTexture.Get<Texture>();
    TextureResource* resource = texture != nullptr ? texture->GetResource() : nullptr;
    if (resource == nullptr || resource->mTiledBuf == nullptr)
    {
        LogError("GcnPlayer: no streaming texture for the game's picture");
        return;
    }
    // RGB565, not the streaming texture's RGBA8: the Quad blends by the texture's alpha, and a
    // copy of the game's frame buffer carries whatever alpha the game left there (often 0,
    // which showed nothing). Half the bytes to copy, too.
    const u16 texW = (u16)resource->mTexWidth, texH = (u16)resource->mTexHeight;
    memset(resource->mTiledBuf, 0, size_t(texW) * texH * 2);
    DCFlushRange(resource->mTiledBuf, size_t(texW) * texH * 2);
    GX_InitTexObj(&resource->mGxTexObj, resource->mTiledBuf, texW, texH, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    const u8 filter = texture->GetFilterType() == FilterType::Nearest ? GX_NEAR : GX_LINEAR;
    GX_InitTexObjFilterMode(&resource->mGxTexObj, filter, filter);
    gcn_gpu_set_display(resource->mTiledBuf, (int)texW, (int)texH);
    mConsoleTexture = texture;
}

// After a game frame: the GPU state the engine's renderer sets only once (GFX_Initialize),
// and state it assumes off, which the game changed behind libogc's back. What the engine
// sets per draw (vertex formats, TEV, blending...) it sets again anyway.
static void RestoreGxAfterGame()
{
    GXRModeObj* rmode = &GetEngineState()->mSystem.mGxRmode;
    const GXColor black = {0, 0, 0, 0};

    // not the pixel format: the game's picture is still in the frame buffer, in the game's
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering, ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetDispCopyGamma(GX_GM_1_0);
    const uint32_t xfbHeight = GX_SetDispCopyYScale(GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight));
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetScissorBoxOffset(0, 0);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetCullMode(GX_CULL_FRONT);
    GX_SetCoPlanar(GX_DISABLE);
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, black);
    GX_SetDstAlpha(GX_DISABLE, 0);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetNumIndStages(0);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    for (int stage = 0; stage < GX_MAX_TEVSTAGE; ++stage)
    {
        GX_SetTevDirect((u8)stage);
        GX_SetTevSwapMode((u8)stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevKColorSel((u8)stage, GX_TEV_KCSEL_1);
        GX_SetTevKAlphaSel((u8)stage, GX_TEV_KASEL_1);
    }
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_InvVtxCache();
    GX_InvalidateTexAll();
    GX_Flush();
}
#endif

void GcnPlayer::PumpAudio()
{
    if (sAPI == nullptr || sAPI->Audio_OpenStream == nullptr)
    {
        return;
    }
    for (int chunks = 0; chunks < 8; ++chunks)
    {
        const int16_t* frames = nullptr;
        uint32_t rate = 0;
        const uint32_t count = GcnGuestHost::PeekAudio(frames, 2048, rate);
        if (count == 0)
        {
            return;
        }
        if (mAudioStream == 0 || rate != mAudioRate)
        {
            if (mAudioStream && sAPI->Audio_CloseStream)
            {
                sAPI->Audio_CloseStream(mAudioStream);
            }
            mAudioStream = sAPI->Audio_OpenStream(rate, 2, 16);
            mAudioRate = rate;
        }
#if PLATFORM_DOLPHIN
        // engine streams take little-endian PCM (VOICE_*_16BIT_LE); the samples are the
        // console's own big-endian ones: swapped as they go, or they play as loud noise
        static int16_t swapped[2048 * 2];
        for (uint32_t i = 0; i < count * 2; ++i)
        {
            const uint16_t s = (uint16_t)frames[i];
            swapped[i] = (int16_t)((s >> 8) | (s << 8));
        }
        const int16_t* submit = swapped;
#else
        const int16_t* submit = frames;
#endif
        if (mAudioStream && sAPI->Audio_SubmitStreamBuffer &&
            sAPI->Audio_SubmitStreamBuffer(mAudioStream, (const uint8_t*)submit, count * 4) <= 0)
        {
            return; // the stream is full: keep the rest for the next tick
        }
        GcnGuestHost::ConsumeAudio(count);
    }
}

void GcnPlayer::Tick(float deltaTime)
{
    Node3D::Tick(deltaTime);

    if (!mStartAttempted)
    {
        mStartAttempted = true;
        StartGame();
    }
    // mod settings: written to the game once it runs, kept, saved
    ModSettings::Get().Tick(&GcnProvider::Get());
#if defined(RECOMP_DISPLAY_HAS_RESOLUTION)
    // "Resolution": the software GPU draws at 640x528 times this, from the game's next picture
    {
        const int scale = std::max(1, std::min(Recomp_DisplaySettings().resolution, gcn_gpu_max_render_scale()));
        if (scale != gcn_gpu_render_scale())
        {
            gcn_gpu_set_render_scale(scale);
        }
    }
#endif
#if defined(RECOMP_DISPLAY_HAS_RENDER_FEATURES) && !PLATFORM_DOLPHIN && !PLATFORM_3DS && !PLATFORM_ANDROID
    // "Anti-aliasing", "Upscaler", "Sharpness", "Textures": the host GPU's post-processing and
    // filtering (gcn_vk.c), from the game's next picture / primitives
    {
        const RecompDisplaySettings& d = Recomp_DisplaySettings();
        gcn_gpu_set_post(d.antialias == 1, d.upscaler == 1, d.sharpness);
        gcn_gpu_set_texture_filter(d.textures);
    }
#endif
    if (!mRunning)
    {
        return;
    }
    GcnGuestHost::FlushLog();

    const GcnGuestHost::State state = GcnGuestHost::GetState();
    if (state != GcnGuestHost::State::Running)
    {
        if (!mReportedExit)
        {
            mReportedExit = true;
            LogError("GcnPlayer: the game %s", state == GcnGuestHost::State::Crashed ? "stopped after an error"
                                                                                     : "exited");
        }
        return;
    }

    // an open settings menu (RecompMenuController) navigates with the gamepad: the game
    // gets nothing, and not the press that closes it either
    static bool sHoldUntilRelease = false;
    const bool captured = Recomp_IsInputCaptured();
    for (int port = 0; port < 4; ++port)
    {
        GcnPad pad;
        ReadPad(port, pad);
        if (port == 0)
        {
            const bool pressed = pad.buttons != 0 || pad.trigger_l > 64 || pad.trigger_r > 64;
            if (captured) sHoldUntilRelease = true;
            else if (sHoldUntilRelease && !pressed) sHoldUntilRelease = false;
        }
        if (captured || sHoldUntilRelease)
        {
            // neutral, still connected (a disconnected pad makes games show a message)
            pad.buttons = 0;
            pad.stick_x = pad.stick_y = pad.substick_x = pad.substick_y = 0;
            pad.trigger_l = pad.trigger_r = 0;
        }
        GcnGuestHost::SetPad(port, pad);
    }

#if PLATFORM_DOLPHIN
    // One core and one GPU command FIFO: a game frame runs here, in step with the engine,
    // its commands going straight to the GPU; then the engine's GX state comes back before
    // the engine draws (the game's picture is in the display texture by then).
    // The game's picture stays in the GPU's frame buffer (its copies to the external frame
    // buffer are dropped): the engine draws over it and copies it to the screen, clearing
    // with the game's clear color as the game's own copy would have.
    GcnGuestHost::ReleaseHold();
    if (!sPaused)
    {
        // the engine drew since the game's last frame: give the game back its GPU state (its
        // GX library sends only what it thinks changed, so vertex formats would be the engine's)
        gcn_gpu_resume();
        GcnGuestHost::StepFrame(2000);
        RestoreGxAfterGame();
        uint8_t clear[4];
        gcn_gpu_clear_color(clear);
        Renderer::Get()->SetClearColor(glm::vec4(clear[0], clear[1], clear[2], 255) / 255.0f);
    }
#elif PLATFORM_3DS
    // 3DS: the threads of an application share a core and the kernel doesn't time-slice
    // equal priorities, so a game slower than real time would never let the engine draw:
    // one game frame per tick, the engine waiting for it (the software GPU draws it).
    GcnGuestHost::ReleaseHold();
    if (!sPaused)
    {
        GcnGuestHost::StepFrame(2000);
    }
#else
    // The game runs on its own thread at 60 Hz while the engine draws; scripts that
    // touched its memory last tick held it at the end of a frame: let it go on.
    GcnGuestHost::SetFreeRun(!sPaused);
    GcnGuestHost::ReleaseHold();
#endif

    const uint8_t* rgba = nullptr;
    int width = 0, height = 0, scale = 1, logicalW = 0, logicalH = 0;
    if (GcnGuestHost::GetFrame(mLastSerial, rgba, width, height, &scale, &logicalW, &logicalH))
    {
        UpdateDisplayTexture(rgba, width, height, scale, logicalW, logicalH);
    }
    PumpAudio();
}
