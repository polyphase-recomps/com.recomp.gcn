/**
 * @file live.h
 * @brief Live recomp builds: the game's machine code recompiled to native code when it starts
 *        (sljit), from the player's disc - no compiler, no game code in the build.
 *
 *  - live_gen.cpp      LiveGenerator: the recompiler's analysis (Runtime/recomp/tool) -> sljit code,
 *                      one native function per guest function, the same `void f(mem, c)` as the
 *                      C generator's, so recomp_gcn.c's lookup, the HLE glue and the callbacks
 *                      don't know the difference.
 *  - live_exec.cpp     gcnl_exec: what the generator doesn't emit inline, one instruction at a
 *                      time, with exactly the C generator's semantics (cgen.cpp, gcn_recomp.h).
 *  - recomp_live.cpp   the runtime side: reads main.dol from the disc, checks it is the one the
 *                      symbols describe, recompiles everything and fills recomp_gcn.c's tables.
 *
 * Floating point is the C generator's with GCNR_FMA=0 (multiply, then add: like the decomp
 * build), so a Live run matches an AOT run built with GCNR_FMA=OFF frame for frame.
 */
#pragma once

#include "gcn_recomp.h"

#ifdef __cplusplus
#include "../tool/gekko.h"
#include "../tool/program.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

// One instruction gcnl_exec runs: the decoded instruction and, when the analysis proved its
// effective address is a hardware register, that address (cgen.cpp's mmio()).
struct gcnl_insn
{
    gekko::Insn i;
    uint32_t hw = 0;
};

extern "C" void gcnl_exec(uint8_t* mem, gcnr_ctx* c, const gcnl_insn* li);
// the instruction at addr, decoded when it runs (code another module's link rewrites)
extern "C" void gcnl_exec_at(uint8_t* mem, gcnr_ctx* c, uint32_t addr);
// the call the b / bl at addr makes now (its target rewritten by OSLink / OSUnlink)
extern "C" void gcnl_call_insn(uint8_t* mem, gcnr_ctx* c, uint32_t addr);

namespace gcnr
{
struct LiveFunction
{
    uint32_t addr = 0;
    gcnr_func fn = nullptr;
};

struct LiveInputs
{
    const gcnr_named_func* hle = nullptr; // the HLE wrappers by name (gen_hle_glue.py's gcnr_hle_functions)
    // code compiled before (the DOL, modules linked earlier): a call there is direct; nullptr: unknown
    // (the call goes through the lookup when it happens)
    std::function<gcnr_func(uint32_t)> external;
    std::set<uint32_t> hooked;  // functions whose calls go through the lookup (recomp_live.cpp wraps them)
    std::set<uint32_t> dynamic; // instructions another module's link rewrites: decoded when they run
};

struct LiveResult
{
    std::vector<LiveFunction> functions; // sorted by address (recompiled and HLE alike)
    void* code = nullptr;                // the sljit code block
    std::shared_ptr<void> owner;         // the code and what it refers to: freed with the last copy
    size_t codeSize = 0;
    size_t instructions = 0;
    size_t inlined = 0;   // instructions emitted as native code
    size_t helpers = 0;   // instructions that call gcnl_exec
    int recompiled = 0;
    std::vector<std::string> warnings;
    std::string error;    // set when it failed
};

LiveResult live_recompile(Program& program, const LiveInputs& inputs);
} // namespace gcnr
#endif
