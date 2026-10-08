/**
 * @file live_exec.cpp
 * @brief gcnl_exec: one non-branch instruction with the C generator's semantics (see live.h).
 *
 * Each case is cgen.cpp's emitted C, as a statement on the decoded fields; keep the two in step.
 * Compiled with GCNR_FMA=0 (Live builds multiply, then add).
 */
#include "live.h"

using gekko::Insn;
using gekko::Op;

namespace
{
uint32_t ppc_mask(int mb, int me)
{
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = (me >= 31) ? 0u : (0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & ~b) : (a | ~b);
}

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

inline uint32_t ea_d(const gcnr_ctx* c, const Insn& i)
{
    return i.a == 0 ? (uint32_t)i.simm : c->r[i.a] + (uint32_t)i.simm;
}

inline uint32_t ea_x(const gcnr_ctx* c, const Insn& i)
{
    return i.a == 0 ? c->r[i.b] : c->r[i.a] + c->r[i.b];
}

typedef uint32_t (*LoadFn)(const uint8_t*, uint32_t);
typedef void (*StoreFn)(uint8_t*, uint32_t, uint32_t);

inline void load(uint8_t* mem, gcnr_ctx* c, const gcnl_insn* li, LoadFn fn, int bytes, bool xform, bool update,
                 bool sign = false)
{
    const Insn& i = li->i;
    const uint32_t ea = xform ? ea_x(c, i) : ea_d(c, i);
    uint32_t value;
    if (li->hw)
    {
        value = gcnr_mmio_read(li->hw, bytes);
        if (sign)
        {
            value = (uint32_t)(int32_t)(int16_t)value;
        }
    }
    else
    {
        value = fn(mem, ea);
    }
    c->r[i.d] = value;
    if (update)
    {
        c->r[i.a] = ea;
    }
}

inline void store(uint8_t* mem, gcnr_ctx* c, const gcnl_insn* li, StoreFn fn, int bytes, bool xform, bool update)
{
    const Insn& i = li->i;
    if (li->hw)
    {
        gcnr_mmio_write(li->hw, c->r[i.d], bytes);
        if (update)
        {
            c->r[i.a] = li->hw;
        }
        return;
    }
    const uint32_t ea = xform ? ea_x(c, i) : ea_d(c, i);
    fn(mem, ea, c->r[i.d]);
    if (update)
    {
        c->r[i.a] = ea;
    }
}

inline void rc0(gcnr_ctx* c, const Insn& i, int reg)
{
    if (i.rc) GCNR_CR0(c, c->r[reg]);
}

inline void rc1(gcnr_ctx* c, const Insn& i)
{
    if (i.rc) GCNR_CR1(c);
}

inline void single(gcnr_ctx* c, int d, double e)
{
    const double t = GCNR_SINGLE(e);
    c->f[d].ps0 = t;
    c->f[d].ps1 = t;
}
} // namespace

// ---- what live_gen.cpp's code calls besides gcnl_exec ---------------------------------------
extern "C" {
// GCNR_CALL_INDIRECT: a call to a computed address (bctrl, blrl, a branch out of the code)
void gcnl_call(uint8_t* mem, gcnr_ctx* c, uint32_t target)
{
    GCNR_CALL_INDIRECT(mem, c, target);
}

// the inline loads' and stores' slow paths (addresses from 0xC0000000: the uncached RAM mirror,
// hardware registers, the locked cache)
uint32_t gcnl_lw(const uint8_t* mem, uint32_t a) { return gcnr_lw(mem, a); }
uint32_t gcnl_lhz(const uint8_t* mem, uint32_t a) { return gcnr_lhz(mem, a); }
uint32_t gcnl_lha(const uint8_t* mem, uint32_t a) { return gcnr_lha(mem, a); }
uint32_t gcnl_lbz(const uint8_t* mem, uint32_t a) { return gcnr_lbz(mem, a); }
uint64_t gcnl_ld(const uint8_t* mem, uint32_t a) { return gcnr_ld(mem, a); }
void gcnl_sw(uint8_t* mem, uint32_t a, uint32_t v) { gcnr_sw(mem, a, v); }
void gcnl_sh(uint8_t* mem, uint32_t a, uint32_t v) { gcnr_sh(mem, a, v); }
void gcnl_sb(uint8_t* mem, uint32_t a, uint32_t v) { gcnr_sb(mem, a, v); }
void gcnl_sd(uint8_t* mem, uint32_t a, uint64_t v) { gcnr_sd(mem, a, v); }

// code another module's link rewrites (live_gen.cpp: LiveInputs::dynamic)
void gcnl_exec_at(uint8_t* mem, gcnr_ctx* c, uint32_t addr)
{
    gcnl_insn li;
    li.i = gekko::decode(gcnr_lw(mem, addr), addr);
    gcnl_exec(mem, c, &li);
}

void gcnl_call_insn(uint8_t* mem, gcnr_ctx* c, uint32_t addr)
{
    const gekko::Insn i = gekko::decode(gcnr_lw(mem, addr), addr);
    GCNR_CALL_INDIRECT(mem, c, i.target);
}
}

#if GCNR_WATCH
extern "C" uint32_t gcnr_watch_pc; // recomp_gcn.c: the guest instruction a watch hit reports
#endif

extern "C" void gcnl_exec(uint8_t* mem, gcnr_ctx* c, const gcnl_insn* li)
{
    const Insn& i = li->i;
#if GCNR_WATCH
    gcnr_watch_pc = i.addr;
#endif
    const int d = i.d, a = i.a, b = i.b, cc = i.c;
    uint32_t* r = c->r;
    gcnr_fpr* f = c->f;
    switch (i.op)
    {
    // ---- integer arithmetic ----
    case Op::Add: r[d] = i.oe ? gcnr_add(c, r[a], r[b], 0, 0, 1) : r[a] + r[b]; rc0(c, i, d); break;
    case Op::Addc: r[d] = gcnr_add(c, r[a], r[b], 0, 1, i.oe); rc0(c, i, d); break;
    case Op::Adde: r[d] = gcnr_add(c, r[a], r[b], c->xer_ca, 1, i.oe); rc0(c, i, d); break;
    case Op::Addze: r[d] = gcnr_add(c, r[a], 0u, c->xer_ca, 1, i.oe); rc0(c, i, d); break;
    case Op::Addme: r[d] = gcnr_add(c, r[a], 0xFFFFFFFFu, c->xer_ca, 1, i.oe); rc0(c, i, d); break;
    case Op::Addi: r[d] = a == 0 ? (uint32_t)i.simm : r[a] + (uint32_t)i.simm; break;
    case Op::Addis: r[d] = a == 0 ? (uint32_t)i.simm << 16 : r[a] + ((uint32_t)i.simm << 16); break;
    case Op::Addic: case Op::AddicRc:
        r[d] = gcnr_add(c, r[a], (uint32_t)i.simm, 0, 1, 0);
        if (i.op == Op::AddicRc) GCNR_CR0(c, r[d]);
        break;
    case Op::Subf: r[d] = i.oe ? gcnr_add(c, ~r[a], r[b], 1, 0, 1) : r[b] - r[a]; rc0(c, i, d); break;
    case Op::Subfc: r[d] = gcnr_add(c, ~r[a], r[b], 1, 1, i.oe); rc0(c, i, d); break;
    case Op::Subfe: r[d] = gcnr_add(c, ~r[a], r[b], c->xer_ca, 1, i.oe); rc0(c, i, d); break;
    case Op::Subfze: r[d] = gcnr_add(c, ~r[a], 0u, c->xer_ca, 1, i.oe); rc0(c, i, d); break;
    case Op::Subfme: r[d] = gcnr_add(c, ~r[a], 0xFFFFFFFFu, c->xer_ca, 1, i.oe); rc0(c, i, d); break;
    case Op::Subfic: r[d] = gcnr_add(c, ~r[a], (uint32_t)i.simm, 1, 1, 0); break;
    case Op::Neg: r[d] = i.oe ? gcnr_add(c, ~r[a], 0u, 1, 0, 1) : 0u - r[a]; rc0(c, i, d); break;
    case Op::Mulli: r[d] = r[a] * (uint32_t)i.simm; break;
    case Op::Mullw: r[d] = i.oe ? gcnr_mullw(c, r[a], r[b], 1) : r[a] * r[b]; rc0(c, i, d); break;
    case Op::Mulhw: r[d] = (uint32_t)(((int64_t)(int32_t)r[a] * (int32_t)r[b]) >> 32); rc0(c, i, d); break;
    case Op::Mulhwu: r[d] = (uint32_t)(((uint64_t)r[a] * r[b]) >> 32); rc0(c, i, d); break;
    case Op::Divw: r[d] = gcnr_divw(c, r[a], r[b], i.oe); rc0(c, i, d); break;
    case Op::Divwu: r[d] = gcnr_divwu(c, r[a], r[b], i.oe); rc0(c, i, d); break;

    // ---- compare ----
    case Op::Cmp: gcnr_cmp(c, i.crfd(), (int32_t)r[a], (int32_t)r[b]); break;
    case Op::Cmpl: gcnr_cmpl(c, i.crfd(), r[a], r[b]); break;
    case Op::Cmpi: gcnr_cmp(c, i.crfd(), (int32_t)r[a], i.simm); break;
    case Op::Cmpli: gcnr_cmpl(c, i.crfd(), r[a], i.uimm); break;

    // ---- logical ----
    case Op::And: r[a] = r[d] & r[b]; rc0(c, i, a); break;
    case Op::Andc: r[a] = r[d] & ~r[b]; rc0(c, i, a); break;
    case Op::Or: r[a] = d == b ? r[d] : r[d] | r[b]; rc0(c, i, a); break;
    case Op::Orc: r[a] = r[d] | ~r[b]; rc0(c, i, a); break;
    case Op::Xor: r[a] = r[d] ^ r[b]; rc0(c, i, a); break;
    case Op::Nand: r[a] = ~(r[d] & r[b]); rc0(c, i, a); break;
    case Op::Nor: r[a] = ~(r[d] | r[b]); rc0(c, i, a); break;
    case Op::Eqv: r[a] = ~(r[d] ^ r[b]); rc0(c, i, a); break;
    case Op::AndiRc: r[a] = r[d] & i.uimm; GCNR_CR0(c, r[a]); break;
    case Op::AndisRc: r[a] = r[d] & (i.uimm << 16); GCNR_CR0(c, r[a]); break;
    case Op::Ori: r[a] = r[d] | i.uimm; break;
    case Op::Oris: r[a] = r[d] | (i.uimm << 16); break;
    case Op::Xori: r[a] = r[d] ^ i.uimm; break;
    case Op::Xoris: r[a] = r[d] ^ (i.uimm << 16); break;
    case Op::Cntlzw: r[a] = gcnr_cntlzw(r[d]); rc0(c, i, a); break;
    case Op::Extsb: r[a] = (uint32_t)(int32_t)(int8_t)r[d]; rc0(c, i, a); break;
    case Op::Extsh: r[a] = (uint32_t)(int32_t)(int16_t)r[d]; rc0(c, i, a); break;

    // ---- rotate / shift ----
    case Op::Rlwinm: r[a] = (b == 0 ? r[d] : gcnr_rotl(r[d], (uint32_t)b)) & ppc_mask(cc, i.e); rc0(c, i, a); break;
    case Op::Rlwimi:
    {
        const uint32_t m = ppc_mask(cc, i.e);
        r[a] = (gcnr_rotl(r[d], (uint32_t)b) & m) | (r[a] & ~m);
        rc0(c, i, a);
        break;
    }
    case Op::Rlwnm: r[a] = gcnr_rotl(r[d], r[b]) & ppc_mask(cc, i.e); rc0(c, i, a); break;
    case Op::Slw: r[a] = (r[b] & 0x20u) ? 0u : r[d] << (r[b] & 31u); rc0(c, i, a); break;
    case Op::Srw: r[a] = (r[b] & 0x20u) ? 0u : r[d] >> (r[b] & 31u); rc0(c, i, a); break;
    case Op::Sraw: r[a] = gcnr_sraw(c, r[d], r[b] & 0x3Fu); rc0(c, i, a); break;
    case Op::Srawi: r[a] = gcnr_sraw(c, r[d], (uint32_t)b); rc0(c, i, a); break;

    // ---- condition register ----
    case Op::Crand: case Op::Crandc: case Op::Creqv: case Op::Crnand: case Op::Crnor: case Op::Cror:
    case Op::Crorc: case Op::Crxor:
    {
        const uint32_t x = GCNR_CRBIT(c, a), y = GCNR_CRBIT(c, b);
        uint32_t e;
        switch (i.op)
        {
        case Op::Crand: e = x & y; break;
        case Op::Crandc: e = x & (1u ^ y); break;
        case Op::Creqv: e = 1u ^ (x ^ y); break;
        case Op::Crnand: e = 1u ^ (x & y); break;
        case Op::Crnor: e = 1u ^ (x | y); break;
        case Op::Cror: e = x | y; break;
        case Op::Crorc: e = x | (1u ^ y); break;
        default: e = x ^ y; break;
        }
        gcnr_setcrbit(c, d, e);
        break;
    }
    case Op::Mcrf: gcnr_setcrf(c, d >> 2, c->cr >> (28 - 4 * (a >> 2))); break;
    case Op::Mcrxr:
        gcnr_setcrf(c, d >> 2, (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1));
        c->xer_so = c->xer_ov = c->xer_ca = 0;
        break;
    case Op::Mfcr: r[d] = c->cr; break;
    case Op::Mtcrf:
    {
        const uint32_t m = field_mask(i.fxm);
        c->cr = (c->cr & ~m) | (r[d] & m);
        break;
    }

    // ---- floating point ----
    case Op::Fadd: f[d].ps0 = f[a].ps0 + f[b].ps0; rc1(c, i); break;
    case Op::Fsub: f[d].ps0 = f[a].ps0 - f[b].ps0; rc1(c, i); break;
    case Op::Fmul: f[d].ps0 = f[a].ps0 * f[cc].ps0; rc1(c, i); break;
    case Op::Fdiv: f[d].ps0 = f[a].ps0 / f[b].ps0; rc1(c, i); break;
    case Op::Fadds: single(c, d, f[a].ps0 + f[b].ps0); rc1(c, i); break;
    case Op::Fsubs: single(c, d, f[a].ps0 - f[b].ps0); rc1(c, i); break;
    case Op::Fmuls: single(c, d, f[a].ps0 * f[cc].ps0); rc1(c, i); break;
    case Op::Fdivs: single(c, d, f[a].ps0 / f[b].ps0); rc1(c, i); break;
    case Op::Fmadds: single(c, d, GCNR_MADD(f[a].ps0, f[cc].ps0, f[b].ps0)); rc1(c, i); break;
    case Op::Fmsubs: single(c, d, GCNR_MSUB(f[a].ps0, f[cc].ps0, f[b].ps0)); rc1(c, i); break;
    case Op::Fnmadds: single(c, d, -GCNR_MADD(f[a].ps0, f[cc].ps0, f[b].ps0)); rc1(c, i); break;
    case Op::Fnmsubs: single(c, d, -GCNR_MSUB(f[a].ps0, f[cc].ps0, f[b].ps0)); rc1(c, i); break;
    case Op::Fres: single(c, d, gcnr_fres(f[b].ps0)); rc1(c, i); break;
    case Op::Frsp: single(c, d, f[b].ps0); rc1(c, i); break;
    case Op::Fmadd: f[d].ps0 = GCNR_MADD(f[a].ps0, f[cc].ps0, f[b].ps0); rc1(c, i); break;
    case Op::Fmsub: f[d].ps0 = GCNR_MSUB(f[a].ps0, f[cc].ps0, f[b].ps0); rc1(c, i); break;
    case Op::Fnmadd: f[d].ps0 = -GCNR_MADD(f[a].ps0, f[cc].ps0, f[b].ps0); rc1(c, i); break;
    case Op::Fnmsub: f[d].ps0 = -GCNR_MSUB(f[a].ps0, f[cc].ps0, f[b].ps0); rc1(c, i); break;
    case Op::Frsqrte: f[d].ps0 = gcnr_frsqrte(f[b].ps0); rc1(c, i); break;
    case Op::Fsel: f[d].ps0 = gcnr_fsel(f[a].ps0, f[cc].ps0, f[b].ps0); rc1(c, i); break;
    case Op::Fabs: f[d].ps0 = fabs(f[b].ps0); rc1(c, i); break;
    case Op::Fnabs: f[d].ps0 = -fabs(f[b].ps0); rc1(c, i); break;
    case Op::Fneg: f[d].ps0 = -f[b].ps0; rc1(c, i); break;
    case Op::Fmr: f[d].ps0 = f[b].ps0; rc1(c, i); break;
    case Op::Fctiw: f[d].ps0 = gcnr_fctiw(f[b].ps0, 0); rc1(c, i); break;
    case Op::Fctiwz: f[d].ps0 = gcnr_fctiw(f[b].ps0, 1); rc1(c, i); break;
    case Op::Fcmpu: case Op::Fcmpo: gcnr_fcmp(c, i.crfd(), f[a].ps0, f[b].ps0); break;
    case Op::Mffs: f[d].ps0 = gcnr_f64(0xFFF8000000000000ull | c->fpscr); rc1(c, i); break;
    case Op::Mtfsf:
    {
        const uint32_t m = field_mask(i.fxm);
        c->fpscr = (c->fpscr & ~m) | ((uint32_t)gcnr_bits64(f[b].ps0) & m);
        rc1(c, i);
        break;
    }
    case Op::Mtfsfi:
    {
        const int sh = 28 - 4 * i.crfd();
        c->fpscr = (c->fpscr & ~(0xFu << sh)) | (((i.word >> 12) & 15u) << sh);
        rc1(c, i);
        break;
    }
    case Op::Mtfsb0: c->fpscr &= ~(0x80000000u >> d); rc1(c, i); break;
    case Op::Mtfsb1: c->fpscr |= 0x80000000u >> d; rc1(c, i); break;
    case Op::Mcrfs: gcnr_setcrf(c, d >> 2, c->fpscr >> (28 - 4 * (a >> 2))); break;

    // ---- paired singles ----
    case Op::PsAdd: case Op::PsSub: case Op::PsMul: case Op::PsDiv: case Op::PsMadd: case Op::PsMsub:
    case Op::PsNmadd: case Op::PsNmsub: case Op::PsSum0: case Op::PsSum1: case Op::PsMuls0: case Op::PsMuls1:
    case Op::PsMadds0: case Op::PsMadds1: case Op::PsSel: case Op::PsRes: case Op::PsRsqrte: case Op::PsNeg:
    case Op::PsAbs: case Op::PsNabs: case Op::PsMr: case Op::PsMerge00: case Op::PsMerge01: case Op::PsMerge10:
    case Op::PsMerge11:
    {
        const double a0 = f[a].ps0, a1 = f[a].ps1, b0 = f[b].ps0, b1 = f[b].ps1, c0 = f[cc].ps0, c1 = f[cc].ps1;
        double t0, t1;
        bool round = true;
        switch (i.op)
        {
        case Op::PsAdd: t0 = a0 + b0; t1 = a1 + b1; break;
        case Op::PsSub: t0 = a0 - b0; t1 = a1 - b1; break;
        case Op::PsMul: t0 = a0 * c0; t1 = a1 * c1; break;
        case Op::PsDiv: t0 = a0 / b0; t1 = a1 / b1; break;
        case Op::PsMadd: t0 = GCNR_MADD(a0, c0, b0); t1 = GCNR_MADD(a1, c1, b1); break;
        case Op::PsMsub: t0 = GCNR_MSUB(a0, c0, b0); t1 = GCNR_MSUB(a1, c1, b1); break;
        case Op::PsNmadd: t0 = -GCNR_MADD(a0, c0, b0); t1 = -GCNR_MADD(a1, c1, b1); break;
        case Op::PsNmsub: t0 = -GCNR_MSUB(a0, c0, b0); t1 = -GCNR_MSUB(a1, c1, b1); break;
        case Op::PsSum0: t0 = a0 + b1; t1 = c1; break;
        case Op::PsSum1: t0 = c0; t1 = a0 + b1; break;
        case Op::PsMuls0: t0 = a0 * c0; t1 = a1 * c0; break;
        case Op::PsMuls1: t0 = a0 * c1; t1 = a1 * c1; break;
        case Op::PsMadds0: t0 = GCNR_MADD(a0, c0, b0); t1 = GCNR_MADD(a1, c0, b1); break;
        case Op::PsMadds1: t0 = GCNR_MADD(a0, c1, b0); t1 = GCNR_MADD(a1, c1, b1); break;
        case Op::PsSel: t0 = gcnr_fsel(a0, c0, b0); t1 = gcnr_fsel(a1, c1, b1); round = false; break;
        case Op::PsRes: t0 = gcnr_fres(b0); t1 = gcnr_fres(b1); break;
        case Op::PsRsqrte: t0 = gcnr_frsqrte(b0); t1 = gcnr_frsqrte(b1); break;
        case Op::PsNeg: t0 = -b0; t1 = -b1; round = false; break;
        case Op::PsAbs: t0 = fabs(b0); t1 = fabs(b1); round = false; break;
        case Op::PsNabs: t0 = -fabs(b0); t1 = -fabs(b1); round = false; break;
        case Op::PsMr: t0 = b0; t1 = b1; round = false; break;
        case Op::PsMerge00: t0 = a0; t1 = b0; round = false; break;
        case Op::PsMerge01: t0 = a0; t1 = b1; round = false; break;
        case Op::PsMerge10: t0 = a1; t1 = b0; round = false; break;
        default: t0 = a1; t1 = b1; round = false; break; // merge11
        }
        if (round)
        {
            t0 = GCNR_SINGLE(t0);
            t1 = GCNR_SINGLE(t1);
        }
        f[d].ps0 = t0;
        f[d].ps1 = t1;
        rc1(c, i);
        break;
    }
    case Op::PsCmpu0: case Op::PsCmpo0: gcnr_fcmp(c, i.crfd(), f[a].ps0, f[b].ps0); break;
    case Op::PsCmpu1: case Op::PsCmpo1: gcnr_fcmp(c, i.crfd(), f[a].ps1, f[b].ps1); break;

    // ---- integer loads / stores ----
    case Op::Lbz: load(mem, c, li, gcnr_lbz, 1, false, false); break;
    case Op::Lbzu: load(mem, c, li, gcnr_lbz, 1, false, true); break;
    case Op::Lbzx: load(mem, c, li, gcnr_lbz, 1, true, false); break;
    case Op::Lbzux: load(mem, c, li, gcnr_lbz, 1, true, true); break;
    case Op::Lhz: load(mem, c, li, gcnr_lhz, 2, false, false); break;
    case Op::Lhzu: load(mem, c, li, gcnr_lhz, 2, false, true); break;
    case Op::Lhzx: load(mem, c, li, gcnr_lhz, 2, true, false); break;
    case Op::Lhzux: load(mem, c, li, gcnr_lhz, 2, true, true); break;
    case Op::Lha: load(mem, c, li, gcnr_lha, 2, false, false, true); break;
    case Op::Lhau: load(mem, c, li, gcnr_lha, 2, false, true, true); break;
    case Op::Lhax: load(mem, c, li, gcnr_lha, 2, true, false, true); break;
    case Op::Lhaux: load(mem, c, li, gcnr_lha, 2, true, true, true); break;
    case Op::Lwz: load(mem, c, li, gcnr_lw, 4, false, false); break;
    case Op::Lwzu: load(mem, c, li, gcnr_lw, 4, false, true); break;
    case Op::Lwzx: load(mem, c, li, gcnr_lw, 4, true, false); break;
    case Op::Lwzux: load(mem, c, li, gcnr_lw, 4, true, true); break;
    case Op::Lhbrx: r[d] = gcnr_lhbr(mem, ea_x(c, i)); break;
    case Op::Lwbrx: r[d] = gcnr_lwbr(mem, ea_x(c, i)); break;
    case Op::Stb: store(mem, c, li, gcnr_sb, 1, false, false); break;
    case Op::Stbu: store(mem, c, li, gcnr_sb, 1, false, true); break;
    case Op::Stbx: store(mem, c, li, gcnr_sb, 1, true, false); break;
    case Op::Stbux: store(mem, c, li, gcnr_sb, 1, true, true); break;
    case Op::Sth: store(mem, c, li, gcnr_sh, 2, false, false); break;
    case Op::Sthu: store(mem, c, li, gcnr_sh, 2, false, true); break;
    case Op::Sthx: store(mem, c, li, gcnr_sh, 2, true, false); break;
    case Op::Sthux: store(mem, c, li, gcnr_sh, 2, true, true); break;
    case Op::Stw: store(mem, c, li, gcnr_sw, 4, false, false); break;
    case Op::Stwu: store(mem, c, li, gcnr_sw, 4, false, true); break;
    case Op::Stwx: store(mem, c, li, gcnr_sw, 4, true, false); break;
    case Op::Stwux: store(mem, c, li, gcnr_sw, 4, true, true); break;
    case Op::Sthbrx: gcnr_shbr(mem, ea_x(c, i), r[d]); break;
    case Op::Stwbrx: gcnr_swbr(mem, ea_x(c, i), r[d]); break;
    case Op::Lmw:
    {
        uint32_t ea = ea_d(c, i);
        for (int k = d; k < 32; k++, ea += 4) r[k] = gcnr_lw(mem, ea);
        break;
    }
    case Op::Stmw:
    {
        uint32_t ea = ea_d(c, i);
        for (int k = d; k < 32; k++, ea += 4) gcnr_sw(mem, ea, r[k]);
        break;
    }
    case Op::Lswi: gcnr_lsw(mem, c, d, a == 0 ? 0u : r[a], b ? (uint32_t)b : 32u); break;
    case Op::Stswi: gcnr_stsw(mem, c, d, a == 0 ? 0u : r[a], b ? (uint32_t)b : 32u); break;
    case Op::Lswx: gcnr_lsw(mem, c, d, ea_x(c, i), c->xer_bc); break;
    case Op::Stswx: gcnr_stsw(mem, c, d, ea_x(c, i), c->xer_bc); break;
    case Op::Lwarx:
        r[d] = gcnr_lw(mem, ea_x(c, i));
        c->reserve = 1;
        break;
    case Op::StwcxRc:
        gcnr_sw(mem, ea_x(c, i), r[d]);
        c->reserve = 0;
        gcnr_setcrf(c, 0, 2u | c->xer_so);
        break;

    // ---- floating point loads / stores ----
    case Op::Lfs: case Op::Lfsu: case Op::Lfsx: case Op::Lfsux:
    {
        const bool x = i.op == Op::Lfsx || i.op == Op::Lfsux, u = i.op == Op::Lfsu || i.op == Op::Lfsux;
        const uint32_t ea = x ? ea_x(c, i) : ea_d(c, i);
        const double t = (double)gcnr_f32(gcnr_lw(mem, ea));
        f[d].ps0 = t;
        f[d].ps1 = t;
        if (u) r[a] = ea;
        break;
    }
    case Op::Lfd: case Op::Lfdu: case Op::Lfdx: case Op::Lfdux:
    {
        const bool x = i.op == Op::Lfdx || i.op == Op::Lfdux, u = i.op == Op::Lfdu || i.op == Op::Lfdux;
        const uint32_t ea = x ? ea_x(c, i) : ea_d(c, i);
        f[d].ps0 = gcnr_f64(gcnr_ld(mem, ea));
        if (u) r[a] = ea;
        break;
    }
    case Op::Stfs: case Op::Stfsu: case Op::Stfsx: case Op::Stfsux:
    {
        const bool x = i.op == Op::Stfsx || i.op == Op::Stfsux, u = i.op == Op::Stfsu || i.op == Op::Stfsux;
        if (li->hw)
        {
            gcnr_mmio_write(li->hw, gcnr_bits32((float)f[d].ps0), 4);
            if (u) r[a] = li->hw;
            break;
        }
        const uint32_t ea = x ? ea_x(c, i) : ea_d(c, i);
        gcnr_sw(mem, ea, gcnr_bits32((float)f[d].ps0));
        if (u) r[a] = ea;
        break;
    }
    case Op::Stfd: case Op::Stfdu: case Op::Stfdx: case Op::Stfdux:
    {
        const bool x = i.op == Op::Stfdx || i.op == Op::Stfdux, u = i.op == Op::Stfdu || i.op == Op::Stfdux;
        if (li->hw)
        {
            const uint64_t t = gcnr_bits64(f[d].ps0);
            gcnr_mmio_write(li->hw, (uint32_t)(t >> 32), 4);
            gcnr_mmio_write(li->hw + 4, (uint32_t)t, 4);
            if (u) r[a] = li->hw;
            break;
        }
        const uint32_t ea = x ? ea_x(c, i) : ea_d(c, i);
        gcnr_sd(mem, ea, gcnr_bits64(f[d].ps0));
        if (u) r[a] = ea;
        break;
    }
    case Op::Stfiwx:
        if (li->hw)
        {
            gcnr_mmio_write(li->hw, (uint32_t)gcnr_bits64(f[d].ps0), 4);
            break;
        }
        gcnr_sw(mem, ea_x(c, i), (uint32_t)gcnr_bits64(f[d].ps0));
        break;

    // ---- paired-single loads / stores ----
    case Op::PsqL: case Op::PsqLu: case Op::PsqLx: case Op::PsqLux:
    {
        const bool x = i.op == Op::PsqLx || i.op == Op::PsqLux, u = i.op == Op::PsqLu || i.op == Op::PsqLux;
        const uint32_t ea = x ? ea_x(c, i) : ea_d(c, i);
        gcnr_psq_l(mem, c, d, ea, i.psw, i.psi);
        if (u) r[a] = ea;
        break;
    }
    case Op::PsqSt: case Op::PsqStu: case Op::PsqStx: case Op::PsqStux:
    {
        const bool x = i.op == Op::PsqStx || i.op == Op::PsqStux, u = i.op == Op::PsqStu || i.op == Op::PsqStux;
        if (li->hw)
        {
            gcnr_psq_st_mmio(c, d, li->hw, i.psw, i.psi);
            if (u) r[a] = li->hw;
            break;
        }
        const uint32_t ea = x ? ea_x(c, i) : ea_d(c, i);
        gcnr_psq_st(mem, c, d, ea, i.psw, i.psi);
        if (u) r[a] = ea;
        break;
    }
    case Op::Dcbz: case Op::DcbzL: gcnr_dcbz(mem, a == 0 ? r[b] : r[a] + r[b]); break;

    // ---- system ----
    case Op::Mfspr:
        switch (i.spr)
        {
        case 1: r[d] = gcnr_xer(c); break;
        case 8: r[d] = c->lr; break;
        case 9: r[d] = c->ctr; break;
        default: r[d] = (i.spr >= 912 && i.spr <= 919) ? gcnr_gqr[i.spr - 912] : gcnr_mfspr(c, i.spr); break;
        }
        break;
    case Op::Mtspr:
        switch (i.spr)
        {
        case 1: gcnr_setxer(c, r[d]); break;
        case 8: c->lr = r[d]; break;
        case 9: c->ctr = r[d]; break;
        default:
            if (i.spr >= 912 && i.spr <= 919) gcnr_gqr[i.spr - 912] = r[d];
            else gcnr_mtspr(c, i.spr, r[d]);
            break;
        }
        break;
    case Op::Mftb: r[d] = i.spr == 269 ? (uint32_t)(gcnr_timebase() >> 32) : (uint32_t)gcnr_timebase(); break;
    case Op::Mfmsr: r[d] = c->msr; break;
    case Op::Mtmsr: c->msr = r[d]; break;
    case Op::Mfsr: case Op::Mfsrin: r[d] = 0u; break;
    case Op::Mtsr: case Op::Mtsrin: case Op::Tlbie: case Op::Tlbsync: case Op::Sync: case Op::Isync: case Op::Eieio:
    case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Icbi:
        break;
    case Op::Sc: gcnr_syscall(mem, c); break;
    case Op::Tw: case Op::Twi:
    {
        const int to = d;
        const int32_t x = (int32_t)r[a], y = i.op == Op::Twi ? i.simm : (int32_t)r[b];
        const uint32_t ux = r[a], uy = i.op == Op::Twi ? (uint32_t)i.simm : r[b];
        const bool trap = to == 31 || ((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && x == y) ||
                          ((to & 2) && ux < uy) || ((to & 1) && ux > uy);
        if (trap) gcnr_trap(c, i.addr);
        break;
    }
    case Op::Eciwx: case Op::Ecowx: gcnr_unhandled(c, i.addr, "external control"); break;
    default:
        gcnr_unhandled(c, i.addr, "instruction gcnl_exec does not run (a branch?)");
        break;
    }
}
