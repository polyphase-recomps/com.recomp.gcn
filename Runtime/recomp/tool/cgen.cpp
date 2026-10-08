/**
 * @file cgen.cpp
 * @brief The C generator: one C function per guest function (see generator.h, gcn_recomp.h).
 */
#include "generator.h"

#include <cstdarg>
#include <cstdio>

using gekko::Insn;
using gekko::Op;

namespace gcnr
{
namespace
{
std::string fmt(const char* f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string R(int n) { return fmt("c->r[%d]", n); }
std::string RA0(int n) { return n == 0 ? std::string("0u") : R(n); }
std::string F0(int n) { return fmt("c->f[%d].ps0", n); }
std::string F1(int n) { return fmt("c->f[%d].ps1", n); }
std::string U(uint32_t v) { return fmt("0x%Xu", v); }

// rlwinm-style mask, bits mb..me (IBM numbering), wrapping
uint32_t ppc_mask(int mb, int me)
{
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = (me >= 31) ? 0u : (0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & ~b) : (a | ~b);
}

// a CR / FPSCR field mask from an 8-bit field selector (bit 7 = field 0)
uint32_t field_mask(uint32_t fxm)
{
    uint32_t m = 0;
    for (int f = 0; f < 8; f++)
    {
        if (fxm & (0x80u >> f))
        {
            m |= 0xFu << (28 - 4 * f);
        }
    }
    return m;
}

class Emitter
{
public:
    Emitter(const Program& p, const Analysis& an, const GenOptions& o, std::string& out)
        : mP(p), mAn(an), mO(o), mOut(out)
    {
    }

    void run();

private:
    void line(const std::string& s) { mOut += "    " + s + "\n"; }
    void insn(const Insn& i);
    std::string call_target(uint32_t target) const;
    std::string cond(const Insn& i, std::string& pre) const;
    std::string ea_d(const Insn& i) const; // (rA|0) + d
    std::string ea_x(const Insn& i) const; // (rA|0) + rB
    bool mmio(const Insn& i, uint32_t& ea) const;
    void load(const Insn& i, const char* fn, int bytes, bool xform, bool update, bool sign = false);
    void store(const Insn& i, const char* fn, int bytes, bool xform, bool update);
    // the check a backward branch makes: busy-wait loops wait for events (GCNR_LOOP), every other
    // loop takes pending interrupts now and then (GCNR_LOOP_ANY)
    std::string loop_check(uint32_t at) const
    {
        if (mAn.spinLoops.count(at)) return "GCNR_LOOP(mem, c); ";
        if (mAn.backBranches.count(at)) return "GCNR_LOOP_ANY(mem, c); ";
        return "";
    }
    void rc0(const Insn& i, int reg) { if (i.rc) line(fmt("GCNR_CR0(c, %s);", R(reg).c_str())); }
    void rc1(const Insn& i) { if (i.rc) line("GCNR_CR1(c);"); }

    const Program& mP;
    const Analysis& mAn;
    const GenOptions& mO;
    std::string& mOut;
};

std::string Emitter::call_target(uint32_t target) const
{
    const Function* f = mP.function_at(target);
    if (f != nullptr)
    {
        return f->c_name() + "(mem, c);";
    }
    return fmt("GCNR_CALL_INDIRECT(mem, c, 0x%08Xu);", target);
}

// the branch condition of a bc-type instruction; `pre` gets the CTR decrement
std::string Emitter::cond(const Insn& i, std::string& pre) const
{
    const int bo = i.bo();
    std::string ctr, crc;
    if (!(bo & 4))
    {
        pre = "c->ctr--;";
        ctr = (bo & 2) ? "c->ctr == 0" : "c->ctr != 0";
    }
    if (!(bo & 16))
    {
        crc = fmt((bo & 8) ? "GCNR_CRBIT(c, %d)" : "!GCNR_CRBIT(c, %d)", i.bi());
    }
    if (ctr.empty() && crc.empty())
    {
        return "1";
    }
    if (ctr.empty())
    {
        return crc;
    }
    if (crc.empty())
    {
        return ctr;
    }
    return "(" + ctr + ") && " + crc;
}

std::string Emitter::ea_d(const Insn& i) const
{
    if (i.a == 0)
    {
        return U((uint32_t)i.simm);
    }
    if (i.simm == 0)
    {
        return R(i.a);
    }
    return fmt("%s + 0x%Xu", R(i.a).c_str(), (uint32_t)i.simm);
}

std::string Emitter::ea_x(const Insn& i) const
{
    return i.a == 0 ? R(i.b) : R(i.a) + " + " + R(i.b);
}

bool Emitter::mmio(const Insn& i, uint32_t& ea) const
{
    auto it = mAn.knownEa.find(i.addr);
    if (it == mAn.knownEa.end() || (it->second >> 24) != 0xCC)
    {
        return false;
    }
    ea = it->second;
    return true;
}

void Emitter::load(const Insn& i, const char* fn, int bytes, bool xform, bool update, bool sign)
{
    const std::string ea = xform ? ea_x(i) : ea_d(i);
    uint32_t hw = 0;
    std::string value;
    if (mmio(i, hw))
    {
        value = fmt("gcnr_mmio_read(0x%08Xu, %d)", hw, bytes);
        if (sign)
        {
            value = "(uint32_t)(int32_t)(int16_t)" + value;
        }
    }
    if (update)
    {
        line("{ const uint32_t ea = " + ea + ";");
        line("  " + R(i.d) + " = " + (value.empty() ? std::string(fn) + "(mem, ea)" : value) + ";");
        line("  " + R(i.a) + " = ea; }");
    }
    else
    {
        line(R(i.d) + " = " + (value.empty() ? std::string(fn) + "(mem, " + ea + ")" : value) + ";");
    }
}

void Emitter::store(const Insn& i, const char* fn, int bytes, bool xform, bool update)
{
    const std::string ea = xform ? ea_x(i) : ea_d(i);
    uint32_t hw = 0;
    if (mmio(i, hw))
    {
        line(fmt("gcnr_mmio_write(0x%08Xu, %s, %d);", hw, R(i.d).c_str(), bytes));
        if (update)
        {
            line(R(i.a) + " = " + U(hw) + ";");
        }
        return;
    }
    if (update)
    {
        line("{ const uint32_t ea = " + ea + ";");
        line("  " + std::string(fn) + "(mem, ea, " + R(i.d) + ");");
        line("  " + R(i.a) + " = ea; }");
    }
    else
    {
        line(std::string(fn) + "(mem, " + ea + ", " + R(i.d) + ");");
    }
}

void Emitter::run()
{
    const Function& f = *mAn.fn;
    if (mAn.lrMode)
    {
        // the body, entered at any of its entry points; f_ADDR wrappers below
        mOut += "void " + f.c_name() + "_body(uint8_t* mem, gcnr_ctx* c, uint32_t entry)\n{\n";
        line("switch (entry) {");
        for (uint32_t e : mAn.entries)
        {
            line(fmt("case 0x%08Xu: goto L_%08X;", e, e));
        }
        line("default: gcnr_unhandled(c, entry, \"entry into an LR-mode body\"); return;");
        line("}");
    }
    else
    {
        mOut += "void " + f.c_name() + "(uint8_t* mem, gcnr_ctx* c)\n{\n";
    }
    if (mO.traced.count(f.addr))
    {
        line(fmt("gcnr_trace_func(0x%08Xu, mem, c);", f.addr));
    }
    bool endsInBranch = false;
    for (const Insn& i : mAn.insns)
    {
        if (mAn.labels.count(i.addr))
        {
            mOut += fmt("L_%08X:;\n", i.addr);
        }
        if (mO.comments)
        {
            mOut += fmt("    /* %08X: %s */\n", i.addr, gekko::disasm(i).c_str());
        }
        insn(i);
        const bool uncondBranch = ((i.op == Op::B) && !i.lk) ||
                                  ((i.op == Op::Bc || i.op == Op::Bclr || i.op == Op::Bcctr) && !i.lk &&
                                   (i.bo() & 0x14) == 0x14);
        endsInBranch = uncondBranch;
    }
    if (!endsInBranch)
    {
        // runs off the end: into whatever follows (normally unreachable)
        if (const Function* next = mP.function_at(f.end))
        {
            line(next->c_name() + "(mem, c);");
        }
        line("return;");
    }
    mOut += "}\n\n";
    if (mAn.lrMode)
    {
        mOut += "void " + f.c_name() + "(uint8_t* mem, gcnr_ctx* c)\n{\n";
        line(fmt("%s_body(mem, c, 0x%08Xu);", f.c_name().c_str(), f.addr));
        mOut += "}\n\n";
    }
}

void Emitter::insn(const Insn& i)
{
    const uint32_t next = i.addr + 4;
    const int d = i.d, a = i.a, b = i.b, cc = i.c;
    switch (i.op)
    {
    // ---- integer arithmetic ----
    case Op::Add:
        line(i.oe ? fmt("%s = gcnr_add(c, %s, %s, 0, 0, 1);", R(d).c_str(), R(a).c_str(), R(b).c_str())
                  : fmt("%s = %s + %s;", R(d).c_str(), R(a).c_str(), R(b).c_str()));
        rc0(i, d);
        break;
    case Op::Addc:
        line(fmt("%s = gcnr_add(c, %s, %s, 0, 1, %d);", R(d).c_str(), R(a).c_str(), R(b).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Adde:
        line(fmt("%s = gcnr_add(c, %s, %s, c->xer_ca, 1, %d);", R(d).c_str(), R(a).c_str(), R(b).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Addze:
        line(fmt("%s = gcnr_add(c, %s, 0u, c->xer_ca, 1, %d);", R(d).c_str(), R(a).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Addme:
        line(fmt("%s = gcnr_add(c, %s, 0xFFFFFFFFu, c->xer_ca, 1, %d);", R(d).c_str(), R(a).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Addi:
        line(a == 0 ? fmt("%s = %s;", R(d).c_str(), U((uint32_t)i.simm).c_str())
                    : fmt("%s = %s + %s;", R(d).c_str(), R(a).c_str(), U((uint32_t)i.simm).c_str()));
        break;
    case Op::Addis:
        line(a == 0 ? fmt("%s = %s;", R(d).c_str(), U((uint32_t)i.simm << 16).c_str())
                    : fmt("%s = %s + %s;", R(d).c_str(), R(a).c_str(), U((uint32_t)i.simm << 16).c_str()));
        break;
    case Op::Addic:
    case Op::AddicRc:
        line(fmt("%s = gcnr_add(c, %s, %s, 0, 1, 0);", R(d).c_str(), R(a).c_str(), U((uint32_t)i.simm).c_str()));
        if (i.op == Op::AddicRc) line(fmt("GCNR_CR0(c, %s);", R(d).c_str()));
        break;
    case Op::Subf:
        line(i.oe ? fmt("%s = gcnr_add(c, ~%s, %s, 1, 0, 1);", R(d).c_str(), R(a).c_str(), R(b).c_str())
                  : fmt("%s = %s - %s;", R(d).c_str(), R(b).c_str(), R(a).c_str()));
        rc0(i, d);
        break;
    case Op::Subfc:
        line(fmt("%s = gcnr_add(c, ~%s, %s, 1, 1, %d);", R(d).c_str(), R(a).c_str(), R(b).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Subfe:
        line(fmt("%s = gcnr_add(c, ~%s, %s, c->xer_ca, 1, %d);", R(d).c_str(), R(a).c_str(), R(b).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Subfze:
        line(fmt("%s = gcnr_add(c, ~%s, 0u, c->xer_ca, 1, %d);", R(d).c_str(), R(a).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Subfme:
        line(fmt("%s = gcnr_add(c, ~%s, 0xFFFFFFFFu, c->xer_ca, 1, %d);", R(d).c_str(), R(a).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Subfic:
        line(fmt("%s = gcnr_add(c, ~%s, %s, 1, 1, 0);", R(d).c_str(), R(a).c_str(), U((uint32_t)i.simm).c_str()));
        break;
    case Op::Neg:
        line(i.oe ? fmt("%s = gcnr_add(c, ~%s, 0u, 1, 0, 1);", R(d).c_str(), R(a).c_str())
                  : fmt("%s = 0u - %s;", R(d).c_str(), R(a).c_str()));
        rc0(i, d);
        break;
    case Op::Mulli:
        line(fmt("%s = %s * %s;", R(d).c_str(), R(a).c_str(), U((uint32_t)i.simm).c_str()));
        break;
    case Op::Mullw:
        line(i.oe ? fmt("%s = gcnr_mullw(c, %s, %s, 1);", R(d).c_str(), R(a).c_str(), R(b).c_str())
                  : fmt("%s = %s * %s;", R(d).c_str(), R(a).c_str(), R(b).c_str()));
        rc0(i, d);
        break;
    case Op::Mulhw:
        line(fmt("%s = (uint32_t)(((int64_t)(int32_t)%s * (int32_t)%s) >> 32);", R(d).c_str(), R(a).c_str(), R(b).c_str()));
        rc0(i, d);
        break;
    case Op::Mulhwu:
        line(fmt("%s = (uint32_t)(((uint64_t)%s * %s) >> 32);", R(d).c_str(), R(a).c_str(), R(b).c_str()));
        rc0(i, d);
        break;
    case Op::Divw:
        line(fmt("%s = gcnr_divw(c, %s, %s, %d);", R(d).c_str(), R(a).c_str(), R(b).c_str(), i.oe));
        rc0(i, d);
        break;
    case Op::Divwu:
        line(fmt("%s = gcnr_divwu(c, %s, %s, %d);", R(d).c_str(), R(a).c_str(), R(b).c_str(), i.oe));
        rc0(i, d);
        break;

    // ---- compare ----
    case Op::Cmp:
        line(fmt("gcnr_cmp(c, %d, (int32_t)%s, (int32_t)%s);", i.crfd(), R(a).c_str(), R(b).c_str()));
        break;
    case Op::Cmpl:
        line(fmt("gcnr_cmpl(c, %d, %s, %s);", i.crfd(), R(a).c_str(), R(b).c_str()));
        break;
    case Op::Cmpi:
        line(fmt("gcnr_cmp(c, %d, (int32_t)%s, %d);", i.crfd(), R(a).c_str(), i.simm));
        break;
    case Op::Cmpli:
        line(fmt("gcnr_cmpl(c, %d, %s, %s);", i.crfd(), R(a).c_str(), U(i.uimm).c_str()));
        break;

    // ---- logical ----
    case Op::And: line(fmt("%s = %s & %s;", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::Andc: line(fmt("%s = %s & ~%s;", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::Or:
        line(d == b ? fmt("%s = %s;", R(a).c_str(), R(d).c_str())
                    : fmt("%s = %s | %s;", R(a).c_str(), R(d).c_str(), R(b).c_str()));
        rc0(i, a);
        break;
    case Op::Orc: line(fmt("%s = %s | ~%s;", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::Xor: line(fmt("%s = %s ^ %s;", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::Nand: line(fmt("%s = ~(%s & %s);", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::Nor: line(fmt("%s = ~(%s | %s);", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::Eqv: line(fmt("%s = ~(%s ^ %s);", R(a).c_str(), R(d).c_str(), R(b).c_str())); rc0(i, a); break;
    case Op::AndiRc:
        line(fmt("%s = %s & %s;", R(a).c_str(), R(d).c_str(), U(i.uimm).c_str()));
        line(fmt("GCNR_CR0(c, %s);", R(a).c_str()));
        break;
    case Op::AndisRc:
        line(fmt("%s = %s & %s;", R(a).c_str(), R(d).c_str(), U(i.uimm << 16).c_str()));
        line(fmt("GCNR_CR0(c, %s);", R(a).c_str()));
        break;
    case Op::Ori: line(fmt("%s = %s | %s;", R(a).c_str(), R(d).c_str(), U(i.uimm).c_str())); break;
    case Op::Oris: line(fmt("%s = %s | %s;", R(a).c_str(), R(d).c_str(), U(i.uimm << 16).c_str())); break;
    case Op::Xori: line(fmt("%s = %s ^ %s;", R(a).c_str(), R(d).c_str(), U(i.uimm).c_str())); break;
    case Op::Xoris: line(fmt("%s = %s ^ %s;", R(a).c_str(), R(d).c_str(), U(i.uimm << 16).c_str())); break;
    case Op::Cntlzw: line(fmt("%s = gcnr_cntlzw(%s);", R(a).c_str(), R(d).c_str())); rc0(i, a); break;
    case Op::Extsb: line(fmt("%s = (uint32_t)(int32_t)(int8_t)%s;", R(a).c_str(), R(d).c_str())); rc0(i, a); break;
    case Op::Extsh: line(fmt("%s = (uint32_t)(int32_t)(int16_t)%s;", R(a).c_str(), R(d).c_str())); rc0(i, a); break;

    // ---- rotate / shift ----
    case Op::Rlwinm:
    {
        const uint32_t m = ppc_mask(cc, i.e);
        line(b == 0 ? fmt("%s = %s & %s;", R(a).c_str(), R(d).c_str(), U(m).c_str())
                    : fmt("%s = gcnr_rotl(%s, %d) & %s;", R(a).c_str(), R(d).c_str(), b, U(m).c_str()));
        rc0(i, a);
        break;
    }
    case Op::Rlwimi:
    {
        const uint32_t m = ppc_mask(cc, i.e);
        line(fmt("%s = (gcnr_rotl(%s, %d) & %s) | (%s & %s);", R(a).c_str(), R(d).c_str(), b, U(m).c_str(),
                 R(a).c_str(), U(~m).c_str()));
        rc0(i, a);
        break;
    }
    case Op::Rlwnm:
        line(fmt("%s = gcnr_rotl(%s, %s) & %s;", R(a).c_str(), R(d).c_str(), R(b).c_str(), U(ppc_mask(cc, i.e)).c_str()));
        rc0(i, a);
        break;
    case Op::Slw:
        line(fmt("%s = (%s & 0x20u) ? 0u : %s << (%s & 31u);", R(a).c_str(), R(b).c_str(), R(d).c_str(), R(b).c_str()));
        rc0(i, a);
        break;
    case Op::Srw:
        line(fmt("%s = (%s & 0x20u) ? 0u : %s >> (%s & 31u);", R(a).c_str(), R(b).c_str(), R(d).c_str(), R(b).c_str()));
        rc0(i, a);
        break;
    case Op::Sraw:
        line(fmt("%s = gcnr_sraw(c, %s, %s & 0x3Fu);", R(a).c_str(), R(d).c_str(), R(b).c_str()));
        rc0(i, a);
        break;
    case Op::Srawi:
        line(fmt("%s = gcnr_sraw(c, %s, %du);", R(a).c_str(), R(d).c_str(), b));
        rc0(i, a);
        break;

    // ---- condition register ----
    case Op::Crand: case Op::Crandc: case Op::Creqv: case Op::Crnand: case Op::Crnor: case Op::Cror:
    case Op::Crorc: case Op::Crxor:
    {
        const std::string x = fmt("GCNR_CRBIT(c, %d)", a), y = fmt("GCNR_CRBIT(c, %d)", b);
        std::string e;
        switch (i.op)
        {
        case Op::Crand: e = x + " & " + y; break;
        case Op::Crandc: e = x + " & (1u ^ " + y + ")"; break;
        case Op::Creqv: e = "1u ^ (" + x + " ^ " + y + ")"; break;
        case Op::Crnand: e = "1u ^ (" + x + " & " + y + ")"; break;
        case Op::Crnor: e = "1u ^ (" + x + " | " + y + ")"; break;
        case Op::Cror: e = x + " | " + y; break;
        case Op::Crorc: e = x + " | (1u ^ " + y + ")"; break;
        default: e = x + " ^ " + y; break;
        }
        line(fmt("gcnr_setcrbit(c, %d, %s);", d, e.c_str()));
        break;
    }
    case Op::Mcrf:
        line(fmt("gcnr_setcrf(c, %d, c->cr >> %d);", d >> 2, 28 - 4 * (a >> 2)));
        break;
    case Op::Mcrxr:
        line(fmt("gcnr_setcrf(c, %d, (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1));", d >> 2));
        line("c->xer_so = c->xer_ov = c->xer_ca = 0;");
        break;
    case Op::Mfcr:
        line(fmt("%s = c->cr;", R(d).c_str()));
        break;
    case Op::Mtcrf:
    {
        const uint32_t m = field_mask(i.fxm);
        line(fmt("c->cr = (c->cr & %s) | (%s & %s);", U(~m).c_str(), R(d).c_str(), U(m).c_str()));
        break;
    }

    // ---- floating point ----
    case Op::Fadd: line(fmt("%s = %s + %s;", F0(d).c_str(), F0(a).c_str(), F0(b).c_str())); rc1(i); break;
    case Op::Fsub: line(fmt("%s = %s - %s;", F0(d).c_str(), F0(a).c_str(), F0(b).c_str())); rc1(i); break;
    case Op::Fmul: line(fmt("%s = %s * %s;", F0(d).c_str(), F0(a).c_str(), F0(cc).c_str())); rc1(i); break;
    case Op::Fdiv: line(fmt("%s = %s / %s;", F0(d).c_str(), F0(a).c_str(), F0(b).c_str())); rc1(i); break;
    case Op::Fadds: case Op::Fsubs: case Op::Fmuls: case Op::Fdivs: case Op::Fmadds: case Op::Fmsubs:
    case Op::Fnmadds: case Op::Fnmsubs: case Op::Fres: case Op::Frsp:
    {
        std::string e;
        switch (i.op)
        {
        case Op::Fadds: e = F0(a) + " + " + F0(b); break;
        case Op::Fsubs: e = F0(a) + " - " + F0(b); break;
        case Op::Fmuls: e = F0(a) + " * " + F0(cc); break;
        case Op::Fdivs: e = F0(a) + " / " + F0(b); break;
        case Op::Fmadds: e = "GCNR_MADD(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ")"; break;
        case Op::Fmsubs: e = "GCNR_MSUB(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ")"; break;
        case Op::Fnmadds: e = "-GCNR_MADD(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ")"; break;
        case Op::Fnmsubs: e = "-GCNR_MSUB(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ")"; break;
        case Op::Fres: e = "gcnr_fres(" + F0(b) + ")"; break;
        default: e = F0(b); break; // frsp
        }
        // single-precision results go to both halves (paired-single mode)
        line("{ const double t = GCNR_SINGLE(" + e + "); " + F0(d) + " = t; " + F1(d) + " = t; }");
        rc1(i);
        break;
    }
    case Op::Fmadd: line(F0(d) + " = GCNR_MADD(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ");"); rc1(i); break;
    case Op::Fmsub: line(F0(d) + " = GCNR_MSUB(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ");"); rc1(i); break;
    case Op::Fnmadd: line(F0(d) + " = -GCNR_MADD(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ");"); rc1(i); break;
    case Op::Fnmsub: line(F0(d) + " = -GCNR_MSUB(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ");"); rc1(i); break;
    case Op::Frsqrte: line(F0(d) + " = gcnr_frsqrte(" + F0(b) + ");"); rc1(i); break;
    case Op::Fsel: line(F0(d) + " = gcnr_fsel(" + F0(a) + ", " + F0(cc) + ", " + F0(b) + ");"); rc1(i); break;
    case Op::Fabs: line(F0(d) + " = fabs(" + F0(b) + ");"); rc1(i); break;
    case Op::Fnabs: line(F0(d) + " = -fabs(" + F0(b) + ");"); rc1(i); break;
    case Op::Fneg: line(F0(d) + " = -" + F0(b) + ";"); rc1(i); break;
    case Op::Fmr: line(F0(d) + " = " + F0(b) + ";"); rc1(i); break;
    case Op::Fctiw: line(F0(d) + " = gcnr_fctiw(" + F0(b) + ", 0);"); rc1(i); break;
    case Op::Fctiwz: line(F0(d) + " = gcnr_fctiw(" + F0(b) + ", 1);"); rc1(i); break;
    case Op::Fcmpu: case Op::Fcmpo:
        line(fmt("gcnr_fcmp(c, %d, %s, %s);", i.crfd(), F0(a).c_str(), F0(b).c_str()));
        break;
    case Op::Mffs:
        line(F0(d) + " = gcnr_f64(0xFFF8000000000000ull | c->fpscr);");
        rc1(i);
        break;
    case Op::Mtfsf:
    {
        const uint32_t m = field_mask(i.fxm);
        line(fmt("c->fpscr = (c->fpscr & %s) | ((uint32_t)gcnr_bits64(%s) & %s);", U(~m).c_str(), F0(b).c_str(), U(m).c_str()));
        rc1(i);
        break;
    }
    case Op::Mtfsfi:
    {
        const int sh = 28 - 4 * i.crfd();
        line(fmt("c->fpscr = (c->fpscr & %s) | %s;", U(~(0xFu << sh)).c_str(), U(((i.word >> 12) & 15u) << sh).c_str()));
        rc1(i);
        break;
    }
    case Op::Mtfsb0: line(fmt("c->fpscr &= %s;", U(~(0x80000000u >> d)).c_str())); rc1(i); break;
    case Op::Mtfsb1: line(fmt("c->fpscr |= %s;", U(0x80000000u >> d).c_str())); rc1(i); break;
    case Op::Mcrfs:
        line(fmt("gcnr_setcrf(c, %d, c->fpscr >> %d);", d >> 2, 28 - 4 * (a >> 2)));
        break;

    // ---- paired singles ----
    case Op::PsAdd: case Op::PsSub: case Op::PsMul: case Op::PsDiv: case Op::PsMadd: case Op::PsMsub:
    case Op::PsNmadd: case Op::PsNmsub: case Op::PsSum0: case Op::PsSum1: case Op::PsMuls0: case Op::PsMuls1:
    case Op::PsMadds0: case Op::PsMadds1: case Op::PsSel: case Op::PsRes: case Op::PsRsqrte: case Op::PsNeg:
    case Op::PsAbs: case Op::PsNabs: case Op::PsMr: case Op::PsMerge00: case Op::PsMerge01: case Op::PsMerge10:
    case Op::PsMerge11:
    {
        const std::string a0 = F0(a), a1 = F1(a), b0 = F0(b), b1 = F1(b), c0 = F0(cc), c1 = F1(cc);
        std::string e0, e1;
        bool round = true;
        switch (i.op)
        {
        case Op::PsAdd: e0 = a0 + " + " + b0; e1 = a1 + " + " + b1; break;
        case Op::PsSub: e0 = a0 + " - " + b0; e1 = a1 + " - " + b1; break;
        case Op::PsMul: e0 = a0 + " * " + c0; e1 = a1 + " * " + c1; break;
        case Op::PsDiv: e0 = a0 + " / " + b0; e1 = a1 + " / " + b1; break;
        case Op::PsMadd: e0 = "GCNR_MADD(" + a0 + ", " + c0 + ", " + b0 + ")"; e1 = "GCNR_MADD(" + a1 + ", " + c1 + ", " + b1 + ")"; break;
        case Op::PsMsub: e0 = "GCNR_MSUB(" + a0 + ", " + c0 + ", " + b0 + ")"; e1 = "GCNR_MSUB(" + a1 + ", " + c1 + ", " + b1 + ")"; break;
        case Op::PsNmadd: e0 = "-GCNR_MADD(" + a0 + ", " + c0 + ", " + b0 + ")"; e1 = "-GCNR_MADD(" + a1 + ", " + c1 + ", " + b1 + ")"; break;
        case Op::PsNmsub: e0 = "-GCNR_MSUB(" + a0 + ", " + c0 + ", " + b0 + ")"; e1 = "-GCNR_MSUB(" + a1 + ", " + c1 + ", " + b1 + ")"; break;
        case Op::PsSum0: e0 = a0 + " + " + b1; e1 = c1; break;
        case Op::PsSum1: e0 = c0; e1 = a0 + " + " + b1; break;
        case Op::PsMuls0: e0 = a0 + " * " + c0; e1 = a1 + " * " + c0; break;
        case Op::PsMuls1: e0 = a0 + " * " + c1; e1 = a1 + " * " + c1; break;
        case Op::PsMadds0: e0 = "GCNR_MADD(" + a0 + ", " + c0 + ", " + b0 + ")"; e1 = "GCNR_MADD(" + a1 + ", " + c0 + ", " + b1 + ")"; break;
        case Op::PsMadds1: e0 = "GCNR_MADD(" + a0 + ", " + c1 + ", " + b0 + ")"; e1 = "GCNR_MADD(" + a1 + ", " + c1 + ", " + b1 + ")"; break;
        case Op::PsSel: e0 = "gcnr_fsel(" + a0 + ", " + c0 + ", " + b0 + ")"; e1 = "gcnr_fsel(" + a1 + ", " + c1 + ", " + b1 + ")"; round = false; break;
        case Op::PsRes: e0 = "gcnr_fres(" + b0 + ")"; e1 = "gcnr_fres(" + b1 + ")"; break;
        case Op::PsRsqrte: e0 = "gcnr_frsqrte(" + b0 + ")"; e1 = "gcnr_frsqrte(" + b1 + ")"; break;
        case Op::PsNeg: e0 = "-" + b0; e1 = "-" + b1; round = false; break;
        case Op::PsAbs: e0 = "fabs(" + b0 + ")"; e1 = "fabs(" + b1 + ")"; round = false; break;
        case Op::PsNabs: e0 = "-fabs(" + b0 + ")"; e1 = "-fabs(" + b1 + ")"; round = false; break;
        case Op::PsMr: e0 = b0; e1 = b1; round = false; break;
        case Op::PsMerge00: e0 = a0; e1 = b0; round = false; break;
        case Op::PsMerge01: e0 = a0; e1 = b1; round = false; break;
        case Op::PsMerge10: e0 = a1; e1 = b0; round = false; break;
        default: e0 = a1; e1 = b1; round = false; break; // merge11
        }
        if (round)
        {
            e0 = "GCNR_SINGLE(" + e0 + ")";
            e1 = "GCNR_SINGLE(" + e1 + ")";
        }
        line("{ const double t0 = " + e0 + ", t1 = " + e1 + "; " + F0(d) + " = t0; " + F1(d) + " = t1; }");
        rc1(i);
        break;
    }
    case Op::PsCmpu0: case Op::PsCmpo0:
        line(fmt("gcnr_fcmp(c, %d, %s, %s);", i.crfd(), F0(a).c_str(), F0(b).c_str()));
        break;
    case Op::PsCmpu1: case Op::PsCmpo1:
        line(fmt("gcnr_fcmp(c, %d, %s, %s);", i.crfd(), F1(a).c_str(), F1(b).c_str()));
        break;

    // ---- integer loads / stores ----
    case Op::Lbz: load(i, "gcnr_lbz", 1, false, false); break;
    case Op::Lbzu: load(i, "gcnr_lbz", 1, false, true); break;
    case Op::Lbzx: load(i, "gcnr_lbz", 1, true, false); break;
    case Op::Lbzux: load(i, "gcnr_lbz", 1, true, true); break;
    case Op::Lhz: load(i, "gcnr_lhz", 2, false, false); break;
    case Op::Lhzu: load(i, "gcnr_lhz", 2, false, true); break;
    case Op::Lhzx: load(i, "gcnr_lhz", 2, true, false); break;
    case Op::Lhzux: load(i, "gcnr_lhz", 2, true, true); break;
    case Op::Lha: load(i, "gcnr_lha", 2, false, false, true); break;
    case Op::Lhau: load(i, "gcnr_lha", 2, false, true, true); break;
    case Op::Lhax: load(i, "gcnr_lha", 2, true, false, true); break;
    case Op::Lhaux: load(i, "gcnr_lha", 2, true, true, true); break;
    case Op::Lwz: load(i, "gcnr_lw", 4, false, false); break;
    case Op::Lwzu: load(i, "gcnr_lw", 4, false, true); break;
    case Op::Lwzx: load(i, "gcnr_lw", 4, true, false); break;
    case Op::Lwzux: load(i, "gcnr_lw", 4, true, true); break;
    case Op::Lhbrx: line(R(d) + " = gcnr_lhbr(mem, " + ea_x(i) + ");"); break;
    case Op::Lwbrx: line(R(d) + " = gcnr_lwbr(mem, " + ea_x(i) + ");"); break;
    case Op::Stb: store(i, "gcnr_sb", 1, false, false); break;
    case Op::Stbu: store(i, "gcnr_sb", 1, false, true); break;
    case Op::Stbx: store(i, "gcnr_sb", 1, true, false); break;
    case Op::Stbux: store(i, "gcnr_sb", 1, true, true); break;
    case Op::Sth: store(i, "gcnr_sh", 2, false, false); break;
    case Op::Sthu: store(i, "gcnr_sh", 2, false, true); break;
    case Op::Sthx: store(i, "gcnr_sh", 2, true, false); break;
    case Op::Sthux: store(i, "gcnr_sh", 2, true, true); break;
    case Op::Stw: store(i, "gcnr_sw", 4, false, false); break;
    case Op::Stwu: store(i, "gcnr_sw", 4, false, true); break;
    case Op::Stwx: store(i, "gcnr_sw", 4, true, false); break;
    case Op::Stwux: store(i, "gcnr_sw", 4, true, true); break;
    case Op::Sthbrx: line("gcnr_shbr(mem, " + ea_x(i) + ", " + R(d) + ");"); break;
    case Op::Stwbrx: line("gcnr_swbr(mem, " + ea_x(i) + ", " + R(d) + ");"); break;
    case Op::Lmw:
        line("{ uint32_t ea = " + ea_d(i) + ";");
        line(fmt("  for (int k = %d; k < 32; k++, ea += 4) c->r[k] = gcnr_lw(mem, ea); }", d));
        break;
    case Op::Stmw:
        line("{ uint32_t ea = " + ea_d(i) + ";");
        line(fmt("  for (int k = %d; k < 32; k++, ea += 4) gcnr_sw(mem, ea, c->r[k]); }", d));
        break;
    case Op::Lswi: line(fmt("gcnr_lsw(mem, c, %d, %s, %du);", d, RA0(a).c_str(), b ? b : 32)); break;
    case Op::Stswi: line(fmt("gcnr_stsw(mem, c, %d, %s, %du);", d, RA0(a).c_str(), b ? b : 32)); break;
    case Op::Lswx: line(fmt("gcnr_lsw(mem, c, %d, %s, c->xer_bc);", d, ea_x(i).c_str())); break;
    case Op::Stswx: line(fmt("gcnr_stsw(mem, c, %d, %s, c->xer_bc);", d, ea_x(i).c_str())); break;
    case Op::Lwarx:
        line(R(d) + " = gcnr_lw(mem, " + ea_x(i) + ");");
        line("c->reserve = 1;");
        break;
    case Op::StwcxRc:
        line("gcnr_sw(mem, " + ea_x(i) + ", " + R(d) + ");");
        line("c->reserve = 0;");
        line("gcnr_setcrf(c, 0, 2u | c->xer_so);");
        break;

    // ---- floating point loads / stores ----
    case Op::Lfs: case Op::Lfsu: case Op::Lfsx: case Op::Lfsux:
    {
        const bool x = i.op == Op::Lfsx || i.op == Op::Lfsux, u = i.op == Op::Lfsu || i.op == Op::Lfsux;
        line("{ const uint32_t ea = " + (x ? ea_x(i) : ea_d(i)) + ";");
        line("  const double t = (double)gcnr_f32(gcnr_lw(mem, ea)); " + F0(d) + " = t; " + F1(d) + " = t;");
        line(u ? "  " + R(a) + " = ea; }" : std::string("}"));
        break;
    }
    case Op::Lfd: case Op::Lfdu: case Op::Lfdx: case Op::Lfdux:
    {
        const bool x = i.op == Op::Lfdx || i.op == Op::Lfdux, u = i.op == Op::Lfdu || i.op == Op::Lfdux;
        line("{ const uint32_t ea = " + (x ? ea_x(i) : ea_d(i)) + ";");
        line("  " + F0(d) + " = gcnr_f64(gcnr_ld(mem, ea));");
        line(u ? "  " + R(a) + " = ea; }" : std::string("}"));
        break;
    }
    case Op::Stfs: case Op::Stfsu: case Op::Stfsx: case Op::Stfsux:
    {
        const bool x = i.op == Op::Stfsx || i.op == Op::Stfsux, u = i.op == Op::Stfsu || i.op == Op::Stfsux;
        uint32_t hw = 0;
        if (mmio(i, hw))
        {
            line(fmt("gcnr_mmio_write(0x%08Xu, gcnr_bits32((float)%s), 4);", hw, F0(d).c_str()));
            if (u) line(R(a) + " = " + U(hw) + ";");
            break;
        }
        line("{ const uint32_t ea = " + (x ? ea_x(i) : ea_d(i)) + ";");
        line("  gcnr_sw(mem, ea, gcnr_bits32((float)" + F0(d) + "));");
        line(u ? "  " + R(a) + " = ea; }" : std::string("}"));
        break;
    }
    case Op::Stfd: case Op::Stfdu: case Op::Stfdx: case Op::Stfdux:
    {
        const bool x = i.op == Op::Stfdx || i.op == Op::Stfdux, u = i.op == Op::Stfdu || i.op == Op::Stfdux;
        uint32_t hw = 0;
        if (mmio(i, hw))
        {
            line(fmt("{ const uint64_t t = gcnr_bits64(%s); gcnr_mmio_write(0x%08Xu, (uint32_t)(t >> 32), 4); "
                     "gcnr_mmio_write(0x%08Xu, (uint32_t)t, 4); }", F0(d).c_str(), hw, hw + 4));
            if (u) line(R(a) + " = " + U(hw) + ";");
            break;
        }
        line("{ const uint32_t ea = " + (x ? ea_x(i) : ea_d(i)) + ";");
        line("  gcnr_sd(mem, ea, gcnr_bits64(" + F0(d) + "));");
        line(u ? "  " + R(a) + " = ea; }" : std::string("}"));
        break;
    }
    case Op::Stfiwx:
    {
        uint32_t hw = 0;
        if (mmio(i, hw))
        {
            line(fmt("gcnr_mmio_write(0x%08Xu, (uint32_t)gcnr_bits64(%s), 4);", hw, F0(d).c_str()));
            break;
        }
        line("gcnr_sw(mem, " + ea_x(i) + ", (uint32_t)gcnr_bits64(" + F0(d) + "));");
        break;
    }

    // ---- paired-single loads / stores ----
    case Op::PsqL: case Op::PsqLu: case Op::PsqLx: case Op::PsqLux:
    {
        const bool x = i.op == Op::PsqLx || i.op == Op::PsqLux, u = i.op == Op::PsqLu || i.op == Op::PsqLux;
        line("{ const uint32_t ea = " + (x ? ea_x(i) : ea_d(i)) + ";");
        line(fmt("  gcnr_psq_l(mem, c, %d, ea, %d, %d);", d, i.psw, i.psi));
        line(u ? "  " + R(a) + " = ea; }" : std::string("}"));
        break;
    }
    case Op::PsqSt: case Op::PsqStu: case Op::PsqStx: case Op::PsqStux:
    {
        const bool x = i.op == Op::PsqStx || i.op == Op::PsqStux, u = i.op == Op::PsqStu || i.op == Op::PsqStux;
        uint32_t hw = 0;
        if (mmio(i, hw))
        {
            line(fmt("gcnr_psq_st_mmio(c, %d, 0x%08Xu, %d, %d);", d, hw, i.psw, i.psi));
            if (u) line(R(a) + " = " + U(hw) + ";");
            break;
        }
        line("{ const uint32_t ea = " + (x ? ea_x(i) : ea_d(i)) + ";");
        line(fmt("  gcnr_psq_st(mem, c, %d, ea, %d, %d);", d, i.psw, i.psi));
        line(u ? "  " + R(a) + " = ea; }" : std::string("}"));
        break;
    }
    case Op::Dcbz: case Op::DcbzL:
        line("gcnr_dcbz(mem, " + (a == 0 ? R(b) : R(a) + " + " + R(b)) + ");");
        break;

    // ---- branches ----
    case Op::B:
        if (i.lk && mAn.lrMode && i.target != next && mAn.local(i.target))
        {
            line(fmt("c->lr = 0x%08Xu;", next));
            line(fmt("goto L_%08X;", i.target));
        }
        else if (i.lk)
        {
            line(fmt("c->lr = 0x%08Xu;", next));
            if (i.target != next)
            {
                if (mO.checkSp || mO.traced.count(mAn.fn->addr))
                {
                    line(fmt("{ const uint32_t sp_ = c->r[1]; %s if (c->r[1] != sp_) gcnr_sp_changed(0x%08Xu, 0x%08Xu, sp_, c->r[1]); }",
                             call_target(i.target).c_str(), i.addr, i.target));
                }
                else
                {
                    line(call_target(i.target));
                }
            }
        }
        else if (mAn.local(i.target))
        {
            if (!loop_check(i.addr).empty()) line(loop_check(i.addr));
            line(fmt("goto L_%08X;", i.target));
        }
        else
        {
            line(call_target(i.target));
            line("return;");
        }
        break;
    case Op::Bc:
    {
        std::string pre;
        const std::string c = cond(i, pre);
        if (!pre.empty()) line(pre);
        if (i.lk && mAn.lrMode && i.target != next && mAn.local(i.target))
        {
            line("if (" + c + ") { c->lr = " + U(next) + fmt("; goto L_%08X; }", i.target));
        }
        else if (i.lk)
        {
            line("if (" + c + ") { c->lr = " + U(next) + "; " + call_target(i.target) + " }");
        }
        else if (mAn.local(i.target))
        {
            const std::string loop = loop_check(i.addr);
            line("if (" + c + ") { " + loop + fmt("goto L_%08X; }", i.target));
        }
        else
        {
            line("if (" + c + ") { " + call_target(i.target) + " return; }");
        }
        break;
    }
    case Op::Bclr:
    {
        std::string pre;
        const std::string c = cond(i, pre);
        if (!pre.empty()) line(pre);
        if (i.lk)
        {
            line("if (" + c + ") { const uint32_t t = c->lr; c->lr = " + U(next) + "; GCNR_CALL_INDIRECT(mem, c, t); }");
        }
        else if (mAn.lrMode && !mAn.lrSites.empty())
        {
            std::string sw = "switch (c->lr) {";
            for (uint32_t s : mAn.lrSites)
            {
                sw += fmt(" case 0x%08Xu: goto L_%08X;", s, s);
            }
            sw += " default: return; }";
            line(c == "1" ? sw : "if (" + c + ") { " + sw + " }");
        }
        else
        {
            line(c == "1" ? std::string("return;") : "if (" + c + ") return;");
        }
        break;
    }
    case Op::Bcctr:
    {
        std::string pre;
        const std::string c = cond(i, pre);
        if (!pre.empty()) line(pre);
        if (i.lk)
        {
            if (mO.checkSp || mO.traced.count(mAn.fn->addr))
            {
                line("if (" + c + ") { const uint32_t sp_ = c->r[1], t_ = c->ctr; c->lr = " + U(next) +
                     "; GCNR_CALL_INDIRECT(mem, c, t_); if (c->r[1] != sp_) gcnr_sp_changed(" + U(i.addr) +
                     ", t_, sp_, c->r[1]); }");
            }
            else
            {
                line("if (" + c + ") { c->lr = " + U(next) + "; GCNR_CALL_INDIRECT(mem, c, c->ctr); }");
            }
            break;
        }
        auto sw = mAn.switches.find(i.addr);
        if (sw != mAn.switches.end())
        {
            line("if (" + c + ") {");
            line("  switch (c->ctr) {");
            std::set<uint32_t> done;
            for (uint32_t t : sw->second)
            {
                if (done.insert(t).second)
                {
                    line(fmt("  case 0x%08Xu: goto L_%08X;", t, t));
                }
            }
            line(fmt("  default: gcnr_unhandled(c, 0x%08Xu, \"jump table target\"); return;", i.addr));
            line("  }");
            line("}");
        }
        else
        {
            line("if (" + c + ") { GCNR_CALL_INDIRECT(mem, c, c->ctr); return; }");
        }
        break;
    }

    // ---- system ----
    case Op::Mfspr:
        switch (i.spr)
        {
        case 1: line(R(d) + " = gcnr_xer(c);"); break;
        case 8: line(R(d) + " = c->lr;"); break;
        case 9: line(R(d) + " = c->ctr;"); break;
        default:
            if (i.spr >= 912 && i.spr <= 919) line(fmt("%s = gcnr_gqr[%d];", R(d).c_str(), i.spr - 912));
            else line(fmt("%s = gcnr_mfspr(c, %du);", R(d).c_str(), i.spr));
            break;
        }
        break;
    case Op::Mtspr:
        switch (i.spr)
        {
        case 1: line("gcnr_setxer(c, " + R(d) + ");"); break;
        case 8: line("c->lr = " + R(d) + ";"); break;
        case 9: line("c->ctr = " + R(d) + ";"); break;
        default:
            if (i.spr >= 912 && i.spr <= 919) line(fmt("gcnr_gqr[%d] = %s;", i.spr - 912, R(d).c_str()));
            else line(fmt("gcnr_mtspr(c, %du, %s);", i.spr, R(d).c_str()));
            break;
        }
        break;
    case Op::Mftb:
        line(i.spr == 269 ? R(d) + " = (uint32_t)(gcnr_timebase() >> 32);" : R(d) + " = (uint32_t)gcnr_timebase();");
        break;
    case Op::Mfmsr: line(R(d) + " = c->msr;"); break;
    case Op::Mtmsr: line("c->msr = " + R(d) + ";"); break;
    case Op::Mfsr: case Op::Mfsrin: line(R(d) + " = 0u;"); break;
    case Op::Mtsr: case Op::Mtsrin: case Op::Tlbie: case Op::Tlbsync: case Op::Sync: case Op::Isync: case Op::Eieio:
    case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Icbi:
        break; // no effect here
    case Op::Sc: line("gcnr_syscall(mem, c);"); break;
    case Op::Rfi: line(fmt("gcnr_unhandled(c, 0x%08Xu, \"rfi\"); return;", i.addr)); break;
    case Op::Tw: case Op::Twi:
    {
        const int to = d;
        const std::string x = "(int32_t)" + R(a);
        const std::string y = i.op == Op::Twi ? fmt("%d", i.simm) : "(int32_t)" + R(b);
        const std::string ux = R(a), uy = i.op == Op::Twi ? U((uint32_t)i.simm) : R(b);
        std::string e;
        auto add = [&](const std::string& t) { e += (e.empty() ? "" : " || ") + t; };
        if (to == 31) e = "1";
        else
        {
            if (to & 16) add(x + " < " + y);
            if (to & 8) add(x + " > " + y);
            if (to & 4) add(x + " == " + y);
            if (to & 2) add(ux + " < " + uy);
            if (to & 1) add(ux + " > " + uy);
        }
        if (!e.empty()) line("if (" + e + ") gcnr_trap(c, " + U(i.addr) + ");");
        break;
    }
    case Op::Eciwx: case Op::Ecowx:
        line(fmt("gcnr_unhandled(c, 0x%08Xu, \"external control\");", i.addr));
        break;
    case Op::Invalid:
    default:
        line(fmt("gcnr_unhandled(c, 0x%08Xu, \"invalid instruction 0x%08X\");", i.addr, i.word));
        break;
    }
}
} // namespace

void CGenerator::function(const Program& program, const Analysis& analysis, std::string& out)
{
    Emitter(program, analysis, mOptions, out).run();
}
} // namespace gcnr
