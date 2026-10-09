/**
 * @file GcnPlayer.h
 * @brief Node that runs a natively compiled GameCube game (com.recomp.gcn runtime) and
 *        shows its frames on a Quad.
 *
 * The game is chosen by package id (Game property, e.g. "com.recomp.starfoxadventures");
 * that package's game.json gives the disc image and save folder, and the explicit
 * properties override them when set. The game must have been translated into this addon
 * (Source/Guest/<name>, by the game package's Native/build.ps1 or build.sh, or Packaging >
 * Target Options > GCN Recomp > Setup Dependencies).
 */
#pragma once

#include "AssetRef.h"
#include "Nodes/3D/Node3D.h"

#include <string>
#include <vector>

struct PolyphaseEngineAPI;
struct GcnPad;

class Texture;

class GcnPlayer : public Node3D
{
public:
    DECLARE_NODE(GcnPlayer, Node3D);

    GcnPlayer();
    virtual ~GcnPlayer();

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void LoadStream(Stream& stream, Platform platform, uint32_t version) override;

    static void SetEngineAPI(PolyphaseEngineAPI* api);
    // Stops the running game; called when the addon unloads.
    static void ShutdownAll();
    // Scripts: pause / resume the game (it keeps its last frame on screen).
    static void SetPaused(bool paused);
    static bool IsPaused();
    // The launcher's Play (GcnLauncher): every player of that game package starts it again,
    // with the disc the player chose.
    static void RestartGame(const std::string& package);

private:
    bool StartGame();
    void StopGame();
    void ReadPad(int port, GcnPad& pad) const;
    void EnsureDisplayQuad();
    // `scale`: the frame is drawn at scale x the game's resolution (mod settings "Resolution")
    // logicalW x logicalH: the picture's size in the console's pixels (0: width / scale)
    void UpdateDisplayTexture(const uint8_t* pixels, int width, int height, int scale = 1, int logicalW = 0,
                              int logicalH = 0);
#if PLATFORM_DOLPHIN
    // Wii / GameCube: the display texture the console's GPU copies the game's frames into.
    void EnsureConsoleDisplay();
    Texture* mConsoleTexture = nullptr;
#endif
    // Instead of the game: a dark screen with these lines (why it does not run).
    void ShowStatus(const std::vector<std::string>& lines);
    void PumpAudio();
    std::string ResolvePath(const std::string& path) const;
    // Fills empty path properties from Packages/<mGame>/game.json.
    void ResolveGameDefaults(std::string& disc, std::string& saves) const;

    static PolyphaseEngineAPI* sAPI;

    class Quad* mDisplayQuad = nullptr;
    WeakPtr<class Quad> mBoundQuad;
    AssetRef mFrameTexture; // transient, owned by the AssetManager; this ref keeps it alive

    std::string mGame = "com.recomp.starfoxadventures";
    std::string mDiscPath;
    std::string mSaveDir;
    bool mStretch = false; // fill the node instead of keeping 4:3

    bool mStartAttempted = false;
    bool mRunning = false;
    bool mReportedExit = false;
    float mFrameTime = 0.0f;
    uint32_t mLastSerial = 0;
    uint32_t mAudioStream = 0;
    uint32_t mAudioRate = 0;
};
