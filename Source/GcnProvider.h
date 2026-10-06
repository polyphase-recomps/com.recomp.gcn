/**
 * @file GcnProvider.h
 * @brief The GameCube runtime's RecompProvider (com.recomp.mod.base): mod settings, the
 *        Recomp/Mods Lua tables, Recomp* widgets and the editor's Mods windows work on
 *        the game GcnGuestHost runs through it.
 *
 * Variables are the mods' published variables (gcn_mod_variable) and, by name, any
 * global of the decomp (gcnw_find_symbol): game memory sits at the console's own
 * addresses, big-endian, so RAM maps and cheat addresses work as they are.
 */
#pragma once

#include "ModBaseProvider.h"

class GcnProvider : public RecompProvider
{
public:
    static GcnProvider& Get();

    void SetFrame(int width, int height);

    const char* RuntimeId() const override { return "gcn"; }
    std::string GamePackage() const override;
    bool IsLive() const override;
    void Variables(std::vector<RecompVarInfo>& out) const override;
    void Requests(std::vector<RecompRequestInfo>& out) const override;
    bool Get(const std::string& name, int index, RecompValue& out) override;
    bool Set(const std::string& name, int index, const RecompValue& value) override;
    int Request(const std::string& name, const std::vector<int>& args) override;
    bool Result(int id, int& result) override;
    bool ReadAddress(uint64_t address, RecompType type, RecompValue& out) override;
    bool WriteAddress(uint64_t address, RecompType type, const RecompValue& value) override;
    bool ResolveSymbol(const std::string& name, uint64_t& address) override;
    RecompFrameInfo FrameInfo() const override;

private:
    int mWidth = 0;
    int mHeight = 0;
};
