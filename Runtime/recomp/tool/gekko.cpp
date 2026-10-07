/**
 * @file gekko.cpp
 * @brief Gekko instruction decoder and objdump-style disassembler (see gekko.h).
 */
#include "gekko.h"

#include <cstdio>

namespace gekko
{
namespace
{
const char* const kMnemonics[] = {
#define GEKKO_NAME(name, mnem) mnem,
    GEKKO_OPS(GEKKO_NAME)
#undef GEKKO_NAME
};

int32_t sext(uint32_t v, int bits)
{
    const uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    return (int32_t)((v ^ m) - m);
}

// opcode 31, 10-bit extended opcode
Op op31(uint32_t xo10)
{
    switch (xo10)
    {
    case 0: return Op::Cmp;
    case 4: return Op::Tw;
    case 19: return Op::Mfcr;
    case 20: return Op::Lwarx;
    case 23: return Op::Lwzx;
    case 24: return Op::Slw;
    case 26: return Op::Cntlzw;
    case 28: return Op::And;
    case 32: return Op::Cmpl;
    case 54: return Op::Dcbst;
    case 55: return Op::Lwzux;
    case 60: return Op::Andc;
    case 83: return Op::Mfmsr;
    case 86: return Op::Dcbf;
    case 87: return Op::Lbzx;
    case 119: return Op::Lbzux;
    case 124: return Op::Nor;
    case 144: return Op::Mtcrf;
    case 146: return Op::Mtmsr;
    case 150: return Op::StwcxRc;
    case 151: return Op::Stwx;
    case 183: return Op::Stwux;
    case 210: return Op::Mtsr;
    case 215: return Op::Stbx;
    case 242: return Op::Mtsrin;
    case 246: return Op::Dcbtst;
    case 247: return Op::Stbux;
    case 278: return Op::Dcbt;
    case 279: return Op::Lhzx;
    case 284: return Op::Eqv;
    case 306: return Op::Tlbie;
    case 310: return Op::Eciwx;
    case 311: return Op::Lhzux;
    case 316: return Op::Xor;
    case 339: return Op::Mfspr;
    case 343: return Op::Lhax;
    case 371: return Op::Mftb;
    case 375: return Op::Lhaux;
    case 407: return Op::Sthx;
    case 412: return Op::Orc;
    case 438: return Op::Ecowx;
    case 439: return Op::Sthux;
    case 444: return Op::Or;
    case 467: return Op::Mtspr;
    case 470: return Op::Dcbi;
    case 476: return Op::Nand;
    case 512: return Op::Mcrxr;
    case 533: return Op::Lswx;
    case 534: return Op::Lwbrx;
    case 535: return Op::Lfsx;
    case 536: return Op::Srw;
    case 566: return Op::Tlbsync;
    case 567: return Op::Lfsux;
    case 595: return Op::Mfsr;
    case 597: return Op::Lswi;
    case 598: return Op::Sync;
    case 599: return Op::Lfdx;
    case 631: return Op::Lfdux;
    case 659: return Op::Mfsrin;
    case 661: return Op::Stswx;
    case 662: return Op::Stwbrx;
    case 663: return Op::Stfsx;
    case 695: return Op::Stfsux;
    case 725: return Op::Stswi;
    case 727: return Op::Stfdx;
    case 759: return Op::Stfdux;
    case 790: return Op::Lhbrx;
    case 792: return Op::Sraw;
    case 824: return Op::Srawi;
    case 854: return Op::Eieio;
    case 918: return Op::Sthbrx;
    case 922: return Op::Extsh;
    case 954: return Op::Extsb;
    case 982: return Op::Icbi;
    case 983: return Op::Stfiwx;
    case 1014: return Op::Dcbz;
    default: return Op::Invalid;
    }
}

// opcode 31, XO-form arithmetic (9-bit extended opcode; bit 21 is OE)
Op op31_xo(uint32_t xo9)
{
    switch (xo9)
    {
    case 8: return Op::Subfc;
    case 10: return Op::Addc;
    case 11: return Op::Mulhwu;
    case 40: return Op::Subf;
    case 75: return Op::Mulhw;
    case 104: return Op::Neg;
    case 136: return Op::Subfe;
    case 138: return Op::Adde;
    case 200: return Op::Subfze;
    case 202: return Op::Addze;
    case 232: return Op::Subfme;
    case 234: return Op::Addme;
    case 235: return Op::Mullw;
    case 266: return Op::Add;
    case 459: return Op::Divwu;
    case 491: return Op::Divw;
    default: return Op::Invalid;
    }
}

Op op19(uint32_t xo10)
{
    switch (xo10)
    {
    case 0: return Op::Mcrf;
    case 16: return Op::Bclr;
    case 33: return Op::Crnor;
    case 50: return Op::Rfi;
    case 129: return Op::Crandc;
    case 150: return Op::Isync;
    case 193: return Op::Crxor;
    case 225: return Op::Crnand;
    case 257: return Op::Crand;
    case 289: return Op::Creqv;
    case 417: return Op::Crorc;
    case 449: return Op::Cror;
    case 528: return Op::Bcctr;
    default: return Op::Invalid;
    }
}

Op op4(uint32_t w)
{
    switch ((w >> 1) & 0x3FF)
    {
    case 0: return Op::PsCmpu0;
    case 32: return Op::PsCmpo0;
    case 40: return Op::PsNeg;
    case 64: return Op::PsCmpu1;
    case 72: return Op::PsMr;
    case 96: return Op::PsCmpo1;
    case 136: return Op::PsNabs;
    case 264: return Op::PsAbs;
    case 528: return Op::PsMerge00;
    case 560: return Op::PsMerge01;
    case 592: return Op::PsMerge10;
    case 624: return Op::PsMerge11;
    case 1014: return Op::DcbzL;
    default: break;
    }
    switch ((w >> 1) & 0x3F)
    {
    case 6: return Op::PsqLx;
    case 7: return Op::PsqStx;
    case 38: return Op::PsqLux;
    case 39: return Op::PsqStux;
    default: break;
    }
    switch ((w >> 1) & 0x1F)
    {
    case 10: return Op::PsSum0;
    case 11: return Op::PsSum1;
    case 12: return Op::PsMuls0;
    case 13: return Op::PsMuls1;
    case 14: return Op::PsMadds0;
    case 15: return Op::PsMadds1;
    case 18: return Op::PsDiv;
    case 20: return Op::PsSub;
    case 21: return Op::PsAdd;
    case 23: return Op::PsSel;
    case 24: return Op::PsRes;
    case 25: return Op::PsMul;
    case 26: return Op::PsRsqrte;
    case 28: return Op::PsMsub;
    case 29: return Op::PsMadd;
    case 30: return Op::PsNmsub;
    case 31: return Op::PsNmadd;
    default: return Op::Invalid;
    }
}

Op op59(uint32_t w)
{
    switch ((w >> 1) & 0x1F)
    {
    case 18: return Op::Fdivs;
    case 20: return Op::Fsubs;
    case 21: return Op::Fadds;
    case 24: return Op::Fres;
    case 25: return Op::Fmuls;
    case 28: return Op::Fmsubs;
    case 29: return Op::Fmadds;
    case 30: return Op::Fnmsubs;
    case 31: return Op::Fnmadds;
    default: return Op::Invalid;
    }
}

Op op63(uint32_t w)
{
    switch ((w >> 1) & 0x3FF)
    {
    case 0: return Op::Fcmpu;
    case 12: return Op::Frsp;
    case 14: return Op::Fctiw;
    case 15: return Op::Fctiwz;
    case 32: return Op::Fcmpo;
    case 38: return Op::Mtfsb1;
    case 40: return Op::Fneg;
    case 64: return Op::Mcrfs;
    case 70: return Op::Mtfsb0;
    case 72: return Op::Fmr;
    case 134: return Op::Mtfsfi;
    case 136: return Op::Fnabs;
    case 264: return Op::Fabs;
    case 583: return Op::Mffs;
    case 711: return Op::Mtfsf;
    default: break;
    }
    switch ((w >> 1) & 0x1F)
    {
    case 18: return Op::Fdiv;
    case 20: return Op::Fsub;
    case 21: return Op::Fadd;
    case 23: return Op::Fsel;
    case 25: return Op::Fmul;
    case 26: return Op::Frsqrte;
    case 28: return Op::Fmsub;
    case 29: return Op::Fmadd;
    case 30: return Op::Fnmsub;
    case 31: return Op::Fnmadd;
    default: return Op::Invalid;
    }
}

Op primary(uint32_t w)
{
    switch (w >> 26)
    {
    case 3: return Op::Twi;
    case 4: return op4(w);
    case 7: return Op::Mulli;
    case 8: return Op::Subfic;
    case 10: return Op::Cmpli;
    case 11: return Op::Cmpi;
    case 12: return Op::Addic;
    case 13: return Op::AddicRc;
    case 14: return Op::Addi;
    case 15: return Op::Addis;
    case 16: return Op::Bc;
    case 17: return (w & 2) ? Op::Sc : Op::Invalid;
    case 18: return Op::B;
    case 19: return op19((w >> 1) & 0x3FF);
    case 20: return Op::Rlwimi;
    case 21: return Op::Rlwinm;
    case 23: return Op::Rlwnm;
    case 24: return Op::Ori;
    case 25: return Op::Oris;
    case 26: return Op::Xori;
    case 27: return Op::Xoris;
    case 28: return Op::AndiRc;
    case 29: return Op::AndisRc;
    case 31:
    {
        const Op op = op31((w >> 1) & 0x3FF);
        if (op != Op::Invalid)
        {
            return op;
        }
        const Op xo = op31_xo((w >> 1) & 0x1FF);
        if ((xo == Op::Mulhw || xo == Op::Mulhwu) && (w & 0x400))
        {
            return Op::Invalid; // no OE form
        }
        return xo;
    }
    case 32: return Op::Lwz;
    case 33: return Op::Lwzu;
    case 34: return Op::Lbz;
    case 35: return Op::Lbzu;
    case 36: return Op::Stw;
    case 37: return Op::Stwu;
    case 38: return Op::Stb;
    case 39: return Op::Stbu;
    case 40: return Op::Lhz;
    case 41: return Op::Lhzu;
    case 42: return Op::Lha;
    case 43: return Op::Lhau;
    case 44: return Op::Sth;
    case 45: return Op::Sthu;
    case 46: return Op::Lmw;
    case 47: return Op::Stmw;
    case 48: return Op::Lfs;
    case 49: return Op::Lfsu;
    case 50: return Op::Lfd;
    case 51: return Op::Lfdu;
    case 52: return Op::Stfs;
    case 53: return Op::Stfsu;
    case 54: return Op::Stfd;
    case 55: return Op::Stfdu;
    case 56: return Op::PsqL;
    case 57: return Op::PsqLu;
    case 59: return op59(w);
    case 60: return Op::PsqSt;
    case 61: return Op::PsqStu;
    case 63: return op63(w);
    default: return Op::Invalid;
    }
}

bool has_oe(Op op)
{
    switch (op)
    {
    case Op::Add: case Op::Addc: case Op::Adde: case Op::Addme: case Op::Addze: case Op::Divw:
    case Op::Divwu: case Op::Mullw: case Op::Neg: case Op::Subf: case Op::Subfc: case Op::Subfe:
    case Op::Subfme: case Op::Subfze:
        return true;
    default:
        return false;
    }
}

// ops whose bit 31 is the record bit
bool has_rc(Op op, uint32_t w)
{
    switch (w >> 26)
    {
    case 4: return op != Op::PsqLx && op != Op::PsqStx && op != Op::PsqLux && op != Op::PsqStux &&
                   op != Op::PsCmpu0 && op != Op::PsCmpu1 && op != Op::PsCmpo0 && op != Op::PsCmpo1 &&
                   op != Op::DcbzL;
    case 20: case 21: case 23: case 59: return true;
    case 63: return op != Op::Fcmpu && op != Op::Fcmpo && op != Op::Mcrfs;
    case 31:
        switch (op)
        {
        case Op::Add: case Op::Addc: case Op::Adde: case Op::Addme: case Op::Addze: case Op::Divw:
        case Op::Divwu: case Op::Mulhw: case Op::Mulhwu: case Op::Mullw: case Op::Neg: case Op::Subf:
        case Op::Subfc: case Op::Subfe: case Op::Subfme: case Op::Subfze: case Op::And: case Op::Andc:
        case Op::Cntlzw: case Op::Eqv: case Op::Extsb: case Op::Extsh: case Op::Nand: case Op::Nor:
        case Op::Or: case Op::Orc: case Op::Xor: case Op::Slw: case Op::Sraw: case Op::Srawi: case Op::Srw:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}
} // namespace

const char* mnemonic(Op op)
{
    return kMnemonics[(int)op];
}

Insn decode(uint32_t w, uint32_t addr)
{
    Insn i;
    i.word = w;
    i.addr = addr;
    i.op = primary(w);
    i.d = (w >> 21) & 31;
    i.a = (w >> 16) & 31;
    i.b = (w >> 11) & 31;
    i.c = (w >> 6) & 31;
    i.e = (w >> 1) & 31;
    i.simm = sext(w, 16);
    i.uimm = w & 0xFFFF;
    if (i.op == Op::Invalid)
    {
        return i;
    }
    i.rc = has_rc(i.op, w) && (w & 1);
    i.oe = has_oe(i.op) && ((w >> 10) & 1);
    switch (i.op)
    {
    case Op::B:
        i.aa = (w >> 1) & 1;
        i.lk = w & 1;
        i.target = (uint32_t)sext(w & 0x03FFFFFC, 26) + (i.aa ? 0 : addr);
        break;
    case Op::Bc:
        i.aa = (w >> 1) & 1;
        i.lk = w & 1;
        i.target = (uint32_t)sext(w & 0xFFFC, 16) + (i.aa ? 0 : addr);
        break;
    case Op::Bclr:
    case Op::Bcctr:
        i.lk = w & 1;
        break;
    case Op::Mfspr:
    case Op::Mtspr:
    case Op::Mftb:
        i.spr = (uint16_t)(((w >> 16) & 31) | (((w >> 11) & 31) << 5));
        break;
    case Op::Mtcrf:
        i.fxm = (w >> 12) & 0xFF;
        break;
    case Op::Mtfsf:
        i.fxm = (w >> 17) & 0xFF;
        break;
    case Op::PsqL:
    case Op::PsqLu:
    case Op::PsqSt:
    case Op::PsqStu:
        i.psw = (w >> 15) & 1;
        i.psi = (w >> 12) & 7;
        i.simm = sext(w, 12);
        break;
    case Op::PsqLx:
    case Op::PsqLux:
    case Op::PsqStx:
    case Op::PsqStux:
        i.psw = (w >> 10) & 1;
        i.psi = (w >> 7) & 7;
        break;
    default:
        break;
    }
    return i;
}

// ---- disassembly, in objdump's raw syntax ------------------------------------------------------
namespace
{
std::string r(int n) { return "r" + std::to_string(n); }
std::string f(int n) { return "f" + std::to_string(n); }
std::string r0(int n) { return n == 0 ? std::string("0") : r(n); } // "(RA|0)" operands
std::string cr(int n) { return "cr" + std::to_string(n); }
std::string num(int64_t v) { return std::to_string(v); }

std::string hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

// a condition register bit: "lt" .. "so" in cr0, else "4*crN+eq"
std::string crbit(int bit)
{
    static const char* const kNames[] = {"lt", "gt", "eq", "so"};
    if (bit < 4)
    {
        return kNames[bit];
    }
    return "4*" + cr(bit >> 2) + "+" + kNames[bit & 3];
}

std::string mem(int32_t d, int ra, bool zeroIsZero)
{
    return num(d) + "(" + (zeroIsZero ? r0(ra) : r(ra)) + ")";
}
} // namespace

std::string disasm(const Insn& i)
{
    if (i.op == Op::Invalid)
    {
        return ".long " + hex(i.word);
    }
    std::string m = mnemonic(i.op);
    if (i.op == Op::B || i.op == Op::Bc || i.op == Op::Bclr || i.op == Op::Bcctr)
    {
        if (i.lk) m += "l";
        if (i.aa) m += "a";
    }
    if (i.oe) m += "o";
    if (i.rc) m += ".";

    std::string ops;
    const uint32_t w = i.word;
    switch (i.op)
    {
    case Op::Add: case Op::Addc: case Op::Adde: case Op::Divw: case Op::Divwu: case Op::Mulhw: case Op::Mulhwu:
    case Op::Mullw: case Op::Subf: case Op::Subfc: case Op::Subfe:
        ops = r(i.d) + "," + r(i.a) + "," + r(i.b);
        break;
    case Op::Addme: case Op::Addze: case Op::Neg: case Op::Subfme: case Op::Subfze:
        ops = r(i.d) + "," + r(i.a);
        break;
    case Op::Addi: case Op::Addis:
        ops = r(i.d) + "," + r0(i.a) + "," + num(i.simm);
        break;
    case Op::Addic: case Op::AddicRc: case Op::Subfic: case Op::Mulli:
        ops = r(i.d) + "," + r(i.a) + "," + num(i.simm);
        break;
    case Op::Cmp: case Op::Cmpl:
        ops = cr(i.crfd()) + "," + num(i.d & 1) + "," + r(i.a) + "," + r(i.b);
        break;
    case Op::Cmpi:
        ops = cr(i.crfd()) + "," + num(i.d & 1) + "," + r(i.a) + "," + num(i.simm);
        break;
    case Op::Cmpli:
        ops = cr(i.crfd()) + "," + num(i.d & 1) + "," + r(i.a) + "," + num(i.uimm);
        break;
    case Op::And: case Op::Andc: case Op::Eqv: case Op::Nand: case Op::Nor: case Op::Or: case Op::Orc:
    case Op::Xor: case Op::Slw: case Op::Sraw: case Op::Srw:
        ops = r(i.a) + "," + r(i.d) + "," + r(i.b);
        break;
    case Op::AndiRc: case Op::AndisRc: case Op::Ori: case Op::Oris: case Op::Xori: case Op::Xoris:
        ops = r(i.a) + "," + r(i.d) + "," + num(i.uimm);
        break;
    case Op::Cntlzw: case Op::Extsb: case Op::Extsh:
        ops = r(i.a) + "," + r(i.d);
        break;
    case Op::Rlwimi: case Op::Rlwinm:
        ops = r(i.a) + "," + r(i.d) + "," + num(i.b) + "," + num(i.c) + "," + num(i.e);
        break;
    case Op::Rlwnm:
        ops = r(i.a) + "," + r(i.d) + "," + r(i.b) + "," + num(i.c) + "," + num(i.e);
        break;
    case Op::Srawi:
        ops = r(i.a) + "," + r(i.d) + "," + num(i.b);
        break;

    // floating point
    case Op::Fadd: case Op::Fadds: case Op::Fdiv: case Op::Fdivs: case Op::Fsub: case Op::Fsubs:
    case Op::PsAdd: case Op::PsSub: case Op::PsDiv: case Op::PsMerge00: case Op::PsMerge01:
    case Op::PsMerge10: case Op::PsMerge11:
        ops = f(i.d) + "," + f(i.a) + "," + f(i.b);
        break;
    case Op::Fmul: case Op::Fmuls: case Op::PsMul: case Op::PsMuls0: case Op::PsMuls1:
        ops = f(i.d) + "," + f(i.a) + "," + f(i.c);
        break;
    case Op::Fmadd: case Op::Fmadds: case Op::Fmsub: case Op::Fmsubs: case Op::Fnmadd: case Op::Fnmadds:
    case Op::Fnmsub: case Op::Fnmsubs: case Op::Fsel: case Op::PsMadd: case Op::PsMsub: case Op::PsNmadd:
    case Op::PsNmsub: case Op::PsSel: case Op::PsSum0: case Op::PsSum1: case Op::PsMadds0: case Op::PsMadds1:
        ops = f(i.d) + "," + f(i.a) + "," + f(i.c) + "," + f(i.b);
        break;
    case Op::Fres: case Op::Frsqrte:
        ops = f(i.d) + "," + f(i.b) + "," + num(i.a ? 1 : 0);
        break;
    case Op::Fabs: case Op::Fmr: case Op::Fnabs: case Op::Fneg: case Op::Frsp: case Op::Fctiw: case Op::Fctiwz:
    case Op::PsMr: case Op::PsNeg: case Op::PsAbs: case Op::PsNabs: case Op::PsRes: case Op::PsRsqrte:
        ops = f(i.d) + "," + f(i.b);
        break;
    case Op::Fcmpu: case Op::Fcmpo: case Op::PsCmpu0: case Op::PsCmpu1: case Op::PsCmpo0: case Op::PsCmpo1:
        ops = cr(i.crfd()) + "," + f(i.a) + "," + f(i.b);
        break;
    case Op::Mffs:
        ops = f(i.d);
        break;
    case Op::Mtfsb0: case Op::Mtfsb1:
        ops = num(i.d);
        break;
    case Op::Mtfsf:
        ops = num(i.fxm) + "," + f(i.b);
        break;
    case Op::Mtfsfi:
        ops = cr(i.crfd()) + "," + num((w >> 12) & 15);
        break;
    case Op::Mcrfs:
        ops = cr(i.crfd()) + "," + cr(i.a >> 2);
        break;

    // loads / stores
    case Op::Lbz: case Op::Lha: case Op::Lhz: case Op::Lwz: case Op::Stb: case Op::Sth: case Op::Stw:
    case Op::Lmw: case Op::Stmw:
        ops = r(i.d) + "," + mem(i.simm, i.a, true);
        break;
    case Op::Lbzu: case Op::Lhau: case Op::Lhzu: case Op::Lwzu: case Op::Stbu: case Op::Sthu: case Op::Stwu:
        ops = r(i.d) + "," + mem(i.simm, i.a, false);
        break;
    case Op::Lfs: case Op::Lfd: case Op::Stfs: case Op::Stfd:
        ops = f(i.d) + "," + mem(i.simm, i.a, true);
        break;
    case Op::Lfsu: case Op::Lfdu: case Op::Stfsu: case Op::Stfdu:
        ops = f(i.d) + "," + mem(i.simm, i.a, false);
        break;
    case Op::Lbzx: case Op::Lhax: case Op::Lhzx: case Op::Lwzx: case Op::Stbx: case Op::Sthx: case Op::Stwx:
    case Op::Lhbrx: case Op::Lwbrx: case Op::Sthbrx: case Op::Stwbrx: case Op::Lswx: case Op::Stswx:
    case Op::Lwarx: case Op::StwcxRc: case Op::Eciwx: case Op::Ecowx:
        ops = r(i.d) + "," + r0(i.a) + "," + r(i.b);
        break;
    case Op::Lbzux: case Op::Lhaux: case Op::Lhzux: case Op::Lwzux: case Op::Stbux: case Op::Sthux:
    case Op::Stwux:
        ops = r(i.d) + "," + r(i.a) + "," + r(i.b);
        break;
    case Op::Lfsx: case Op::Lfdx: case Op::Stfsx: case Op::Stfdx: case Op::Stfiwx:
        ops = f(i.d) + "," + r0(i.a) + "," + r(i.b);
        break;
    case Op::Lfsux: case Op::Lfdux: case Op::Stfsux: case Op::Stfdux:
        ops = f(i.d) + "," + r(i.a) + "," + r(i.b);
        break;
    case Op::Lswi: case Op::Stswi:
        ops = r(i.d) + "," + r0(i.a) + "," + num(i.b);
        break;
    case Op::PsqL: case Op::PsqSt:
        ops = f(i.d) + "," + mem(i.simm, i.a, true) + "," + num(i.psw) + "," + num(i.psi);
        break;
    case Op::PsqLu: case Op::PsqStu:
        ops = f(i.d) + "," + mem(i.simm, i.a, false) + "," + num(i.psw) + "," + num(i.psi);
        break;
    case Op::PsqLx: case Op::PsqStx:
        ops = f(i.d) + "," + r0(i.a) + "," + r(i.b) + "," + num(i.psw) + "," + num(i.psi);
        break;
    case Op::PsqLux: case Op::PsqStux:
        ops = f(i.d) + "," + r(i.a) + "," + r(i.b) + "," + num(i.psw) + "," + num(i.psi);
        break;
    case Op::DcbzL:
        ops = r(i.a) + "," + r(i.b);
        break;

    // branches / CR
    case Op::B:
        ops = hex(i.target);
        break;
    case Op::Bc:
        ops = num(i.bo()) + "," + crbit(i.bi()) + "," + hex(i.target);
        break;
    case Op::Bclr: case Op::Bcctr:
        ops = num(i.bo()) + "," + crbit(i.bi()) + "," + num((w >> 11) & 3);
        break;
    case Op::Crand: case Op::Crandc: case Op::Creqv: case Op::Crnand: case Op::Crnor: case Op::Cror:
    case Op::Crorc: case Op::Crxor:
        ops = crbit(i.d) + "," + crbit(i.a) + "," + crbit(i.b);
        break;
    case Op::Mcrf:
        ops = cr(i.crfd()) + "," + cr(i.a >> 2);
        break;
    case Op::Mcrxr:
        ops = cr(i.crfd());
        break;
    case Op::Mfcr:
        ops = r(i.d) + ",-1"; // objdump's raw syntax shows the (absent) field mask as -1
        break;
    case Op::Mtcrf:
        ops = num(i.fxm) + "," + r(i.d);
        break;

    // system
    case Op::Mfmsr:
        ops = r(i.d);
        break;
    case Op::Mtmsr:
        ops = r(i.d) + "," + num((w >> 16) & 1);
        break;
    case Op::Mfspr: case Op::Mftb:
        ops = r(i.d) + "," + num(i.spr);
        break;
    case Op::Mtspr:
        ops = num(i.spr) + "," + r(i.d);
        break;
    case Op::Mfsr:
        ops = r(i.d) + "," + num(i.a & 15);
        break;
    case Op::Mtsr:
        ops = num(i.a & 15) + "," + r(i.d);
        break;
    case Op::Mfsrin:
        ops = r(i.d) + "," + r(i.b);
        break;
    case Op::Mtsrin:
        ops = r(i.d) + "," + r(i.b);
        break;
    case Op::Sc:
        ops = num((w >> 5) & 0x7F);
        break;
    case Op::Sync:
        ops = num((w >> 21) & 3);
        break;
    case Op::Tw:
        ops = num(i.d) + "," + r(i.a) + "," + r(i.b);
        break;
    case Op::Twi:
        ops = num(i.d) + "," + r(i.a) + "," + num(i.simm);
        break;
    case Op::Dcbf:
        ops = r0(i.a) + "," + r(i.b) + "," + num((w >> 21) & 3);
        break;
    case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Dcbz: case Op::Icbi:
        ops = r0(i.a) + "," + r(i.b);
        break;
    case Op::Tlbie:
        ops = r(i.b);
        break;
    default:
        break; // rfi, isync, eieio, tlbsync
    }
    if (ops.empty())
    {
        return m;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%-7s %s", m.c_str(), ops.c_str());
    return buf;
}

bool is_branch(Op op)
{
    return op == Op::B || op == Op::Bc || op == Op::Bclr || op == Op::Bcctr;
}

bool is_load_store(Op op)
{
    return (op >= Op::Lbz && op <= Op::Stfsx) || (op >= Op::PsqL && op <= Op::PsqStx) || op == Op::DcbzL;
}
} // namespace gekko
