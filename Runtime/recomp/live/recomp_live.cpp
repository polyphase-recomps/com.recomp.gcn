/**
 * @file recomp_live.cpp
 * @brief The runtime side of a Live build (see live.h): recompiles the game from the disc the
 *        first time recomp_gcn.c needs a function, and provides the tables an AOT build's
 *        generated functable.c and game.c would (gcnr_functions, gcnr_r2, ...).
 *
 * The symbols (syms.txt: names, sizes, switch tables, HLE list - no game code) are built into the
 * library (<name>_live_syms.c, gcn_live_syms.py); main.dol comes from the disc the game runs
 * from, and must be the one they describe (its SHA-1), or the game does not start.
 */
#include "live.h"

#include "../tool/analysis.h"

extern "C" {
#include "gcn_platform.h"
}

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
}

namespace
{
std::vector<gcnr_function> sTable;

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
} // namespace

extern "C" void gcnr_live_ensure(void)
{
    static bool sDone = false;
    if (sDone)
    {
        return;
    }
    sDone = true;
    const auto t0 = std::chrono::steady_clock::now();

    gcnr::Program program;
    std::string error;
    const std::string syms(reinterpret_cast<const char*>(gcnr_live_syms), gcnr_live_syms_size);
    if (!program.load_symbols_text(syms, "syms.txt", error)) fail(error);
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

    gcnr::LiveResult result = gcnr::live_recompile(program, gcnr_hle_functions);
    if (!result.error.empty()) fail(result.error);

    sTable.clear();
    sTable.reserve(result.functions.size());
    for (const gcnr::LiveFunction& f : result.functions)
    {
        sTable.push_back({f.addr, f.fn});
    }
    gcnr_functions = sTable.data();
    gcnr_function_count = (uint32_t)sTable.size();
    gcnr_entry = program.entry;
    gcnr_r1 = program.r1;
    gcnr_r2 = program.r2;
    gcnr_r13 = program.r13;
    gcnr_ctors = program.ctors;
    std::snprintf(gcnr_dol_sha1, sizeof(gcnr_dol_sha1), "%s", sha1.c_str());

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const size_t total = result.inlined + result.helpers;
    logf("live: recompiled %d functions (%d extra entry points, %zu instructions) from main.dol %s in %.0f ms: "
         "%zu KB of code, %.0f%% of the instructions inline",
         result.recompiled, extras, result.instructions, sha1.c_str(), ms, result.codeSize / 1024,
         total ? 100.0 * (double)result.inlined / (double)total : 0.0);
    warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
    for (size_t i = 0; i < warnings.size() && i < 20; i++)
    {
        logf("live: %s", warnings[i].c_str());
    }
}
