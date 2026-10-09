/**
 * @file analysis.cpp
 * @brief Per-function analysis (see analysis.h).
 */
#include "analysis.h"

#include <cstdio>
#include <deque>
#include <map>
#include <set>

using gekko::Insn;
using gekko::Op;

namespace gcnr
{
namespace
{
std::string hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

std::vector<Insn> decode_range(const Program& p, uint32_t start, uint32_t end)
{
    std::vector<Insn> out;
    for (uint32_t a = start; a < end; a += 4)
    {
        uint32_t w = 0;
        p.read32(a, w);
        out.push_back(gekko::decode(w, a));
    }
    return out;
}

bool is_call(const Insn& i)
{
    return ((i.op == Op::B || i.op == Op::Bc || i.op == Op::Bclr || i.op == Op::Bcctr) && i.lk) || i.op == Op::Sc;
}

// unconditional: BO = 1z1zz
bool always(const Insn& i)
{
    return (i.bo() & 0x14) == 0x14;
}

// GPRs an instruction writes, as a bit mask
uint32_t gpr_writes(const Insn& i)
{
    const uint32_t d = 1u << i.d, a = 1u << i.a;
    switch (i.op)
    {
    case Op::Add: case Op::Addc: case Op::Adde: case Op::Addi: case Op::Addic: case Op::AddicRc: case Op::Addis:
    case Op::Addme: case Op::Addze: case Op::Divw: case Op::Divwu: case Op::Mulhw: case Op::Mulhwu: case Op::Mulli:
    case Op::Mullw: case Op::Neg: case Op::Subf: case Op::Subfc: case Op::Subfe: case Op::Subfic: case Op::Subfme:
    case Op::Subfze: case Op::Lbz: case Op::Lbzx: case Op::Lha: case Op::Lhax: case Op::Lhz: case Op::Lhzx:
    case Op::Lwz: case Op::Lwzx: case Op::Lhbrx: case Op::Lwbrx: case Op::Lwarx: case Op::Mfcr: case Op::Mfmsr:
    case Op::Mfspr: case Op::Mftb: case Op::Mfsr: case Op::Mfsrin: case Op::Eciwx:
        return d;
    case Op::Lbzu: case Op::Lbzux: case Op::Lhau: case Op::Lhaux: case Op::Lhzu: case Op::Lhzux: case Op::Lwzu:
    case Op::Lwzux:
        return d | a;
    case Op::And: case Op::Andc: case Op::AndiRc: case Op::AndisRc: case Op::Cntlzw: case Op::Eqv: case Op::Extsb:
    case Op::Extsh: case Op::Nand: case Op::Nor: case Op::Or: case Op::Orc: case Op::Ori: case Op::Oris:
    case Op::Xor: case Op::Xori: case Op::Xoris: case Op::Rlwimi: case Op::Rlwinm: case Op::Rlwnm: case Op::Slw:
    case Op::Sraw: case Op::Srawi: case Op::Srw:
    case Op::Stbu: case Op::Stbux: case Op::Sthu: case Op::Sthux: case Op::Stwu: case Op::Stwux:
    case Op::Lfsu: case Op::Lfsux: case Op::Lfdu: case Op::Lfdux: case Op::Stfsu: case Op::Stfsux: case Op::Stfdu:
    case Op::Stfdux: case Op::PsqLu: case Op::PsqLux: case Op::PsqStu: case Op::PsqStux:
        return a;
    case Op::Lmw:
        return i.d == 0 ? 0xFFFFFFFFu : ~((1u << i.d) - 1u);
    case Op::Lswi: case Op::Lswx:
        return 0xFFFFFFFFu; // several registers, wrapping: give up on all of them
    default:
        return 0;
    }
}

// GPRs an instruction reads, as a bit mask (conservative: unknown forms read d, a and b)
uint32_t gpr_reads(const Insn& i)
{
    const uint32_t d = 1u << i.d, a = i.a ? 1u << i.a : 0u, ra = 1u << i.a, b = 1u << i.b;
    switch (i.op)
    {
    case Op::Addi: case Op::Addis: return a;
    case Op::Addic: case Op::AddicRc: case Op::Subfic: case Op::Mulli: case Op::Cmpi: case Op::Cmpli:
    case Op::Addme: case Op::Addze: case Op::Subfme: case Op::Subfze: case Op::Neg:
        return ra;
    case Op::Add: case Op::Addc: case Op::Adde: case Op::Divw: case Op::Divwu: case Op::Mulhw: case Op::Mulhwu:
    case Op::Mullw: case Op::Subf: case Op::Subfc: case Op::Subfe: case Op::Cmp: case Op::Cmpl:
        return ra | b;
    case Op::And: case Op::Andc: case Op::Eqv: case Op::Nand: case Op::Nor: case Op::Or: case Op::Orc: case Op::Xor:
    case Op::Slw: case Op::Sraw: case Op::Srw: case Op::Rlwnm:
        return d | b;
    case Op::AndiRc: case Op::AndisRc: case Op::Ori: case Op::Oris: case Op::Xori: case Op::Xoris: case Op::Cntlzw:
    case Op::Extsb: case Op::Extsh: case Op::Rlwinm: case Op::Srawi:
        return d;
    case Op::Rlwimi:
        return d | ra;
    case Op::Lbz: case Op::Lha: case Op::Lhz: case Op::Lwz: case Op::Lfs: case Op::Lfd: case Op::PsqL: case Op::Lmw:
        return a;
    case Op::Lbzu: case Op::Lhau: case Op::Lhzu: case Op::Lwzu: case Op::Lfsu: case Op::Lfdu: case Op::PsqLu:
        return ra;
    case Op::Lbzx: case Op::Lhax: case Op::Lhzx: case Op::Lwzx: case Op::Lhbrx: case Op::Lwbrx: case Op::Lfsx:
    case Op::Lfdx: case Op::PsqLx: case Op::Lwarx:
        return a | b;
    case Op::Lbzux: case Op::Lhaux: case Op::Lhzux: case Op::Lwzux: case Op::Lfsux: case Op::Lfdux: case Op::PsqLux:
        return ra | b;
    case Op::B: case Op::Bc: case Op::Mfspr: case Op::Mftb: case Op::Mfcr: case Op::Mfmsr: case Op::Sync:
    case Op::Isync: case Op::Crand: case Op::Crandc: case Op::Creqv: case Op::Crnand: case Op::Crnor: case Op::Cror:
    case Op::Crorc: case Op::Crxor: case Op::Mcrf:
        return 0;
    default:
        if ((i.word >> 26) == 59 || (i.word >> 26) == 63 || ((i.word >> 26) == 4 && !gekko::is_load_store(i.op)))
        {
            return 0; // floating point / paired single arithmetic
        }
        return d | ra | b;
    }
}

struct Value
{
    bool known = false;
    uint32_t v = 0;
    bool operator==(const Value& o) const { return known == o.known && (!known || v == o.v); }
};

struct State
{
    Value r[32];
};

State merge(const State& x, const State& y)
{
    State s;
    for (int i = 0; i < 32; i++)
    {
        s.r[i] = (x.r[i] == y.r[i]) ? x.r[i] : Value{};
    }
    return s;
}

bool same(const State& x, const State& y)
{
    for (int i = 0; i < 32; i++)
    {
        if (!(x.r[i] == y.r[i]))
        {
            return false;
        }
    }
    return true;
}

void transfer(State& s, const Insn& i)
{
    Value out;
    switch (i.op)
    {
    case Op::Addi:
        if (i.a == 0) out = {true, (uint32_t)i.simm};
        else if (s.r[i.a].known) out = {true, s.r[i.a].v + (uint32_t)i.simm};
        s.r[i.d] = out;
        return;
    case Op::Addis:
        if (i.a == 0) out = {true, (uint32_t)i.simm << 16};
        else if (s.r[i.a].known) out = {true, s.r[i.a].v + ((uint32_t)i.simm << 16)};
        s.r[i.d] = out;
        return;
    case Op::Ori: case Op::Oris: case Op::Xori: case Op::Xoris:
        if (s.r[i.d].known)
        {
            const uint32_t v = s.r[i.d].v;
            const uint32_t imm = i.uimm;
            out.known = true;
            out.v = i.op == Op::Ori ? v | imm : i.op == Op::Oris ? v | (imm << 16) : i.op == Op::Xori ? v ^ imm : v ^ (imm << 16);
        }
        s.r[i.a] = out;
        return;
    case Op::Or:
        if (i.d == i.b) // mr rA,rS
        {
            s.r[i.a] = s.r[i.d];
            return;
        }
        break;
    default:
        break;
    }
    const uint32_t w = gpr_writes(i);
    for (int r = 0; r < 32; r++)
    {
        if (w & (1u << r))
        {
            s.r[r] = Value{};
        }
    }
    if (is_call(i))
    {
        s.r[0] = Value{};
        for (int r = 3; r <= 12; r++)
        {
            s.r[r] = Value{};
        }
    }
}

// the effective address of a load/store when the state makes it constant
bool effective_address(const State& s, const Insn& i, uint32_t& ea)
{
    if (!gekko::is_load_store(i.op) && i.op != Op::Dcbz)
    {
        return false;
    }
    const bool xform = (i.word >> 26) == 31 || ((i.word >> 26) == 4 && i.op != Op::PsqL);
    const bool psqd = i.op == Op::PsqL || i.op == Op::PsqLu || i.op == Op::PsqSt || i.op == Op::PsqStu;
    uint32_t base = 0;
    if (i.a != 0)
    {
        if (!s.r[i.a].known)
        {
            return false;
        }
        base = s.r[i.a].v;
    }
    if (xform && !psqd)
    {
        if (!s.r[i.b].known)
        {
            return false;
        }
        ea = base + s.r[i.b].v;
        return true;
    }
    ea = base + (uint32_t)i.simm;
    return true;
}
} // namespace

int find_extra_entries(Program& program, std::vector<std::string>& warnings)
{
    std::deque<uint32_t> work;
    for (auto& [addr, f] : program.functions)
    {
        if (!f.hle)
        {
            work.push_back(addr);
        }
    }
    int added = 0;
    auto add_entry = [&](uint32_t target, uint32_t from) {
        if (!program.in_text(target) || program.function_at(target) != nullptr)
        {
            return;
        }
        const Function* owner = program.function_containing(target);
        if (owner == nullptr)
        {
            // code outside every sized symbol (an assembly routine the decomp only has a label
            // for): a function of its own, up to the next function
            auto next = program.functions.upper_bound(target);
            uint32_t end = next == program.functions.end() ? target + 4 : next->first;
            uint32_t w = 0;
            for (uint32_t a = target; a < end; a += 4)
            {
                if (!program.in_text(a) || !program.read32(a, w))
                {
                    end = a;
                    break;
                }
            }
            if (end <= target)
            {
                warnings.push_back("branch from " + hex(from) + " to " + hex(target) + " outside the code");
                return;
            }
            Function g;
            g.addr = target;
            g.end = end;
            program.functions[target] = g;
            work.push_back(target);
            added++;
            return;
        }
        if (owner->hle)
        {
            warnings.push_back("branch from " + hex(from) + " into the middle of HLE function " + owner->name);
            return;
        }
        Function e;
        e.addr = target;
        e.end = owner->end;
        e.extra = true;
        program.functions[target] = e;
        work.push_back(target);
        added++;
    };
    for (const auto& [addr, name] : program.labels)
    {
        add_entry(addr, addr);
    }
    while (!work.empty())
    {
        const uint32_t addr = work.front();
        work.pop_front();
        const Function f = program.functions[addr];
        for (const Insn& i : decode_range(program, f.addr, f.end))
        {
            if (i.op != Op::B && i.op != Op::Bc)
            {
                continue;
            }
            const bool local = i.target >= f.addr && i.target < f.end;
            if (i.lk && i.target != i.addr + 4)
            {
                add_entry(i.target, i.addr); // a call: always to an entry point
            }
            else if (!local)
            {
                add_entry(i.target, i.addr); // a tail call / jump into another function
            }
        }
    }
    return added;
}

bool needs_lr_mode(const Program& program, const Function& fn)
{
    for (uint32_t a = fn.addr; a < fn.end; a += 4)
    {
        uint32_t w = 0;
        program.read32(a, w);
        const Insn i = gekko::decode(w, a);
        if ((i.op == Op::B || i.op == Op::Bc) && i.lk && i.target != fn.addr && i.target != a + 4 &&
            i.target >= fn.addr && i.target < fn.end)
        {
            return true;
        }
    }
    return false;
}

Analysis analyze(const Program& program, const Function& fn)
{
    Analysis an;
    an.fn = &fn;
    an.insns = decode_range(program, fn.addr, fn.end);
    const size_t n = an.insns.size();
    auto index_of = [&](uint32_t addr) { return (size_t)((addr - fn.addr) / 4); };
    an.lrMode = needs_lr_mode(program, fn);
    if (an.lrMode)
    {
        an.entries.push_back(fn.addr);
        for (auto it = program.functions.upper_bound(fn.addr); it != program.functions.end() && it->first < fn.end; ++it)
        {
            an.entries.push_back(it->first);
            an.labels.insert(it->first);
        }
        an.labels.insert(fn.addr);
        for (const Insn& i : an.insns)
        {
            if ((i.op == Op::B || i.op == Op::Bc) && i.lk && i.target != i.addr + 4 && an.local(i.target))
            {
                an.labels.insert(i.target);
                if (i.addr + 4 < fn.end)
                {
                    an.lrSites.insert(i.addr + 4);
                    an.labels.insert(i.addr + 4);
                }
            }
        }
    }

    // labels and backward branches
    for (size_t k = 0; k < n; k++)
    {
        const Insn& i = an.insns[k];
        if ((i.op == Op::B || i.op == Op::Bc) && !i.lk && an.local(i.target))
        {
            an.labels.insert(i.target);
            if (i.target <= i.addr)
            {
                an.backBranches.insert(i.addr);
            }
        }
    }

    // busy-wait loops: a backward branch whose loop body neither stores nor calls and does not
    // advance (every GPR it reads is loop-invariant or written earlier in the same iteration:
    // no index / pointer stepping, no list walking). Only those get loop checks.
    for (uint32_t br : an.backBranches)
    {
        const Insn& j = an.insns[index_of(br)];
        const size_t first = index_of(j.target), last = index_of(br);
        // a loop closed by bdnz & co counts CTR down: it ends by itself
        bool quiet = !(j.op == Op::Bc && !(j.bo() & 4));
        uint32_t writtenInLoop = 0;
        for (size_t k = first; k <= last && quiet; k++)
        {
            const Insn& i = an.insns[k];
            const std::string m = gekko::mnemonic(i.op);
            const bool store = (m.size() > 2 && m[0] == 's' && m[1] == 't') || i.op == Op::PsqSt ||
                               i.op == Op::PsqStu || i.op == Op::PsqStx || i.op == Op::PsqStux || i.op == Op::Dcbz ||
                               i.op == Op::DcbzL;
            quiet = !store && !is_call(i) && i.op != Op::Bcctr && i.op != Op::Bclr;
            writtenInLoop |= gpr_writes(i);
        }
        uint32_t defined = 0;
        for (size_t k = first; k <= last && quiet; k++)
        {
            const Insn& i = an.insns[k];
            if (gpr_reads(i) & writtenInLoop & ~defined)
            {
                quiet = false; // a value carried from the previous iteration: the loop advances
            }
            defined |= gpr_writes(i);
        }
        if (quiet)
        {
            an.spinLoops.insert(br);
        }
    }

    // Constant propagation over the basic blocks, to a fixpoint. Jump tables are recognised while
    // walking; their targets become block leaders, so it runs again until no new ones appear.
    std::vector<size_t> blockStart;
    std::map<size_t, size_t> blockOf;
    std::vector<State> in;
    std::vector<bool> seen;
    auto block_end = [&](size_t b) { return b + 1 < blockStart.size() ? blockStart[b + 1] : n; };

    // Stack slots (offsets from r1) and the constants stored there: a jump table's base the
    // compiler spilled (lis/addi early, lwz rX,off(r1) at the switch) is found through a slot
    // stored once, with one constant, and written by nothing else in the function. Only the
    // jump table search reads them (and only with the table's symbol), not the constant
    // propagation, so a store through another pointer into the frame cannot mislead codegen.
    std::map<int32_t, std::set<uint32_t>> slotValues;
    std::set<int32_t> slotClobbered;
    bool slotAll = false; // an indexed store into the frame: every slot may change
    bool slotsChanged = false;
    auto note_stack_store = [&](const State& s, const Insn& i) {
        if (i.a != 1 || !gekko::is_load_store(i.op)) return;
        const std::string m = gekko::mnemonic(i.op);
        const bool store = (m.size() > 2 && m[0] == 's' && m[1] == 't') || i.op == Op::PsqSt || i.op == Op::PsqStu ||
                           i.op == Op::PsqStx || i.op == Op::PsqStux || i.op == Op::DcbzL;
        if (!store || i.op == Op::Stwu) return; // stwu r1: the frame itself
        const bool xform = (i.word >> 26) == 31 || ((i.word >> 26) == 4 && i.op != Op::PsqL);
        if (xform)
        {
            if (!slotAll) slotsChanged = true;
            slotAll = true;
            return;
        }
        if (i.op == Op::Stw && s.r[i.d].known)
        {
            if (slotValues[i.simm].insert(s.r[i.d].v).second) slotsChanged = true;
            return;
        }
        int size = 8;
        if (i.op == Op::Stb || i.op == Op::Stbu) size = 1;
        else if (i.op == Op::Sth || i.op == Op::Sthu) size = 2;
        else if (i.op == Op::Stw || i.op == Op::Stfs || i.op == Op::Stfsu) size = 4;
        else if (i.op == Op::Stmw) size = (32 - i.d) * 4;
        for (int32_t off = i.simm - 3; off < i.simm + size; off++)
        {
            if (slotClobbered.insert(off).second) slotsChanged = true;
        }
    };
    auto slot_value = [&](int32_t off, uint32_t& v) {
        auto it = slotValues.find(off);
        if (slotAll || it == slotValues.end() || it->second.size() != 1 || slotClobbered.count(off)) return false;
        v = *it->second.begin();
        return true;
    };

    auto successors = [&](size_t b, std::vector<size_t>& out) {
        const size_t last = block_end(b) - 1;
        const Insn& i = an.insns[last];
        const bool fall = last + 1 < n;
        if ((i.op == Op::B || i.op == Op::Bc) && !i.lk)
        {
            if (an.local(i.target))
            {
                out.push_back(index_of(i.target));
            }
            if (i.op == Op::Bc && !always(i) && fall)
            {
                out.push_back(last + 1);
            }
            return;
        }
        if ((i.op == Op::Bclr || i.op == Op::Bcctr) && !i.lk)
        {
            auto sw = an.switches.find(i.addr);
            if (i.op == Op::Bcctr && sw != an.switches.end())
            {
                for (uint32_t t : sw->second)
                {
                    out.push_back(index_of(t));
                }
            }
            if (!always(i) && fall)
            {
                out.push_back(last + 1);
            }
            return;
        }
        if (i.op == Op::Rfi)
        {
            return;
        }
        if (fall)
        {
            out.push_back(last + 1);
        }
    };

    // walks block b from its entry state; records effective addresses when `record`
    auto walk = [&](size_t b, bool record) {
        State s = in[b];
        const size_t endK = block_end(b);
        for (size_t k = blockStart[b]; k < endK; k++)
        {
            const Insn& i = an.insns[k];
            uint32_t ea = 0;
            if (record && effective_address(s, i, ea))
            {
                an.knownEa[i.addr] = ea;
            }
            note_stack_store(s, i);
            if (i.op == Op::Bcctr && !i.lk && an.switches.count(i.addr) == 0)
            {
                // lwzx rX,<table>,rI ... mtctr rX ... bctr, with the table address known
                State t = in[b];
                int tableReg = -1;
                uint32_t table = 0;
                bool ctrFromTable = false;
                std::set<int> fromSlot; // registers holding a spilled constant
                bool tableFromSlot = false;
                for (size_t q = blockStart[b]; q < k; q++)
                {
                    const Insn& j = an.insns[q];
                    if (j.op == Op::Lwzx)
                    {
                        // the table base is whichever operand is a known data address
                        if (j.a != 0 && t.r[j.a].known && !program.in_text(t.r[j.a].v) && (t.r[j.a].v >> 24) == 0x80)
                        {
                            table = t.r[j.a].v, tableReg = j.d, tableFromSlot = fromSlot.count(j.a) != 0;
                        }
                        else if (t.r[j.b].known && !program.in_text(t.r[j.b].v) && (t.r[j.b].v >> 24) == 0x80)
                        {
                            table = t.r[j.b].v, tableReg = j.d, tableFromSlot = fromSlot.count(j.b) != 0;
                        }
                    }
                    if (j.op == Op::Mtspr && j.spr == 9)
                    {
                        ctrFromTable = tableReg >= 0 && j.d == tableReg;
                    }
                    transfer(t, j);
                    for (int r = 0; r < 32; r++)
                    {
                        if (gpr_writes(j) & (1u << r)) fromSlot.erase(r);
                    }
                    uint32_t spilled = 0;
                    if (j.op == Op::Lwz && j.a == 1 && slot_value(j.simm, spilled))
                    {
                        t.r[j.d] = {true, spilled};
                        fromSlot.insert(j.d);
                    }
                }
                if (ctrFromTable && tableFromSlot && program.jumpTables.count(table) == 0)
                {
                    ctrFromTable = false; // a spilled base counts only with the table's symbol
                }
                if (ctrFromTable)
                {
                    std::vector<uint32_t> targets;
                    auto known = program.jumpTables.find(table);
                    bool ok = true;
                    if (known != program.jumpTables.end())
                    {
                        for (uint32_t off = 0; off < known->second; off += 4)
                        {
                            uint32_t target = 0;
                            if (!program.read32(table + off, target) || !an.local(target) || (target & 3))
                            {
                                ok = false;
                                break;
                            }
                            targets.push_back(target);
                        }
                    }
                    else
                    {
                        // no symbol there: entries up to the first that is not code of this function
                        for (uint32_t off = 0; off < 4096; off += 4)
                        {
                            uint32_t target = 0;
                            if (!program.read32(table + off, target) || !an.local(target) || (target & 3))
                            {
                                break;
                            }
                            targets.push_back(target);
                        }
                        ok = !targets.empty();
                        if (ok)
                        {
                            an.warnings.push_back("jump table " + hex(table) + " at " + hex(i.addr) + " has no symbol: " +
                                                  std::to_string(targets.size()) + " entries assumed");
                        }
                    }
                    if (ok)
                    {
                        for (uint32_t target : targets)
                        {
                            an.labels.insert(target);
                        }
                        an.switches[i.addr] = targets;
                    }
                    else
                    {
                        an.warnings.push_back("jump table " + hex(table) + " at " + hex(i.addr) + " leaves the function");
                    }
                }
            }
            transfer(s, i);
        }
        return s;
    };

    for (int round = 0; round < 8 && n > 0; round++)
    {
        const size_t switchesBefore = an.switches.size();
        slotsChanged = false;
        std::vector<bool> leader(n + 1, false);
        leader[0] = true;
        for (size_t k = 0; k < n; k++)
        {
            const Insn& i = an.insns[k];
            if (gekko::is_branch(i.op) || i.op == Op::Rfi)
            {
                leader[k + 1] = true;
            }
        }
        for (uint32_t l : an.labels)
        {
            leader[index_of(l)] = true;
        }
        blockStart.clear();
        blockOf.clear();
        for (size_t k = 0; k < n; k++)
        {
            if (leader[k])
            {
                blockOf[k] = blockStart.size();
                blockStart.push_back(k);
            }
        }
        in.assign(blockStart.size(), State{});
        seen.assign(blockStart.size(), false);
        in[0].r[2] = {true, program.r2};
        in[0].r[13] = {true, program.r13};
        seen[0] = true;
        std::deque<size_t> work{0};
        while (!work.empty())
        {
            const size_t b = work.front();
            work.pop_front();
            const State out = walk(b, false);
            std::vector<size_t> succ;
            successors(b, succ);
            for (size_t k : succ)
            {
                auto it = blockOf.find(k);
                if (it == blockOf.end())
                {
                    continue; // a new switch target: becomes a leader next round
                }
                const size_t sb = it->second;
                const State merged = seen[sb] ? merge(in[sb], out) : out;
                if (!seen[sb] || !same(merged, in[sb]))
                {
                    in[sb] = merged;
                    seen[sb] = true;
                    work.push_back(sb);
                }
            }
        }
        if (an.switches.size() == switchesBefore && !slotsChanged)
        {
            break;
        }
    }

    // the final states: constant effective addresses (unreached blocks know nothing)
    for (size_t b = 0; b < blockStart.size(); b++)
    {
        if (seen[b])
        {
            walk(b, true);
        }
    }
    return an;
}
} // namespace gcnr
