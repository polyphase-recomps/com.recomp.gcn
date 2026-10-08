/**
 * @file program.h
 * @brief The program being recompiled: the DOL's sections, the functions (from gcn_syms.py's symbol
 *        file plus extra entry points found by the analysis) and the per-game facts.
 */
#pragma once

#include <cstdint>
#include <istream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gcnr
{
struct Section
{
    uint32_t addr = 0, size = 0, offset = 0;
    bool text = false;
};

struct Function
{
    uint32_t addr = 0;
    uint32_t end = 0; // one past the last instruction
    std::string name; // symbol name ("" for extra entries)
    bool hle = false; // supplied by the runtime: never recompiled, calls go to hle_<name>
    bool extra = false; // an entry point inside another function (runs to that function's end)
    bool stub = false;  // `stub` line: returns stubValue in r3 instead of running (hardware the runtime lacks)
    uint32_t stubValue = 0;

    std::string c_name() const; // f_80003140 / hle_OSReport
};

struct Program
{
    std::vector<uint8_t> dol;
    std::vector<Section> sections;
    std::string dolSha1; // from the symbol file (checked by the build script)
    uint32_t entry = 0, r1 = 0, r2 = 0, r13 = 0, ctors = 0;
    std::map<uint32_t, Function> functions; // by address
    std::map<uint32_t, uint32_t> jumpTables; // address -> size in bytes
    std::vector<std::pair<uint32_t, std::string>> labels;

    bool load_dol(const std::string& path, std::string& error);
    bool load_symbols(const std::string& path, std::string& error);
    // the same from memory (Live builds: the DOL read from the disc, the symbols built in);
    // `path` only names them in errors
    bool load_dol_data(std::vector<uint8_t> data, const std::string& path, std::string& error);
    bool load_symbols_text(const std::string& text, const std::string& path, std::string& error);

    bool in_text(uint32_t addr) const;
    bool read32(uint32_t addr, uint32_t& out) const; // big-endian word from any DOL section
    const Function* function_at(uint32_t addr) const;       // the function starting there
    const Function* function_containing(uint32_t addr) const; // the (non-extra) function covering it

private:
    bool load_symbols_from(std::istream& in, const std::string& path, std::string& error);
};
} // namespace gcnr
