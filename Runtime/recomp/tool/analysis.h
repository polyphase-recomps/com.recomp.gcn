/**
 * @file analysis.h
 * @brief Per-function analysis for the GameCube recompiler.
 *
 *  - local branch targets (labels) and backward branches (loop checks);
 *  - switch jump tables at `bctr` (mtctr fed by lwzx from a known table address);
 *  - effective addresses that are constant (lis/addi/ori chains, also hoisted into callee-saved
 *    registers), so hardware register accesses (0xCCxxxxxx) and write-gather pipe stores
 *    (0xCC008000) become runtime calls instead of memory accesses;
 *  - extra entry points: call / tail-call targets inside other functions, and symbol labels.
 */
#pragma once

#include "gekko.h"
#include "program.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace gcnr
{
struct Analysis
{
    const Function* fn = nullptr;
    std::vector<gekko::Insn> insns;  // the function's instructions, [addr, end)
    std::set<uint32_t> labels;       // local branch targets
    std::set<uint32_t> backBranches; // branches (by address) that jump backwards
    std::set<uint32_t> spinLoops;    // backward branches closing a loop with no stores and no calls:
                                     // busy-waits on memory something else changes (loop checks)
    std::map<uint32_t, std::vector<uint32_t>> switches; // bctr address -> its jump table's targets
    std::map<uint32_t, uint32_t> knownEa; // load/store address -> its effective address, when constant
    // LR mode (hand-written assembly calling subroutines inside its own body): local bl jumps with
    // LR set, blr goes back to the local return site LR names (else returns), as the hardware does
    bool lrMode = false;
    std::set<uint32_t> lrSites;  // return sites of local bl (the instruction after each)
    std::vector<uint32_t> entries; // LR mode: every entry point into the body (the function first)
    std::vector<std::string> warnings;

    bool local(uint32_t addr) const { return addr >= fn->addr && addr < fn->end; }
};

// Adds extra entry points to the program (targets of calls and tail calls that land inside other
// functions, and symbol labels in text). Returns how many were added.
int find_extra_entries(Program& program, std::vector<std::string>& warnings);

// A function that calls into its own body (bl to a target inside it, not its start): LR mode.
bool needs_lr_mode(const Program& program, const Function& fn);

Analysis analyze(const Program& program, const Function& fn);
} // namespace gcnr
