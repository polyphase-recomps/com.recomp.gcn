/**
 * @file recomp_live.cpp
 * @brief The runtime side of a Live build (see live.h): recompiles the game from the disc the
 *        first time recomp_gcn.c needs a function, and provides the tables an AOT build's
 *        generated functable.c and game.c would (gcnr_functions, gcnr_r2, ...).
 *
 * The symbols (syms.txt: names, sizes, switch tables, HLE list - no game code) are built into the
 * library (<name>_live_syms.c, gcn_live_syms.py); main.dol comes from the disc the game runs
 * from, and must be the one they describe (its SHA-1), or the game does not start.
 *
 * REL modules (games that load code at run time: syms.txt `module` / `mfunc` lines): the game's
 * own OSLink / OSLinkFixed / OSUnlink run as usual, wrapped. Before the link the wrapper notes the
 * module's relocations against other modules (OSLink rewrites those instructions whenever such a
 * module links or unlinks, so they are decoded when they run); after it, the module's code is
 * recompiled at the address the game linked it to and joins the lookup table; OSUnlink takes it
 * out again.
 */
#include "live.h"

#include "../tool/analysis.h"

extern "C" {
#include "gcn_platform.h"
}

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
// the AOT build's generated tables, filled here (recomp_gcn.c reads them)
struct gcnr_function
{
    uint32_t addr;
    gcnr_func fn;
};
const gcnr_function* gcnr_functions = nullptr;
uint32_t gcnr_function_count = 0;
uint32_t gcnr_entry = 0, gcnr_r1 = 0, gcnr_r2 = 0, gcnr_r13 = 0, gcnr_ctors = 0;
char gcnr_dol_sha1[48] = "";

// built into the library: the symbol file (gcn_live_syms.py) and the HLE wrappers (gen_hle_glue.py)
extern const unsigned char gcnr_live_syms[];
extern const unsigned int gcnr_live_syms_size;
extern const gcnr_named_func gcnr_hle_functions[];

void gcnr_live_ensure(void);
void gcnr_lookup_reset(void);
}

namespace
{
using Clock = std::chrono::steady_clock;

void logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gcnp_log(buf);
}

[[noreturn]] void fail(const std::string& why)
{
    gcnp_log(("live: the game cannot start: " + why).c_str());
    gcnp_crashed();
    for (;;)
    {
    }
}

uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

std::string hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

std::string sha1_hex(const std::vector<uint8_t>& data)
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    const size_t full = data.size() / 64 * 64;
    for (size_t i = 0; i < full; i += 64) block(data.data() + i);
    uint8_t tail[128] = {0};
    const size_t rest = data.size() - full;
    if (rest) std::memcpy(tail, data.data() + full, rest);
    tail[rest] = 0x80;
    const size_t tailSize = rest + 9 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)data.size() * 8;
    for (int i = 0; i < 8; i++) tail[tailSize - 1 - i] = (uint8_t)(bits >> (i * 8));
    block(tail);
    if (tailSize == 128) block(tail + 64);
    char out[41];
    for (int i = 0; i < 5; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

// main.dol from the disc, as gcn_disc.py extracts it (its sections' extent)
std::vector<uint8_t> read_dol()
{
    uint8_t dir[4], hdr[0x100];
    if (gcnp_disc_read(dir, 0x420, 4) != 4 || gcnp_disc_read(hdr, be32(dir), sizeof(hdr)) != sizeof(hdr))
    {
        fail("cannot read main.dol from the disc");
    }
    uint32_t end = 0;
    for (int i = 0; i < 18; i++)
    {
        const uint32_t off = be32(hdr + i * 4), size = be32(hdr + 0x90 + i * 4);
        if (size && off + size > end) end = off + size;
    }
    if (end < 0x100 || end > (64u << 20)) fail("the disc's main.dol header is not valid");
    std::vector<uint8_t> dol(end);
    if (gcnp_disc_read(dol.data(), be32(dir), end) != end) fail("short read of main.dol");
    return dol;
}

// ---- the lookup table: the DOL's functions and those of the modules linked now -------------------
std::vector<gcnr_function> sDolTable;
std::vector<gcnr_function> sTable;
uint32_t sR2 = 0, sR13 = 0;
std::shared_ptr<void> sDolCode;

// ---- modules ------------------------------------------------------------------------------------
struct ModuleSymbol
{
    std::string section;
    uint32_t offset = 0, size = 0;
    std::string name;
};

struct ModuleSyms
{
    std::string name;
    std::vector<std::string> sections; // dtk's names of the REL's non-empty sections, in order
    std::vector<ModuleSymbol> funcs, labels, tables;
};

std::map<uint32_t, ModuleSyms> sModuleSyms; // by module id

// a relocation against another module: (that module's id, section, offset)
struct Reloc
{
    uint32_t module, section, offset;
};

struct LoadedModule
{
    uint32_t id = 0;
    std::string name;
    std::vector<gcnr_function> functions;
    std::shared_ptr<void> code;
    std::vector<Reloc> relocs; // against modules other than the DOL and itself (decoded when they run)
};

std::map<uint32_t, LoadedModule> sModules;   // by the module's header address
std::map<uint32_t, std::vector<Reloc>> sPending; // header -> relocations, noted before the link
std::map<uint32_t, gcnr_func> sOriginal;                         // hooked function -> its recompiled code
uint32_t sOSLink = 0, sOSLinkFixed = 0, sOSUnlink = 0;

void parse_module_syms(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.size() < 6 || line[0] != 'm') continue;
        std::istringstream ss(line);
        std::string kind;
        uint32_t id = 0;
        ss >> kind >> id;
        if (kind == "module")
        {
            ModuleSyms& m = sModuleSyms[id];
            ss >> m.name;
            std::string s;
            while (ss >> s) m.sections.push_back(s);
            continue;
        }
        ModuleSymbol sym;
        std::string off, size;
        if (kind == "mfunc" || kind == "mtable")
        {
            ss >> sym.section >> off >> size >> sym.name;
            sym.offset = (uint32_t)std::stoul(off, nullptr, 0);
            sym.size = (uint32_t)std::stoul(size, nullptr, 0);
            (kind == "mfunc" ? sModuleSyms[id].funcs : sModuleSyms[id].tables).push_back(sym);
        }
        else if (kind == "mlabel")
        {
            ss >> sym.section >> off >> sym.name;
            sym.offset = (uint32_t)std::stoul(off, nullptr, 0);
            sModuleSyms[id].labels.push_back(sym);
        }
    }
}

void rebuild_table()
{
    sTable = sDolTable;
    for (const auto& [hdr, m] : sModules)
    {
        sTable.insert(sTable.end(), m.functions.begin(), m.functions.end());
    }
    std::sort(sTable.begin(), sTable.end(), [](const gcnr_function& a, const gcnr_function& b) { return a.addr < b.addr; });
    gcnr_functions = sTable.data();
    gcnr_function_count = (uint32_t)sTable.size();
    gcnr_lookup_reset();
}

gcnr_func find_exact(uint32_t addr)
{
    auto it = std::lower_bound(sTable.begin(), sTable.end(), addr,
                               [](const gcnr_function& f, uint32_t a) { return f.addr < a; });
    return it != sTable.end() && it->addr == addr ? it->fn : nullptr;
}

uint8_t* guest(uint8_t* mem, uint32_t a)
{
    return GCNR_PTR(mem, a);
}

// before OSLink: the module's relocations against modules other than the DOL and itself (their
// instructions change when those modules link / unlink). The header is unlinked: offsets.
void note_relocations(uint8_t* mem, uint32_t m)
{
    std::vector<Reloc> sites;
    const uint32_t id = gcnr_lw(mem, m), imp = gcnr_lw(mem, m + 0x28), impSize = gcnr_lw(mem, m + 0x2C);
    for (uint32_t k = 0; k + 8 <= impSize && k < 0x10000; k += 8)
    {
        const uint32_t mod = gcnr_lw(mem, m + imp + k), rel = gcnr_lw(mem, m + imp + k + 4);
        if (mod == 0 || mod == id) continue;
        uint32_t p = m + rel, sec = 0, off = 0;
        for (int guard = 0; guard < 4000000; guard++, p += 8)
        {
            const uint32_t delta = gcnr_lhz(mem, p), type = gcnr_lbz(mem, p + 2), s = gcnr_lbz(mem, p + 3);
            if (type == 203) break;                         // R_DOLPHIN_END
            if (type == 202) { sec = s; off = 0; continue; } // R_DOLPHIN_SECTION
            off += delta;
            if (type == 201 || type == 0) continue;          // R_DOLPHIN_NOP, R_PPC_NONE
            sites.push_back({mod, sec, off & ~3u});
        }
    }
    sPending[m] = std::move(sites);
}

// after OSLink: recompile the module's code where it now is. Its relocations against other
// modules change whenever those link or unlink: those instructions are decoded when they run.
void compile_module(uint8_t* mem, uint32_t m, std::vector<Reloc> relocs)
{
    const auto t0 = Clock::now();
    const uint32_t id = gcnr_lw(mem, m), nsec = gcnr_lw(mem, m + 0xC), secTable = gcnr_lw(mem, m + 0x10);
    auto syms = sModuleSyms.find(id);
    if (syms == sModuleSyms.end())
    {
        logf("live: module %u linked at %08X has no symbols (syms.txt `module` lines): its code cannot run", id, m);
        return;
    }
    const ModuleSyms& ms = syms->second;
    gcnr::Program p;
    p.r2 = sR2;
    p.r13 = sR13;
    std::map<std::string, uint32_t> base; // dtk section name -> address
    std::vector<uint32_t> secBase(nsec, 0);
    size_t named = 0;
    for (uint32_t i = 0; i < nsec && i < 64; i++)
    {
        const uint32_t off = gcnr_lw(mem, secTable + 8 * i), size = gcnr_lw(mem, secTable + 8 * i + 4);
        if (size == 0) continue;
        const uint32_t addr = off & ~1u;
        secBase[i] = addr;
        if (named < ms.sections.size()) base[ms.sections[named]] = addr;
        named++;
        if (addr == 0) continue; // bss before the game placed it
        gcnr::Section s;
        s.addr = addr;
        s.size = size;
        s.offset = (uint32_t)p.dol.size();
        s.text = (off & 1) != 0;
        p.dol.insert(p.dol.end(), guest(mem, addr), guest(mem, addr) + size);
        p.sections.push_back(s);
    }
    for (const ModuleSymbol& f : ms.funcs)
    {
        auto b = base.find(f.section);
        if (b == base.end()) continue;
        gcnr::Function fn;
        fn.addr = b->second + f.offset;
        fn.end = fn.addr + f.size;
        fn.name = f.name;
        p.functions[fn.addr] = fn;
    }
    for (const ModuleSymbol& l : ms.labels)
    {
        auto b = base.find(l.section);
        if (b != base.end()) p.labels.emplace_back(b->second + l.offset, l.name);
    }
    for (const ModuleSymbol& t : ms.tables)
    {
        auto b = base.find(t.section);
        if (b != base.end()) p.jumpTables[b->second + t.offset] = t.size;
    }
    std::vector<std::string> warnings;
    const int extras = gcnr::find_extra_entries(p, warnings);

    gcnr::LiveInputs in;
    in.external = [](uint32_t addr) { return find_exact(addr); };
    // all of them, also against modules linked now: such a module may unlink and link again
    // elsewhere while this one stays, and recompiling this one then could free code that is
    // running (OSUnlink is usually called from module code)
    for (const Reloc& r : relocs)
    {
        if (r.section < secBase.size() && secBase[r.section]) in.dynamic.insert(secBase[r.section] + r.offset);
    }
    sModules.erase(m); // a recompile replaces it
    gcnr::LiveResult r = gcnr::live_recompile(p, in);
    if (!r.error.empty())
    {
        logf("live: module %s (%u) at %08X: %s", ms.name.c_str(), id, m, r.error.c_str());
        return;
    }
    LoadedModule lm;
    lm.id = id;
    lm.name = ms.name;
    lm.code = r.owner;
    lm.relocs = std::move(relocs);
    for (const gcnr::LiveFunction& f : r.functions) lm.functions.push_back({f.addr, f.fn});
    sModules[m] = std::move(lm);
    rebuild_table();
    const double ms_ = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    logf("live: module %s (%u) linked at %08X: %d functions (%d extra entry points, %zu instructions, %zu rewritten by "
         "other modules' links) in %.0f ms",
         ms.name.c_str(), id, m, r.recompiled, extras, r.instructions, in.dynamic.size(), ms_);
    warnings.insert(warnings.end(), r.warnings.begin(), r.warnings.end());
    for (size_t i = 0; i < warnings.size() && i < 8; i++) logf("live: %s: %s", ms.name.c_str(), warnings[i].c_str());
}

void wrap_link(uint8_t* mem, gcnr_ctx* c, uint32_t which)
{
    const uint32_t m = c->r[3];
    note_relocations(mem, m);
    sOriginal[which](mem, c);
    if (c->r[3]) compile_module(mem, m, std::move(sPending[m]));
    sPending.erase(m);
}

void wrap_OSLink(uint8_t* mem, gcnr_ctx* c) { wrap_link(mem, c, sOSLink); }
void wrap_OSLinkFixed(uint8_t* mem, gcnr_ctx* c) { wrap_link(mem, c, sOSLinkFixed); }

void wrap_OSUnlink(uint8_t* mem, gcnr_ctx* c)
{
    const uint32_t m = c->r[3];
    sOriginal[sOSUnlink](mem, c);
    auto it = sModules.find(m);
    if (c->r[3] && it != sModules.end())
    {
        logf("live: module %s (%u) unlinked from %08X", it->second.name.c_str(), it->second.id, m);
        sModules.erase(it); // its code is freed with it
        rebuild_table();
    }
}
} // namespace

extern "C" void gcnr_live_ensure(void)
{
    static bool sDone = false;
    if (sDone)
    {
        return;
    }
    sDone = true;
    const auto t0 = Clock::now();

    gcnr::Program program;
    std::string error;
    const std::string syms(reinterpret_cast<const char*>(gcnr_live_syms), gcnr_live_syms_size);
    if (!program.load_symbols_text(syms, "syms.txt", error)) fail(error);
    parse_module_syms(syms);
    std::vector<uint8_t> dol = read_dol();
    const std::string sha1 = sha1_hex(dol);
    if (!program.dolSha1.empty() && sha1 != program.dolSha1)
    {
        fail("the disc's main.dol (sha1 " + sha1 + ") is not the one this build recompiles (" + program.dolSha1 +
             "): another region or revision");
    }
    if (!program.load_dol_data(std::move(dol), "main.dol", error)) fail(error);
    std::vector<std::string> warnings;
    const int extras = gcnr::find_extra_entries(program, warnings);

    gcnr::LiveInputs in;
    in.hle = gcnr_hle_functions;
    if (!sModuleSyms.empty())
    {
        // the module loader: wrapped (see the top of this file)
        for (const auto& [addr, f] : program.functions)
        {
            if (f.hle) continue;
            if (f.name == "OSLink") sOSLink = addr;
            else if (f.name == "OSLinkFixed") sOSLinkFixed = addr;
            else if (f.name == "OSUnlink") sOSUnlink = addr;
        }
        for (uint32_t a : {sOSLink, sOSLinkFixed, sOSUnlink})
        {
            if (a) in.hooked.insert(a);
        }
        if (!sOSLink && !sOSLinkFixed) logf("live: the game has modules, but no OSLink / OSLinkFixed by name");
    }
    gcnr::LiveResult result = gcnr::live_recompile(program, in);
    if (!result.error.empty()) fail(result.error);
    sDolCode = result.owner;

    sDolTable.clear();
    for (const gcnr::LiveFunction& f : result.functions)
    {
        gcnr_func fn = f.fn;
        if (in.hooked.count(f.addr))
        {
            sOriginal[f.addr] = fn;
            fn = f.addr == sOSLink ? wrap_OSLink : f.addr == sOSLinkFixed ? wrap_OSLinkFixed : wrap_OSUnlink;
        }
        sDolTable.push_back({f.addr, fn});
    }
    sR2 = program.r2;
    sR13 = program.r13;
    rebuild_table();
    gcnr_entry = program.entry;
    gcnr_r1 = program.r1;
    gcnr_r2 = program.r2;
    gcnr_r13 = program.r13;
    gcnr_ctors = program.ctors;
    std::snprintf(gcnr_dol_sha1, sizeof(gcnr_dol_sha1), "%s", sha1.c_str());

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const size_t total = result.inlined + result.helpers;
    logf("live: recompiled %d functions (%d extra entry points, %zu instructions) from main.dol %s in %.0f ms: "
         "%zu KB of code, %.0f%% of the instructions inline%s",
         result.recompiled, extras, result.instructions, sha1.c_str(), ms, result.codeSize / 1024,
         total ? 100.0 * (double)result.inlined / (double)total : 0.0,
         sModuleSyms.empty() ? "" : (", " + std::to_string(sModuleSyms.size()) + " modules recompiled as the game links them").c_str());
    warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
    for (size_t i = 0; i < warnings.size() && i < 20; i++)
    {
        logf("live: %s", warnings[i].c_str());
    }
}
