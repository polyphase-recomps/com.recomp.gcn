/**
 * @file GcnLauncher.cpp
 * @brief GameCube games as com.recomp.mod.base launchers (see GcnLauncher.h).
 */

#include "GcnLauncher.h"

#include "Engine.h"
#include "Log.h"

#include "GcnPlayer.h"

extern "C" {
#include "Gcn/gcn_disc.h"
#include "Gcn/gcnw_module.h"
}

#include "ModBaseLauncher.h"
#include "ModBaseUtil.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
std::string ProjectPath(const std::string& relative)
{
    return GetEngineState()->mProjectDirectory + relative;
}

bool IsAbsolute(const std::string& path)
{
    return path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
}

bool FileExists(const std::string& path)
{
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        fclose(f);
        return true;
    }
    return false;
}

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
    {
        char a = s[s.size() - n + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

uint32_t Be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// The game's build name: a recomp build's module is "<name>recomp" (build_recomp.ps1)
std::string BaseName(const GcnwModule* module)
{
    std::string name = module != nullptr && module->name != nullptr ? module->name : "";
    if (name.size() > 6 && name.compare(name.size() - 6, 6, "recomp") == 0)
    {
        name.resize(name.size() - 6);
    }
    return name;
}

// ---- reading a disc: an image file, or a disc gcn_disc.py unpacked (disc.idx + sys/) ----------
class Disc
{
public:
    ~Disc()
    {
        if (mImage != nullptr) gcn_disc_close(mImage);
    }

    bool Open(const std::string& path)
    {
        mUnpacked = FileExists(path + "/disc.idx");
        if (mUnpacked)
        {
            mPath = path;
            return Read(0, mHeader, sizeof(mHeader));
        }
        // images through the runtime's reader (.iso, .gcm, .nkit.iso, .ciso)
        mImage = FileExists(path) ? gcn_disc_open(path.c_str()) : nullptr;
        return mImage != nullptr && Read(0, mHeader, sizeof(mHeader));
    }

    bool IsGameCube() const { return Be32(mHeader + 0x1C) == 0xC2339F3Du; }
    std::string GameId() const
    {
        std::string id(reinterpret_cast<const char*>(mHeader), 6);
        for (char& c : id)
        {
            if (c < 32 || c > 126) c = '?';
        }
        return id;
    }
    int Revision() const { return mHeader[7]; }
    std::string Title() const
    {
        const char* t = reinterpret_cast<const char*>(mHeader + 0x20);
        size_t n = 0;
        while (n < 0x3E0 - 0x20 && t[n] != 0) n++;
        return std::string(t, n);
    }

    // main.dol: its sections' extent from the DOL header, as gcn_disc.py takes it
    bool ReadDol(std::vector<uint8_t>& out)
    {
        if (mUnpacked)
        {
            std::ifstream file(mPath + "/sys/main.dol", std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return out.size() >= 0x100;
        }
        const uint32_t offset = Be32(mHeader + 0x420);
        uint8_t head[0x100];
        if (offset == 0 || !Read(offset, head, sizeof(head))) return false;
        uint32_t end = 0;
        for (int i = 0; i < 18; i++)
        {
            const uint32_t off = Be32(head + i * 4), size = Be32(head + 0x90 + i * 4);
            if (size != 0) end = std::max(end, off + size);
        }
        if (end < 0x100 || end > (64u << 20)) return false;
        out.resize(end);
        return Read(offset, out.data(), end);
    }

private:
    bool Read(uint64_t offset, uint8_t* out, size_t size)
    {
        if (mUnpacked)
        {
            // the header and the executable are files of their own
            std::ifstream file(mPath + "/sys/head.bin", std::ios::binary);
            file.seekg(std::streamoff(offset));
            file.read(reinterpret_cast<char*>(out), std::streamsize(size));
            return size_t(file.gcount()) == size;
        }
        return mImage != nullptr && gcn_disc_read(mImage, out, offset, (uint32_t)size) == size;
    }

    bool mUnpacked = false;
    std::string mPath;
    GcnDisc* mImage = nullptr;
    uint8_t mHeader[0x440] = {};
};

// What a game expects of its disc ("" / -1 where it doesn't care)
struct DiscFacts
{
    std::string title;
    std::string id;
    int revision = -1;
    std::string dolSha1;
};

DiscFacts FactsFor(const std::string& package)
{
    const GcnwModule* module = gcnw_find_module(package.c_str());
    DiscFacts facts;
    facts.title = module != nullptr && module->title != nullptr ? module->title : package;
    // a recomp or live build (module "<name>recomp"): what build_recomp.ps1 recompiled (project
    // assets); a decomp build: the game package's own game.json
    const bool recomp = module != nullptr && BaseName(module) != module->name;
    std::string json = recomp ? RecompUtil::ReadText(ProjectPath("Assets/Recomp/" + BaseName(module) + "/game.json"))
                              : std::string();
    if (json.empty())
    {
        json = RecompUtil::ReadText(ProjectPath("Packages/" + package + "/Assets/game.json"));
    }
    facts.id = RecompUtil::JsonString(json, "disc_id");
    facts.revision = (int)RecompUtil::JsonNumber(json, "disc_revision", -1);
    facts.dolSha1 = RecompUtil::JsonString(json, "dol_sha1");
    return facts;
}

std::string SavePath(const std::string& package)
{
    return ProjectPath("Saves/" + package + ".disc.txt");
}

// ---- one game ------------------------------------------------------------------------------
class GcnGameLauncher : public RecompGameLauncher
{
public:
    explicit GcnGameLauncher(const GcnwModule* module) : mModule(module) {}

    const char* RuntimeId() const override { return "gcn"; }
    std::string GamePackage() const override { return mModule->package; }
    std::string GameTitle() const override { return mModule->title != nullptr ? mModule->title : mModule->package; }

    bool CheckRom(const std::string& path, std::string& message) override
    {
        std::string resolved;
        return GcnLauncher::CheckDisc(GamePackage(), path, message, resolved);
    }

    bool SetRomLocation(const std::string& path, std::string& message) override
    {
        std::string resolved;
        if (!GcnLauncher::CheckDisc(GamePackage(), path, message, resolved)) return false;
        std::ofstream file(SavePath(GamePackage()), std::ios::binary | std::ios::trunc);
        file << resolved;
        if (!file.good())
        {
            message = "Could not remember the disc (cannot write " + SavePath(GamePackage()) + ")";
            return false;
        }
        return true;
    }

    std::string GetRomLocation() override { return GcnLauncher::ChosenDisc(GamePackage()); }

    void ClearRomLocation() override { remove(SavePath(GamePackage()).c_str()); }

    // a disc unpacked into the project (recomp builds) or into the game package (decomp builds)
    bool HasShippedData() override
    {
        return FileExists(ProjectPath("Assets/Recomp/" + BaseName(mModule) + "/Disc/disc.idx")) ||
               FileExists(ProjectPath("Packages/" + GamePackage() + "/Assets/Disc/disc.idx"));
    }

    bool StartGame(std::string& message) override
    {
        const std::string chosen = GetRomLocation();
        if (!chosen.empty())
        {
            std::string resolved;
            if (!GcnLauncher::CheckDisc(GamePackage(), chosen, message, resolved))
            {
                mMessage = message;
                return false;
            }
        }
        else if (!HasShippedData())
        {
            message = mMessage = "Choose your " + GameTitle() + " disc image first (.iso, .gcm, .nkit.iso or .ciso)";
            return false;
        }
        mStarted = true;
        GcnPlayer::RestartGame(GamePackage());
        message = mMessage = "Starting " + GameTitle();
        return true;
    }

    bool IsStarted() override { return mStarted; }
    std::string LastMessage() override { return mMessage; }

private:
    const GcnwModule* mModule;
    bool mStarted = false;
    std::string mMessage;
};

std::vector<std::unique_ptr<GcnGameLauncher>> sLaunchers;
} // namespace

void GcnLauncher::RegisterAll()
{
    UnregisterAll();
    for (int i = 0; i < gcnw_module_count(); i++)
    {
        const GcnwModule* module = gcnw_module_at(i);
        if (module == nullptr || module->package == nullptr) continue;
        sLaunchers.push_back(std::make_unique<GcnGameLauncher>(module));
        Recomp_RegisterLauncher(sLaunchers.back().get());
    }
}

void GcnLauncher::UnregisterAll()
{
    for (auto& launcher : sLaunchers)
    {
        Recomp_UnregisterLauncher(launcher.get());
    }
    sLaunchers.clear();
}

std::string GcnLauncher::ChosenDisc(const std::string& package)
{
    std::string text = RecompUtil::ReadText(SavePath(package));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

bool GcnLauncher::CheckDisc(const std::string& package, const std::string& path, std::string& message,
                            std::string& resolved)
{
    const DiscFacts facts = FactsFor(package);
    resolved = IsAbsolute(path) ? path : ProjectPath(path);
    while (!resolved.empty() && (resolved.back() == '/' || resolved.back() == '\\')) resolved.pop_back();
    const std::string file = RecompUtil::FileName(resolved);

    for (const char* packed : {".rvz", ".wia", ".wbfs", ".gcz", ".tgc"})
    {
        if (EndsWithNoCase(resolved, packed))
        {
            message = file + " is a compressed image: convert it to .iso first (Dolphin: right-click the game, "
                             "Convert File..., format ISO)";
            return false;
        }
    }
    Disc disc;
    if (!disc.Open(resolved))
    {
        message = FileExists(resolved) ? file + " cannot be read" : "File not found: " + resolved;
        return false;
    }
    if (!disc.IsGameCube())
    {
        message = file + " is not a GameCube disc image (.iso, .gcm, .nkit.iso or .ciso)";
        return false;
    }
    const std::string id = disc.GameId();
    const std::string what = disc.Title() + " (" + id + " rev " + std::to_string(disc.Revision()) + ")";
    if (!facts.id.empty() && id != facts.id)
    {
        message = file + " is " + what + ", not " + facts.title + " (" + facts.id + ")";
        return false;
    }
    if (facts.revision >= 0 && disc.Revision() != facts.revision)
    {
        message = file + " is " + what + ": this build needs revision " + std::to_string(facts.revision);
        return false;
    }
    if (!facts.dolSha1.empty())
    {
        std::vector<uint8_t> dol;
        const std::string sha1 = disc.ReadDol(dol) ? RecompUtil::Sha1Hex(dol) : std::string();
        if (sha1 != facts.dolSha1)
        {
            message = file + " is " + what + ", but its main.dol differs (sha1 " + (sha1.empty() ? "unreadable" : sha1) +
                      "): this build was made from " + facts.dolSha1;
            return false;
        }
    }
    message = what;
    return true;
}
