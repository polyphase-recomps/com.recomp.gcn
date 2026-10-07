/**
 * @file GcnDependencies.cpp
 * @brief Builds the GameCube game packages from the editor (see GcnDependencies.h).
 *
 *   editor on    command (in Packages/<game>/Native)
 *   Windows      build.ps1 -Addon [-Decomp DIR] [-Disc FILE] [-RestoreDisc]
 *   Linux        [DECOMP=DIR] [DISC=FILE] [RESTORE_DISC=1] sh build.sh --addon
 *
 * The result (Source/Guest/<name> in this addon) is portable C: one build serves every
 * platform the project is packaged for. The same build unpacks the user's disc into the
 * game package (Assets/Disc), where the player reads it and packaging picks it up.
 *
 * Recomp mode (Build mode "recomp", packages with a Recomp/ folder, Windows): instead,
 * com.recomp.gcn/Runtime/tools/recomp/build_recomp.ps1 recompiles the disc's own code into a
 * library in com.recomp.gcn/Lib, registered by Source/Guest/<name>_recomp, and unpacks the disc
 * into the PROJECT (Assets/Recomp/<name>/Disc): what the game reads at run time stays out of the
 * packages. The two builds of a game replace each other. Build mode "live" (build_recomp.ps1
 * -Live) makes a library without the game's code that recompiles it from the disc when the game
 * starts: no disc needed to build, and with "Package the unpacked disc" off the packaged game
 * holds no game data at all (the player picks their disc in a launcher scene, GcnLauncher).
 *
 * Tools > Recomp > GameCube > Pre Process Rom is the window for that: pick your disc image
 * and the decomp (saved to the game package's Native/local.json, not in git), check
 * them, run it, see its output.
 */

#include "GcnDependencies.h"

#if EDITOR && (PLATFORM_WINDOWS || PLATFORM_LINUX)

#include "AssetManager.h"
#include "Engine.h"
#include "Log.h"
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"

extern "C" {
#include "Gcn/gcnw_module.h"
}

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#endif

namespace
{
struct GamePackage
{
    std::string id;
    std::string packageDir; // .../Packages/<id>/ (forward slashes, trailing)
    bool hasDecomp = false; // Native/gcn_game.json + build script
    bool hasRecomp = false; // Recomp/game.json
    std::string nativeDir; // .../Packages/<id>/Native/ (forward slashes, trailing)
    std::string name;      // gcn_game.json "name"
    std::string title;     // gcn_game.json "title"
    std::string version;   // gcn_game.json "version", e.g. GSAE01_rev1
    std::string decomp;    // the decomp the build uses (local.json, else gcn_game.json), absolute
    std::string disc;      // the disc image the build uses, absolute
};

struct GameStatus
{
    std::string id;
    bool translated; // Source/Guest/<name> exists
    bool loaded;     // ... and this addon build contains it
    bool unpacked;   // Assets/Disc/disc.idx exists
    std::string unpackedId; // the game id written in it
    bool recomp = false;    // built (or to be built) as the recomp build
    bool live = false;      // ...a Live one (needs no unpacked disc)
};

std::mutex sLock;
std::vector<std::string> sPending; // background output for the log, written by Tick
std::deque<std::string> sRecent;   // the last lines, for the Pre Process window
std::thread sThread;
std::atomic<bool> sRunning{false};
std::atomic<bool> sFinished{false};
std::atomic<bool> sCancel{false};
std::string sRunningId;            // game being pre-processed ("" = all)
std::string sResultId;             // game the last run was for
int sResult = 0;                   // 0 none, 1 done, 2 failed, 3 cancelled
std::chrono::steady_clock::time_point sRunStart;
#if PLATFORM_WINDOWS
HANDLE sJob = nullptr;             // the running build and everything it starts
#endif

std::string sMode = "auto";      // kModeOption, set by the Target Options / before packaging
bool sPackageDisc = true;        // kPackageDiscOption
std::vector<GameStatus> sStatus; // Target Options panel, refreshed on demand
bool sStatusValid = false;
int sStatusGeneration = 0;       // bumped when a run ends

EditorUIHooks* sHooks = nullptr;
uint64_t sHookId = 0;

const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kWarn(1.0f, 0.7f, 0.3f, 1.0f);
const ImVec4 kBad(1.0f, 0.5f, 0.4f, 1.0f);

// ---- small platform layer ---------------------------------------------------------------
std::string Slashes(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\') c = '/';
    }
    return path;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '"' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '"' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string FullPath(const std::string& path)
{
    std::string full = path;
#if PLATFORM_WINDOWS
    char buf[MAX_PATH];
    if (GetFullPathNameA(path.c_str(), sizeof(buf), buf, nullptr) != 0) full = buf;
#else
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf) != nullptr) full = buf;
#endif
    return Slashes(full);
}

std::string ProjectDir()
{
    std::string dir = FullPath(GetEngineState()->mProjectDirectory);
    if (!dir.empty() && dir.back() != '/') dir += "/";
    return dir;
}

bool Exists(const std::string& path)
{
#if PLATFORM_WINDOWS
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

bool IsAbsolute(const std::string& path)
{
    return (path.size() > 1 && path[1] == ':') || (!path.empty() && (path[0] == '/' || path[0] == '\\'));
}

std::vector<std::string> SubDirectories(const std::string& dir)
{
    std::vector<std::string> names;
#if PLATFORM_WINDOWS
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return names;
    while (struct dirent* entry = readdir(d))
    {
        if (entry->d_name[0] != '.' && Exists(dir + entry->d_name + "/")) names.push_back(entry->d_name);
    }
    closedir(d);
#endif
    return names;
}

std::string ReadText(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    if (!file) return std::string();
    buffer << file.rdbuf();
    return buffer.str();
}

std::string JsonString(const std::string& text, const char* key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t pos = text.find(quoted);
    if (pos == std::string::npos) return std::string();
    pos = text.find(':', pos + quoted.size());
    if (pos == std::string::npos) return std::string();
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) return std::string();
    std::string out;
    for (size_t i = pos + 1; i < text.size() && text[i] != '"'; ++i)
    {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        out += text[i];
    }
    return out;
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

void Emit(const std::string& line, bool background)
{
    if (background)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sPending.push_back(line);
        sRecent.push_back(line);
        while (sRecent.size() > 400) sRecent.pop_front();
    }
    else if (line.find("FAILED") != std::string::npos || line.find("error") != std::string::npos)
    {
        LogError("%s", line.c_str());
    }
    else
    {
        LogDebug("%s", line.c_str());
    }
}

void EmitOutput(std::string& line, const char* data, size_t size, bool background)
{
    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] != '\n')
        {
            line += data[i];
            continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) Emit("[gcn] " + line, background);
        line.clear();
    }
}

// Runs a command line in a directory; output to the log. True if it exits with 0.
bool RunCommand(const std::string& command, const std::string& workDir, bool background)
{
    std::string line;
    char buf[4096];
#if PLATFORM_WINDOWS
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        Emit("[gcn] cannot create a pipe for the setup", background);
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::vector<char> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    // in a job, so Cancel stops the build with everything it started (python, clang...)
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        workDir.c_str(), &si, &pi))
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        if (job != nullptr) CloseHandle(job);
        Emit("[gcn] cannot start: " + command, background);
        return false;
    }
    if (job != nullptr)
    {
        AssignProcessToJobObject(job, pi.hProcess);
        std::lock_guard<std::mutex> guard(sLock);
        sJob = job;
    }
    ResumeThread(pi.hThread);
    CloseHandle(writePipe);
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0)
    {
        EmitOutput(line, buf, got, background);
    }
    CloseHandle(readPipe);
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job != nullptr)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sJob = nullptr;
        CloseHandle(job);
    }
#else
    const std::string full = "cd '" + workDir + "' && " + command + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    int code = 1;
    if (pipe == nullptr)
    {
        Emit("[gcn] cannot start: " + command, background);
        return false;
    }
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), pipe)) > 0)
    {
        EmitOutput(line, buf, got, background);
    }
    code = pclose(pipe);
#endif
    if (!line.empty()) Emit("[gcn] " + line, background);
    return code == 0;
}

// ---- game packages ------------------------------------------------------------------------
std::vector<GamePackage> FindGamePackages()
{
    std::vector<GamePackage> games;
    const std::string packages = ProjectDir() + "Packages/";

    for (const std::string& id : SubDirectories(packages))
    {
        const std::string native = packages + id + "/Native/";
        const std::string config = ReadText(native + "gcn_game.json");
        const std::string recomp = ReadText(packages + id + "/Recomp/game.json");
        if (config.empty() && recomp.empty()) continue;
        GamePackage game;
        game.id = id;
        game.packageDir = packages + id + "/";
        game.nativeDir = native;
        game.hasRecomp = !recomp.empty();
#if PLATFORM_WINDOWS
        game.hasDecomp = !config.empty() && Exists(native + "build.ps1");
#else
        game.hasDecomp = !config.empty() && Exists(native + "build.sh");
#endif
        const std::string& info = config.empty() ? recomp : config;
        game.name = JsonString(info, "name");
        game.title = JsonString(info, "title");
        game.version = JsonString(info, "version");
        if (game.title.empty()) game.title = id;
        // the paths the build uses: Native/local.json, else gcn_game.json (relative to
        // Native/; the disc there is relative to the decomp)
        const std::string local = ReadText(native + "local.json");
        std::string decomp = Slashes(JsonString(local, "decomp"));
        std::string disc = Slashes(JsonString(local, "disc"));
        if (decomp.empty()) decomp = JsonString(config, "decomp");
        game.decomp = FullPath(IsAbsolute(decomp) ? decomp : native + decomp);
        if (disc.empty())
        {
            const std::string rel = JsonString(config, "disc");
            game.disc = rel.empty() ? std::string() : FullPath(game.decomp + "/" + rel);
        }
        else
        {
            game.disc = FullPath(IsAbsolute(disc) ? disc : native + disc);
        }
        games.push_back(game);
    }
    return games;
}

std::string AddonDir()
{
    return ProjectDir() + "Packages/com.recomp.gcn/";
}

// whether this game is built (or to be built) as the recomp build (AOT or Live)
bool UseRecomp(const GamePackage& game)
{
    if (!game.hasRecomp) return false;
    if (!game.hasDecomp || sMode == "recomp" || sMode == "live") return true;
    if (sMode == "decomp") return false;
    return Exists(AddonDir() + "Source/Guest/" + game.name + "_recomp/mode.txt"); // auto: as last built
}

// ...and as the Live one (recompiled from the disc when the game starts)
bool UseLive(const GamePackage& game)
{
    if (!UseRecomp(game)) return false;
    if (sMode == "live") return true;
    if (sMode == "recomp") return false;
    return Trim(ReadText(AddonDir() + "Source/Guest/" + game.name + "_recomp/mode.txt")) == "recomp-live";
}

// where the build unpacks the disc: the project for the recomp build (Assets/Recomp/<name>/Disc),
// the package for the decomp build
std::string DiscDir(const GamePackage& game)
{
    if (UseRecomp(game)) return ProjectDir() + "Assets/Recomp/" + game.name + "/Disc/";
    return ProjectDir() + "Packages/" + game.id + "/Assets/Disc/";
}

GameStatus GetGameStatus(const GamePackage& game)
{
    GameStatus s;
    s.id = game.id;
    s.recomp = UseRecomp(game);
    s.live = UseLive(game);
    s.translated = s.recomp ? Exists(AddonDir() + "Source/Guest/" + game.name + "_recomp/" + game.name +
                                     "_recomp_guest_register.cpp")
                            : Exists(AddonDir() + "Source/Guest/" + game.name + "/" + game.name + "_guest_module.c");
    s.loaded = gcnw_find_module(game.id.c_str()) != nullptr;
    const std::string idx = ReadText(DiscDir(game) + "disc.idx");
    s.unpacked = !idx.empty();
    if (s.unpacked)
    {
        const size_t eol = idx.find('\n');
        const std::string first = idx.substr(0, eol);
        const size_t sp = first.find_last_of(' ');
        s.unpackedId = sp == std::string::npos ? std::string() : Trim(first.substr(sp + 1));
    }
    return s;
}

std::vector<GameStatus> GetStatus()
{
    std::vector<GameStatus> status;
    for (const GamePackage& game : FindGamePackages())
    {
        status.push_back(GetGameStatus(game));
    }
    return status;
}

bool WriteLocal(const GamePackage& game, const std::string& decomp, const std::string& disc)
{
    std::ofstream out(game.nativeDir + "local.json", std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n    \"decomp\": \"" << JsonEscape(Slashes(decomp)) << "\",\n    \"disc\": \"" << JsonEscape(Slashes(disc))
        << "\"\n}\n";
    return (bool)out;
}

// decomp / disc: "" = what the game package's local.json or gcn_game.json says
// recomp mode: build_recomp.ps1 (Windows x64 only)
bool SetupRecomp(const GamePackage& game, const std::string& decomp, const std::string& disc, bool background)
{
#if PLATFORM_WINDOWS
    const std::string script = AddonDir() + "Runtime/tools/recomp/build_recomp.ps1";
    if (!Exists(script))
    {
        Emit("[gcn] " + game.id + ": com.recomp.gcn has no recomp mode (Runtime/tools/recomp/build_recomp.ps1)", background);
        return false;
    }
    std::string args = "-Package \"" + game.packageDir + "\"";
    if (!decomp.empty()) args += " -Decomp \"" + decomp + "\"";
    if (!disc.empty()) args += " -Disc \"" + disc + "\"";
    if (UseLive(game)) args += " -Live";
#if defined(_DEBUG)
    args += " -DebugCrt"; // the library's C runtime matches this Debug editor
#endif
    Emit("[gcn] " + game.id + ": recomp build: build_recomp.ps1 " + args, background);
    const bool ok = RunCommand("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" " + args,
                               game.packageDir, background);
    if (sCancel)
    {
        Emit("[gcn] " + game.id + ": cancelled", background);
        return false;
    }
    Emit("[gcn] " + game.id + (ok ? ": recompiled into com.recomp.gcn" : ": RECOMP BUILD FAILED"), background);
    return ok;
#else
    (void)decomp;
    (void)disc;
    Emit("[gcn] " + game.id + ": the recomp build is Windows only for now: use Build mode Decomp", background);
    return false;
#endif
}

bool SetupGame(const GamePackage& game, const std::string& decompIn, const std::string& discIn, bool restoreDisc,
               bool background)
{
    const std::string decomp = Slashes(decompIn), disc = Slashes(discIn);
    if (UseRecomp(game))
    {
        return SetupRecomp(game, decomp, disc, background);
    }
    bool ok;
#if PLATFORM_WINDOWS
    if (!Exists(game.nativeDir + "build.ps1"))
    {
        Emit("[gcn] " + game.id + ": no Native/build.ps1", background);
        return false;
    }
    std::string args = "-Addon";
    if (!decomp.empty()) args += " -Decomp \"" + decomp + "\"";
    if (!disc.empty()) args += " -Disc \"" + disc + "\"";
    if (restoreDisc) args += " -RestoreDisc";
    Emit("[gcn] " + game.id + ": build.ps1 " + args, background);
    ok = RunCommand("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + game.nativeDir + "build.ps1\" " + args,
                    game.nativeDir, background);
#else
    if (!Exists(game.nativeDir + "build.sh"))
    {
        Emit("[gcn] " + game.id + ": no Native/build.sh", background);
        return false;
    }
    std::string env;
    if (!decomp.empty()) env += "DECOMP='" + decomp + "' ";
    if (!disc.empty()) env += "DISC='" + disc + "' ";
    if (restoreDisc) env += "RESTORE_DISC=1 ";
    Emit("[gcn] " + game.id + ": " + env + "build.sh --addon", background);
    ok = RunCommand(env + "sh build.sh --addon", game.nativeDir, background);
#endif
    if (sCancel)
    {
        Emit("[gcn] " + game.id + ": cancelled", background);
        return false;
    }
    Emit("[gcn] " + game.id + (ok ? ": built into com.recomp.gcn" : ": SETUP FAILED"), background);
    if (ok)
    {
        // the decomp build replaces a recomp build of the same game (one at a time)
        std::error_code ec;
        const std::filesystem::path recomp(AddonDir() + "Source/Guest/" + game.name + "_recomp");
        if (std::filesystem::exists(recomp, ec))
        {
            std::filesystem::remove_all(recomp, ec);
            Emit("[gcn] " + game.id + ": removed its recomp build (Source/Guest/" + game.name + "_recomp)", background);
        }
    }
    return ok;
}

bool SetupGames(const std::string& decomp, bool background)
{
    bool ok = true;
    for (const GamePackage& game : FindGamePackages())
    {
        if (sCancel) break;
        ok = SetupGame(game, decomp, "", false, background) && ok;
    }
    return ok;
}

const char* kDoneHint = "Reload Native Addons so the editor compiles the translated games into com.recomp.gcn";

// The recomp build unpacks the disc into the project's Assets/Recomp/<name>/Disc while the
// editor runs: tell the asset manager about those files (raw assets), so packaging copies them
// without a project reload. Main thread only.
void RegisterRecompAssets()
{
    AssetManager* am = AssetManager::Get();
    if (am == nullptr) return;
    std::string project = Slashes(GetEngineState()->mProjectDirectory);
    if (!project.empty() && project.back() != '/') project += "/";
    for (const GamePackage& game : FindGamePackages())
    {
        if (!UseRecomp(game)) continue;
        const std::string rel = "Assets/Recomp/" + game.name + "/Disc";
        if (!sPackageDisc)
        {
            LogDebug("[gcn] %s: %s stays out of the package (Package the unpacked disc is off): the game asks the "
                     "player for their disc",
                     game.id.c_str(), rel.c_str());
            continue;
        }
        std::error_code ec;
        const std::filesystem::path root(project + rel);
        if (!std::filesystem::is_directory(root, ec)) continue;
        size_t n = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            RawAssetEntry entry;
            entry.mAbsolutePath = project + rel + "/" +
                                  Slashes(std::filesystem::relative(it->path(), root, ec).generic_string());
            entry.mEngineAsset = false;
            am->AddRawAssetEntry(entry);
            ++n;
        }
        LogDebug("[gcn] %s: %u disc files in %s are raw assets (packaged with the game)", game.id.c_str(), (unsigned)n,
                 rel.c_str());
    }
}

// runs `work` on the background thread (one run at a time)
template <typename Fn>
bool StartBackground(const std::string& id, Fn work)
{
    if (sRunning)
    {
        LogWarning("[gcn] a GameCube setup is already running");
        return false;
    }
    if (sThread.joinable()) sThread.join();
    {
        std::lock_guard<std::mutex> guard(sLock);
        sRecent.clear();
        sRunningId = id;
        sResultId = id;
        sResult = 0;
    }
    sCancel = false;
    sRunning = true;
    sRunStart = std::chrono::steady_clock::now();
    sThread = std::thread([work]() {
        const bool ok = work();
        {
            std::lock_guard<std::mutex> guard(sLock);
            sResult = sCancel ? 3 : ok ? 1 : 2;
        }
        Emit(sCancel ? std::string("[gcn] cancelled")
             : ok    ? std::string("[gcn] done. ") + kDoneHint
                     : std::string("[gcn] FAILED (see above)"),
             true);
        sRunning = false;
        sFinished = true;
    });
    return true;
}

// ---- disc image check -------------------------------------------------------------------------
struct DiscCheck
{
    bool ok = false;    // a GameCube disc of the right game
    bool wrong = false; // a GameCube disc, but another game or version
    std::string message;
};

DiscCheck CheckDisc(const std::string& pathIn, const GamePackage& game)
{
    DiscCheck c;
    const std::string path = Trim(pathIn);
    if (path.empty())
    {
        c.message = "Pick your disc image (.iso, .gcm or .nkit.iso).";
        return c;
    }
    std::ifstream f(path, std::ios::binary);
    unsigned char hdr[0x440] = {};
    if (!f || !f.read((char*)hdr, sizeof(hdr)))
    {
        c.message = "Cannot read " + path + ".";
        return c;
    }
    const uint32_t magic = (uint32_t(hdr[0x1C]) << 24) | (uint32_t(hdr[0x1D]) << 16) | (uint32_t(hdr[0x1E]) << 8) | hdr[0x1F];
    if (magic != 0xC2339F3Du)
    {
        c.message = "Not a GameCube disc image (compressed formats like .rvz/.ciso are not read: convert to .iso).";
        return c;
    }
    const std::string id((const char*)hdr, 6);
    std::string title((const char*)hdr + 0x20, strnlen((const char*)hdr + 0x20, 0x3E0));
    const int revision = hdr[7];
    char text[512];
    snprintf(text, sizeof(text), "%s (%s, revision %d)", title.c_str(), id.c_str(), revision);
    // gcn_game.json "version" is <game id>[_rev<n>], the disc the decomp matches
    const std::string wantId = game.version.substr(0, 6);
    int wantRev = 0;
    const size_t rev = game.version.find("_rev");
    if (rev != std::string::npos) wantRev = atoi(game.version.c_str() + rev + 4);
    if (!wantId.empty() && (id != wantId || revision != wantRev))
    {
        c.wrong = true;
        snprintf(text + strlen(text), sizeof(text) - strlen(text), ": the decomp needs %s revision %d.", wantId.c_str(),
                 wantRev);
        c.message = text;
        return c;
    }
    c.ok = true;
    c.message = std::string(text) + ": the right disc.";
    return c;
}

// ---- the Pre Process Rom window ----------------------------------------------------------------
struct PreprocessModal
{
    std::vector<GamePackage> games;
    int selected = 0;
    char disc[1024] = "";
    char decomp[1024] = "";
    std::string checkedDisc = "\x01";
    DiscCheck check;
    std::string checkedDecomp = "\x01";
    bool decompOk = false;
    GameStatus status;
    int statusGeneration = -1;
    bool restore = false;
    bool windowRegistered = false;
};

PreprocessModal sModal;
const char* kModalTitle = "Pre Process Rom: GameCube";

void LoadFields(PreprocessModal& m)
{
    if (m.games.empty()) return;
    const GamePackage& g = m.games[m.selected];
    snprintf(m.disc, sizeof(m.disc), "%s", g.disc.c_str());
    snprintf(m.decomp, sizeof(m.decomp), "%s", g.decomp.c_str());
    m.checkedDisc = "\x01";
    m.checkedDecomp = "\x01";
    m.statusGeneration = -1;
}

void WrappedText(const ImVec4& color, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void TextStatus(bool ok, const std::string& text)
{
    ImGui::TextColored(ok ? kGood : kWarn, "%s %s", ok ? "[x]" : "[ ]", text.c_str());
}

bool DrawPreprocessModal(void*)
{
    PreprocessModal& m = sModal;
    const bool running = sRunning;
    int result;
    std::string resultId;
    {
        std::lock_guard<std::mutex> guard(sLock);
        result = sResult;
        resultId = sResultId;
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640.0f);
    if (m.games.empty())
    {
        ImGui::TextWrapped("No GameCube game packages in this project (a package with Native/gcn_game.json, e.g. "
                           "com.recomp.starfoxadventures).");
        ImGui::PopTextWrapPos();
        return !ImGui::Button("Close", ImVec2(100.0f, 0.0f));
    }
    if (m.games.size() > 1)
    {
        ImGui::SetNextItemWidth(400.0f);
        if (ImGui::BeginCombo("Game", m.games[m.selected].title.c_str()))
        {
            for (int i = 0; i < (int)m.games.size(); ++i)
            {
                if (ImGui::Selectable(m.games[i].title.c_str(), i == m.selected) && i != m.selected)
                {
                    m.selected = i;
                    LoadFields(m);
                }
            }
            ImGui::EndCombo();
        }
    }
    const GamePackage& game = m.games[m.selected];
    const bool runningThis = running && sRunningId == game.id;
    if (m.statusGeneration != sStatusGeneration)
    {
        m.status = GetGameStatus(game);
        m.statusGeneration = sStatusGeneration;
    }

    if (game.hasRecomp)
    {
        static const char* const kModes[] = {"auto", "decomp", "recomp", "live"};
        static const char* const kModeNames[] = {"Auto (as last built)", "Decomp", "Recomp (Windows)",
                                                 "Recomp Live (Windows)"};
        int mode = sMode == "decomp" ? 1 : sMode == "recomp" ? 2 : sMode == "live" ? 3 : 0;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Build mode", &mode, kModeNames, 4))
        {
            sMode = kModes[mode];
            m.statusGeneration = -1;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Decomp: the decompilation compiled for every platform.\n"
                              "Recomp: the disc's own code recompiled ahead of time (Windows x64).\n"
                              "Recomp Live: recompiled from the disc when the game starts; the build holds no game\n"
                              "code and needs no disc (Windows x64).\n"
                              "Packaging uses Packaging > Target Options > GCN Recomp > Build mode.");
        }
    }
    if (UseLive(game))
    {
        ImGui::TextWrapped("%s Live: com.recomp.gcn gets the runtime and the game's symbols only, and recompiles the "
                           "game from the disc when it starts (no compiler, no game code in the build). The decomp "
                           "is still needed to build (the runtime's headers); a disc image here is unpacked into the "
                           "project's Assets/Recomp/%s/Disc for playing in the editor.",
                           game.title.c_str(), game.name.c_str());
    }
    else if (UseRecomp(game))
    {
        ImGui::TextWrapped("%s comes as code only: no game data. Point this at your own disc image (and the decomp, "
                           "whose headers the runtime builds against); Pre Process recompiles the disc's code into "
                           "com.recomp.gcn and unpacks the disc's files into the project's Assets/Recomp/%s/Disc, "
                           "where the game reads them and packaging takes them along.",
                           game.title.c_str(), game.name.c_str());
    }
    else
    {
        ImGui::TextWrapped("%s comes as code only: no game data. Point this at your own disc image and the decomp; "
                           "Pre Process translates the decomp into com.recomp.gcn (compiled when the editor loads its "
                           "addons) and unpacks the disc's files into Packages/%s/Assets/Disc, where the game reads "
                           "them and packaging takes them along (and where file mods go).",
                           game.title.c_str(), game.id.c_str());
    }
    ImGui::Spacing();

    // disc image
    ImGui::TextUnformatted("Disc image");
    ImGui::TextDisabled("%s, the version the decomp matches (%s)", game.title.c_str(), game.version.c_str());
    ImGui::SetNextItemWidth(520.0f);
    ImGui::InputText("##disc", m.disc, sizeof(m.disc));
    ImGui::SameLine();
    if (ImGui::Button("Browse...##disc") && sHooks != nullptr && sHooks->ShowOpenFileDialog != nullptr)
    {
        char picked[1024] = "";
        if (sHooks->ShowOpenFileDialog("Your disc image (.iso / .gcm / .nkit.iso)", "Disc image|*.iso;*.gcm", nullptr,
                                       picked, sizeof(picked)))
        {
            snprintf(m.disc, sizeof(m.disc), "%s", picked);
        }
    }
    if (m.checkedDisc != m.disc)
    {
        m.checkedDisc = m.disc;
        m.check = CheckDisc(m.disc, game);
    }
    WrappedText(m.check.ok ? kGood : kBad, m.check.message);

    // decomp
    ImGui::Spacing();
    ImGui::TextUnformatted("Decomp folder");
    ImGui::TextDisabled("The decompilation checkout (config/%s inside)", game.version.c_str());
    ImGui::SetNextItemWidth(520.0f);
    ImGui::InputText("##decomp", m.decomp, sizeof(m.decomp));
    ImGui::SameLine();
    if (ImGui::Button("Browse...##decomp") && sHooks != nullptr && sHooks->ShowSelectFolderDialog != nullptr)
    {
        char picked[1024] = "";
        if (sHooks->ShowSelectFolderDialog("The decomp folder", picked, sizeof(picked)))
        {
            snprintf(m.decomp, sizeof(m.decomp), "%s", picked);
        }
    }
    if (m.checkedDecomp != m.decomp)
    {
        m.checkedDecomp = m.decomp;
        const std::string dir = Trim(m.decomp);
        m.decompOk = !dir.empty() && Exists(dir + "/config/" + game.version + "/symbols.txt");
    }
    if (m.decompOk)
        ImGui::TextColored(kGood, "Found.");
    else
        ImGui::TextColored(kBad, "Not found (no config/%s/symbols.txt there).", game.version.c_str());
    ImGui::TextDisabled("Saved to Packages/%s/Native/local.json (not in git) when you pre-process.", game.id.c_str());

    // what is done already
    ImGui::Separator();
    ImGui::TextUnformatted("Status");
    TextStatus(m.status.translated, m.status.loaded ? "Game translated into com.recomp.gcn, loaded in this editor"
                                    : m.status.translated ? "Game translated into com.recomp.gcn (Reload Native Addons to load it)"
                                                          : "Game translated into com.recomp.gcn");
    TextStatus(m.status.unpacked, "Disc unpacked to Packages/" + game.id + "/Assets/Disc" +
                                      (m.status.unpackedId.empty() ? std::string() : " (" + m.status.unpackedId + ")"));
    if (!runningThis && resultId == game.id)
    {
        if (result == 1)
            WrappedText(kGood, std::string("Pre-processed. ") + kDoneHint + ".");
        else if (result == 2)
            WrappedText(kBad, "Pre-processing failed: see the output below and the log.");
        else if (result == 3)
            WrappedText(kWarn, "Cancelled.");
    }
    else if (!runningThis && m.status.loaded && m.status.unpacked)
    {
        WrappedText(kGood, "Ready. Pre Process again after changing the disc, the decomp, its patches or mods.");
    }

    // progress
    if (runningThis || (resultId == game.id && result != 0))
    {
        std::vector<std::string> tail;
        {
            std::lock_guard<std::mutex> guard(sLock);
            const size_t n = std::min<size_t>(sRecent.size(), 200);
            tail.assign(sRecent.end() - n, sRecent.end());
        }
        if (runningThis)
        {
            const auto secs =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - sRunStart).count();
            ImGui::Text("Pre-processing... %d:%02d (the first run takes several minutes)", int(secs / 60), int(secs % 60));
        }
        ImGui::BeginChild("##output", ImVec2(640.0f, 200.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& line : tail)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (runningThis) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::PopTextWrapPos();

    // buttons
    ImGui::Separator();
    ImGui::Checkbox("Restore the original disc files", &m.restore);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Unpacks every file of the disc again over Packages/%s/Assets/Disc,\n"
                          "replacing modded files. Without it, files already there are kept.",
                          game.id.c_str());
    }
    const bool canRun = !running && m.check.ok && m.decompOk;
    if (!canRun) ImGui::BeginDisabled();
    if (ImGui::Button(m.status.translated || m.status.unpacked ? "Pre Process again" : "Pre Process", ImVec2(160.0f, 0.0f)))
    {
        const std::string decomp = Trim(m.decomp), disc = Trim(m.disc);
        if (!WriteLocal(game, decomp, disc)) LogError("[gcn] cannot write %slocal.json", game.nativeDir.c_str());
        const GamePackage g = game;
        const bool restore = m.restore;
        StartBackground(game.id, [g, decomp, disc, restore]() { return SetupGame(g, decomp, disc, restore, true); });
        // the paths the build uses now
        m.games = FindGamePackages();
        if (m.selected >= (int)m.games.size()) m.selected = 0;
    }
    if (!canRun) ImGui::EndDisabled();
    if (runningThis)
    {
        ImGui::SameLine();
#if PLATFORM_WINDOWS
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) GcnDependencies::Cancel();
#else
        ImGui::TextDisabled("(running)");
#endif
    }
    if (running && !runningThis)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("A GameCube setup is running for another game.");
    }
    ImGui::SameLine();
    bool keepOpen = true;
    if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
    {
        keepOpen = false; // a running pre-process carries on; its result goes to the log
    }
    return keepOpen;
}
}

void GcnDependencies::OpenPreprocessWindow()
{
    PreprocessModal& m = sModal;
    const std::string current = m.games.empty() ? std::string() : m.games[m.selected].id;
    m.games = FindGamePackages();
    m.selected = 0;
    for (int i = 0; i < (int)m.games.size(); ++i)
    {
        if (m.games[i].id == current) m.selected = i;
    }
    LoadFields(m);
    if (sHooks == nullptr) return;
    if (sHooks->OpenModal != nullptr)
    {
        sHooks->OpenModal(sHookId, kModalTitle, DrawPreprocessModal, nullptr);
        return;
    }
    // older engines: a dockable window instead
    if (!m.windowRegistered && sHooks->RegisterWindow != nullptr)
    {
        sHooks->RegisterWindow(sHookId, kModalTitle, kModalTitle,
            [](void*) {
                if (!DrawPreprocessModal(nullptr) && sHooks != nullptr && sHooks->CloseWindow != nullptr)
                {
                    sHooks->CloseWindow(kModalTitle);
                }
            },
            nullptr);
        m.windowRegistered = true;
    }
    if (sHooks->OpenWindow != nullptr) sHooks->OpenWindow(kModalTitle);
}

void GcnDependencies::RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    sHookId = hookId;
    sModal.windowRegistered = false;
    if (hooks != nullptr && hooks->AddMenuItem != nullptr)
    {
        hooks->AddMenuItem(hookId, "Tools", "Recomp/GameCube/Pre Process Rom",
                           [](void*) { GcnDependencies::OpenPreprocessWindow(); }, nullptr, nullptr);
    }
}

void GcnDependencies::Cancel()
{
    if (!sRunning) return;
    sCancel = true;
#if PLATFORM_WINDOWS
    std::lock_guard<std::mutex> guard(sLock);
    if (sJob != nullptr) TerminateJobObject(sJob, 1);
#endif
}

bool GcnDependencies::SetupAll(const char* decompDir)
{
    if (sRunning)
    {
        LogWarning("[gcn] Setup Dependencies is still running in the background; packaging waits for it");
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    Tick();
    sCancel = false;
    const bool ok = SetupGames(decompDir ? decompDir : "", false);
    sStatusValid = false;
    ++sStatusGeneration;
    if (ok)
    {
        RegisterRecompAssets(); // before packaging copies the raw assets
    }
    return ok;
}

void GcnDependencies::SetupAllAsync(const char* decompDir)
{
    const std::string decomp = decompDir ? decompDir : "";
    StartBackground("", [decomp]() { return SetupGames(decomp, true); });
}

void GcnDependencies::Tick()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> guard(sLock);
        lines.swap(sPending);
    }
    for (const std::string& line : lines)
    {
        if (line.find("FAILED") != std::string::npos || line.find("error") != std::string::npos)
            LogError("%s", line.c_str());
        else
            LogDebug("%s", line.c_str());
    }
    if (sFinished.exchange(false))
    {
        sStatusValid = false;
        ++sStatusGeneration;
        if (sThread.joinable()) sThread.join();
        RegisterRecompAssets();
    }
}

void GcnDependencies::CheckReady()
{
    for (const GameStatus& game : GetStatus())
    {
        if (!game.translated)
        {
            LogWarning("[gcn] %s is not built yet: Tools > Recomp > GameCube > Pre Process Rom. It needs your own disc "
                       "image and the decomp, see the package's README",
                       game.id.c_str());
        }
        else if (!game.loaded)
        {
            LogWarning("[gcn] %s is translated but not in this build of com.recomp.gcn: Reload Native Addons",
                       game.id.c_str());
        }
        else if (!game.unpacked)
        {
            LogWarning("[gcn] %s: its disc is not unpacked into the package yet (packaged games need it): Tools > "
                       "Recomp > GameCube > Pre Process Rom",
                       game.id.c_str());
        }
    }
}

void GcnDependencies::SetBuildMode(const char* mode)
{
    const std::string m = mode ? mode : "";
    sMode = (m == "decomp" || m == "recomp" || m == "live") ? m : "auto";
    sStatusValid = false;
}

void GcnDependencies::SetPackageDisc(bool package)
{
    sPackageDisc = package;
}

void GcnDependencies::DrawTargetOptions(const PolyphaseBuildContext* ctx)
{
    char value[8] = "";
    const bool hasValue = ctx->GetProfileSetting != nullptr && ctx->GetProfileSetting(kSetupOption, value, sizeof(value)) != 0;
    bool setup = !hasValue || value[0] != '0';
    static char decomp[512];
    static bool decompLoaded = false;

    if (ImGui::Button("Pre Process Rom..."))
    {
        OpenPreprocessWindow();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Pick your disc image and the decomp, translate the game and unpack the disc\n"
                          "(also Tools > Recomp > GameCube > Pre Process Rom).");
    }
    if (ImGui::Checkbox("Setup Dependencies before packaging", &setup) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kSetupOption, setup ? "1" : "0");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Before packaging, builds each GameCube game package (Packages/<game>/Native) into\n"
                          "com.recomp.gcn: the decomp compiled to portable C, with its patches and mods, and its\n"
                          "disc unpacked into the package. Only what changed is redone. A failure cancels the packaging.");
    }
    if (!decompLoaded)
    {
        decomp[0] = 0;
        if (ctx->GetProfileSetting != nullptr) ctx->GetProfileSetting(kDecompOption, decomp, sizeof(decomp));
        char mode[16] = "";
        if (ctx->GetProfileSetting != nullptr && ctx->GetProfileSetting(kModeOption, mode, sizeof(mode))) SetBuildMode(mode);
        char pack[8] = "";
        if (ctx->GetProfileSetting != nullptr && ctx->GetProfileSetting(kPackageDiscOption, pack, sizeof(pack)))
            SetPackageDisc(pack[0] != '0');
        decompLoaded = true;
    }
    {
        static const char* const kModes[] = {"auto", "decomp", "recomp", "live"};
        static const char* const kModeNames[] = {"Auto", "Decomp", "Recomp (Windows)", "Recomp Live (Windows)"};
        int mode = sMode == "decomp" ? 1 : sMode == "recomp" ? 2 : sMode == "live" ? 3 : 0;
        if (ImGui::Combo("Build mode", &mode, kModeNames, 4))
        {
            SetBuildMode(kModes[mode]);
            if (ctx->SetProfileSetting != nullptr) ctx->SetProfileSetting(kModeOption, kModes[mode]);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Decomp: each game package's decompilation compiled to portable C (every platform).\n"
                              "Recomp: the game's own code from your disc recompiled ahead of time (Windows x64;\n"
                              "packages with a Recomp/ folder); its disc is unpacked into the project's\n"
                              "Assets/Recomp/<game>.\n"
                              "Recomp Live: no game code in the build; recompiled from the disc when the game starts.\n"
                              "Auto: as each game was last built, else its decomp.");
        }
        bool packDisc = sPackageDisc;
        if (ImGui::Checkbox("Package the unpacked disc", &packDisc))
        {
            SetPackageDisc(packDisc);
            if (ctx->SetProfileSetting != nullptr) ctx->SetProfileSetting(kPackageDiscOption, packDisc ? "1" : "0");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Recomp builds: the disc unpacked into Assets/Recomp/<game>/Disc goes into the package.\n"
                              "Off (with Recomp Live: a build without any game data): the packaged game asks the\n"
                              "player for their own disc in a launcher scene (Tools > Recomp > Mods > Launcher).");
        }
    }
    if (ImGui::InputText("Decomp folder", decomp, sizeof(decomp)) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kDecompOption, decomp);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Overrides the decomp for every GameCube game package when packaging. Leave empty for\n"
                          "each package's own setting (Pre Process Rom, or gcn_game.json \"decomp\").");
    }
    const bool running = sRunning;
    if (running) ImGui::BeginDisabled();
    if (ImGui::Button("Setup Dependencies Now"))
    {
        SetupAllAsync(decomp);
    }
    if (running)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("running, see the log...");
    }
    if (!sStatusValid && !running)
    {
        sStatus = GetStatus();
        sStatusValid = true;
    }
    for (const GameStatus& game : sStatus)
    {
        if (game.translated && game.loaded && (game.unpacked || game.live))
            ImGui::Text("%s: ready (%s)", game.id.c_str(), game.live ? "recomp live" : game.recomp ? "recomp" : "decomp");
        else
            ImGui::TextColored(kWarn, "%s: %s", game.id.c_str(),
                               !game.translated ? "needs Pre Process Rom"
                               : !game.loaded   ? "translated, Reload Native Addons"
                                                : "disc not unpacked yet: Pre Process Rom");
    }
    if (sStatus.empty())
    {
        ImGui::TextUnformatted("No GameCube game packages in this project.");
    }
}

#else

// Building games needs the editor on Windows or Linux.
bool GcnDependencies::SetupAll(const char*)
{
    return true;
}
void GcnDependencies::SetupAllAsync(const char*)
{
}
void GcnDependencies::Tick()
{
}
void GcnDependencies::CheckReady()
{
}
void GcnDependencies::DrawTargetOptions(const PolyphaseBuildContext*)
{
}
void GcnDependencies::OpenPreprocessWindow()
{
}
void GcnDependencies::RegisterEditorUI(EditorUIHooks*, uint64_t)
{
}
void GcnDependencies::Cancel()
{
}
void GcnDependencies::SetBuildMode(const char*)
{
}
void GcnDependencies::SetPackageDisc(bool)
{
}

#endif
