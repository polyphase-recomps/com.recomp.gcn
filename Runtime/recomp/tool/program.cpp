/**
 * @file program.cpp
 * @brief Loading the DOL and the symbol file (see program.h).
 */
#include "program.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace gcnr
{
namespace
{
uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t parse_u32(const std::string& s)
{
    return (uint32_t)std::stoul(s, nullptr, 0);
}
} // namespace

std::string Function::c_name() const
{
    if (hle)
    {
        return "hle_" + name;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "f_%08X", addr);
    return buf;
}

bool Program::load_dol(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load_dol_data(std::move(data), path, error);
}

bool Program::load_dol_data(std::vector<uint8_t> data, const std::string& path, std::string& error)
{
    dol = std::move(data);
    sections.clear();
    if (dol.size() < 0x100)
    {
        error = path + " is not a DOL";
        return false;
    }
    for (int i = 0; i < 18; i++)
    {
        Section s;
        s.offset = be32(&dol[i * 4]);
        s.addr = be32(&dol[0x48 + i * 4]);
        s.size = be32(&dol[0x90 + i * 4]);
        s.text = i < 7;
        if (s.size == 0)
        {
            continue;
        }
        if ((uint64_t)s.offset + s.size > dol.size())
        {
            error = path + ": section past the end of the file";
            return false;
        }
        sections.push_back(s);
    }
    entry = be32(&dol[0xE0]);
    return true;
}

bool Program::load_symbols(const std::string& path, std::string& error)
{
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    return load_symbols_from(in, path, error);
}

bool Program::load_symbols_text(const std::string& text, const std::string& path, std::string& error)
{
    std::istringstream in(text);
    return load_symbols_from(in, path, error);
}

bool Program::load_symbols_from(std::istream& in, const std::string& path, std::string& error)
{
    std::set<std::string> hleNames;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line))
    {
        lineNo++;
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        try
        {
            if (kind == "dol_sha1")
            {
                ss >> dolSha1;
            }
            else if (kind == "entry" || kind == "r1" || kind == "r2" || kind == "r13" || kind == "ctors")
            {
                std::string v;
                ss >> v;
                uint32_t& field = kind == "entry" ? entry : kind == "r1" ? r1 : kind == "r2" ? r2 : kind == "r13" ? r13 : ctors;
                field = parse_u32(v);
            }
            else if (kind == "text")
            {
                // the DOL header is authoritative; nothing to do
            }
            else if (kind == "func")
            {
                std::string a, s, name;
                ss >> a >> s >> name;
                Function f;
                f.addr = parse_u32(a);
                f.end = f.addr + parse_u32(s);
                f.name = name;
                functions[f.addr] = f;
            }
            else if (kind == "label")
            {
                std::string a, name;
                ss >> a >> name;
                labels.emplace_back(parse_u32(a), name);
            }
            else if (kind == "jumptable")
            {
                std::string a, s;
                ss >> a >> s;
                jumpTables[parse_u32(a)] = parse_u32(s);
            }
            else if (kind == "hle")
            {
                std::string name;
                ss >> name;
                hleNames.insert(name);
            }
        }
        catch (...)
        {
            error = path + ":" + std::to_string(lineNo) + ": bad line: " + line;
            return false;
        }
    }
    for (auto& [addr, f] : functions)
    {
        f.hle = hleNames.count(f.name) != 0;
    }
    return true;
}

bool Program::in_text(uint32_t addr) const
{
    for (const Section& s : sections)
    {
        if (s.text && addr >= s.addr && addr - s.addr < s.size)
        {
            return true;
        }
    }
    return false;
}

bool Program::read32(uint32_t addr, uint32_t& out) const
{
    for (const Section& s : sections)
    {
        if (addr >= s.addr && addr - s.addr + 4 <= s.size)
        {
            out = be32(&dol[s.offset + (addr - s.addr)]);
            return true;
        }
    }
    return false;
}

const Function* Program::function_at(uint32_t addr) const
{
    auto it = functions.find(addr);
    return it == functions.end() ? nullptr : &it->second;
}

const Function* Program::function_containing(uint32_t addr) const
{
    auto it = functions.upper_bound(addr);
    while (it != functions.begin())
    {
        --it;
        if (!it->second.extra)
        {
            return addr < it->second.end ? &it->second : nullptr;
        }
    }
    return nullptr;
}
} // namespace gcnr
