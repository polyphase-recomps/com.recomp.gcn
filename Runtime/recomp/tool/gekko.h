/**
 * @file gekko.h
 * @brief Gekko (PowerPC 750CL) instruction decoder for the GameCube recompiler.
 *
 * decode() turns one big-endian instruction word into an Insn: the operation and its raw fields.
 * The generators interpret the fields per operation. disasm() prints it the way
 * `powerpc-eabi-objdump -M gekko,raw` does, so the decoder can be checked against binutils over a
 * whole DOL (tool/test/decode_test.cpp).
 */
#pragma once

#include <cstdint>
#include <string>

namespace gekko
{
// X(enum name, mnemonic)
#define GEKKO_OPS(X) \
    X(Invalid, ".long") \
    /* integer arithmetic */ \
    X(Add, "add") X(Addc, "addc") X(Adde, "adde") X(Addi, "addi") X(Addic, "addic") X(AddicRc, "addic.") \
    X(Addis, "addis") X(Addme, "addme") X(Addze, "addze") X(Divw, "divw") X(Divwu, "divwu") \
    X(Mulhw, "mulhw") X(Mulhwu, "mulhwu") X(Mulli, "mulli") X(Mullw, "mullw") X(Neg, "neg") \
    X(Subf, "subf") X(Subfc, "subfc") X(Subfe, "subfe") X(Subfic, "subfic") X(Subfme, "subfme") \
    X(Subfze, "subfze") \
    /* compare */ \
    X(Cmp, "cmp") X(Cmpi, "cmpi") X(Cmpl, "cmpl") X(Cmpli, "cmpli") \
    /* logical */ \
    X(And, "and") X(Andc, "andc") X(AndiRc, "andi.") X(AndisRc, "andis.") X(Cntlzw, "cntlzw") X(Eqv, "eqv") \
    X(Extsb, "extsb") X(Extsh, "extsh") X(Nand, "nand") X(Nor, "nor") X(Or, "or") X(Orc, "orc") X(Ori, "ori") \
    X(Oris, "oris") X(Xor, "xor") X(Xori, "xori") X(Xoris, "xoris") \
    /* rotate / shift */ \
    X(Rlwimi, "rlwimi") X(Rlwinm, "rlwinm") X(Rlwnm, "rlwnm") X(Slw, "slw") X(Sraw, "sraw") X(Srawi, "srawi") \
    X(Srw, "srw") \
    /* floating point */ \
    X(Fadd, "fadd") X(Fadds, "fadds") X(Fdiv, "fdiv") X(Fdivs, "fdivs") X(Fmul, "fmul") X(Fmuls, "fmuls") \
    X(Fres, "fres") X(Frsqrte, "frsqrte") X(Fsub, "fsub") X(Fsubs, "fsubs") X(Fsel, "fsel") \
    X(Fmadd, "fmadd") X(Fmadds, "fmadds") X(Fmsub, "fmsub") X(Fmsubs, "fmsubs") X(Fnmadd, "fnmadd") \
    X(Fnmadds, "fnmadds") X(Fnmsub, "fnmsub") X(Fnmsubs, "fnmsubs") X(Fctiw, "fctiw") X(Fctiwz, "fctiwz") \
    X(Frsp, "frsp") X(Fcmpo, "fcmpo") X(Fcmpu, "fcmpu") X(Mcrfs, "mcrfs") X(Mffs, "mffs") X(Mtfsb0, "mtfsb0") \
    X(Mtfsb1, "mtfsb1") X(Mtfsf, "mtfsf") X(Mtfsfi, "mtfsfi") X(Fabs, "fabs") X(Fmr, "fmr") X(Fnabs, "fnabs") \
    X(Fneg, "fneg") \
    /* loads / stores */ \
    X(Lbz, "lbz") X(Lbzu, "lbzu") X(Lbzux, "lbzux") X(Lbzx, "lbzx") X(Lha, "lha") X(Lhau, "lhau") \
    X(Lhaux, "lhaux") X(Lhax, "lhax") X(Lhz, "lhz") X(Lhzu, "lhzu") X(Lhzux, "lhzux") X(Lhzx, "lhzx") \
    X(Lwz, "lwz") X(Lwzu, "lwzu") X(Lwzux, "lwzux") X(Lwzx, "lwzx") X(Stb, "stb") X(Stbu, "stbu") \
    X(Stbux, "stbux") X(Stbx, "stbx") X(Sth, "sth") X(Sthu, "sthu") X(Sthux, "sthux") X(Sthx, "sthx") \
    X(Stw, "stw") X(Stwu, "stwu") X(Stwux, "stwux") X(Stwx, "stwx") X(Lhbrx, "lhbrx") X(Lwbrx, "lwbrx") \
    X(Sthbrx, "sthbrx") X(Stwbrx, "stwbrx") X(Lmw, "lmw") X(Stmw, "stmw") X(Lswi, "lswi") X(Lswx, "lswx") \
    X(Stswi, "stswi") X(Stswx, "stswx") X(Lwarx, "lwarx") X(StwcxRc, "stwcx.") \
    X(Lfd, "lfd") X(Lfdu, "lfdu") X(Lfdux, "lfdux") X(Lfdx, "lfdx") X(Lfs, "lfs") X(Lfsu, "lfsu") \
    X(Lfsux, "lfsux") X(Lfsx, "lfsx") X(Stfd, "stfd") X(Stfdu, "stfdu") X(Stfdux, "stfdux") X(Stfdx, "stfdx") \
    X(Stfiwx, "stfiwx") X(Stfs, "stfs") X(Stfsu, "stfsu") X(Stfsux, "stfsux") X(Stfsx, "stfsx") \
    /* paired singles */ \
    X(PsqL, "psq_l") X(PsqLu, "psq_lu") X(PsqLux, "psq_lux") X(PsqLx, "psq_lx") X(PsqSt, "psq_st") \
    X(PsqStu, "psq_stu") X(PsqStux, "psq_stux") X(PsqStx, "psq_stx") X(PsAbs, "ps_abs") X(PsAdd, "ps_add") \
    X(PsCmpo0, "ps_cmpo0") X(PsCmpo1, "ps_cmpo1") X(PsCmpu0, "ps_cmpu0") X(PsCmpu1, "ps_cmpu1") \
    X(PsDiv, "ps_div") X(PsMadd, "ps_madd") X(PsMadds0, "ps_madds0") X(PsMadds1, "ps_madds1") \
    X(PsMerge00, "ps_merge00") X(PsMerge01, "ps_merge01") X(PsMerge10, "ps_merge10") \
    X(PsMerge11, "ps_merge11") X(PsMr, "ps_mr") X(PsMsub, "ps_msub") X(PsMul, "ps_mul") \
    X(PsMuls0, "ps_muls0") X(PsMuls1, "ps_muls1") X(PsNabs, "ps_nabs") X(PsNeg, "ps_neg") \
    X(PsNmadd, "ps_nmadd") X(PsNmsub, "ps_nmsub") X(PsRes, "ps_res") X(PsRsqrte, "ps_rsqrte") \
    X(PsSel, "ps_sel") X(PsSub, "ps_sub") X(PsSum0, "ps_sum0") X(PsSum1, "ps_sum1") X(DcbzL, "dcbz_l") \
    /* branch / condition register */ \
    X(B, "b") X(Bc, "bc") X(Bcctr, "bcctr") X(Bclr, "bclr") X(Crand, "crand") X(Crandc, "crandc") \
    X(Creqv, "creqv") X(Crnand, "crnand") X(Crnor, "crnor") X(Cror, "cror") X(Crorc, "crorc") \
    X(Crxor, "crxor") X(Mcrf, "mcrf") X(Mcrxr, "mcrxr") X(Mfcr, "mfcr") X(Mtcrf, "mtcrf") \
    /* system */ \
    X(Mfmsr, "mfmsr") X(Mtmsr, "mtmsr") X(Mfspr, "mfspr") X(Mtspr, "mtspr") X(Mftb, "mftb") X(Mfsr, "mfsr") \
    X(Mfsrin, "mfsrin") X(Mtsr, "mtsr") X(Mtsrin, "mtsrin") X(Sc, "sc") X(Rfi, "rfi") X(Tw, "tw") X(Twi, "twi") \
    X(Sync, "sync") X(Isync, "isync") X(Eieio, "eieio") X(Dcbf, "dcbf") X(Dcbi, "dcbi") X(Dcbst, "dcbst") \
    X(Dcbt, "dcbt") X(Dcbtst, "dcbtst") X(Dcbz, "dcbz") X(Icbi, "icbi") X(Tlbie, "tlbie") X(Tlbsync, "tlbsync") \
    X(Eciwx, "eciwx") X(Ecowx, "ecowx")

enum class Op : uint16_t
{
#define GEKKO_ENUM(name, mnem) name,
    GEKKO_OPS(GEKKO_ENUM)
#undef GEKKO_ENUM
    Count
};

const char* mnemonic(Op op);

struct Insn
{
    uint32_t word = 0; // the instruction, as a big-endian word read from memory
    uint32_t addr = 0; // its address
    Op op = Op::Invalid;

    // raw fields (see the PowerPC architecture books); which ones matter depends on the op
    uint8_t d = 0;  // rD / rS / frD / frS / BO / TO / crbD / crfD<<2 (bits 6-10)
    uint8_t a = 0;  // rA / frA / BI / crbA (bits 11-15)
    uint8_t b = 0;  // rB / frB / SH / NB / crbB (bits 16-20)
    uint8_t c = 0;  // frC / MB (bits 21-25)
    uint8_t e = 0;  // ME (bits 26-30)
    bool rc = false; // record bit: updates CR0 (integer) / CR1 (floating point)
    bool oe = false; // overflow enable: updates XER[OV/SO]
    bool lk = false; // branch: link (sets LR)
    bool aa = false; // branch: absolute target
    int32_t simm = 0;    // sign-extended 16-bit immediate / displacement (psq: 12-bit)
    uint32_t uimm = 0;   // zero-extended 16-bit immediate
    uint32_t target = 0; // branch target (B, Bc)
    uint16_t spr = 0;    // SPR / TBR number (halves already swapped)
    uint8_t fxm = 0;     // mtcrf CRM / mtfsf FM
    uint8_t psw = 0;     // psq: W (1: load/store ps0 only)
    uint8_t psi = 0;     // psq: GQR index

    // CR field for compares and fp compares (bits 6-8)
    int crfd() const { return d >> 2; }
    // condition branch helpers
    int bo() const { return d; }
    int bi() const { return a; }
};

Insn decode(uint32_t word, uint32_t addr);

// The instruction as `objdump -M gekko,raw` prints it ("addi    r3,0,0").
std::string disasm(const Insn& insn);

// classification helpers for the analysis
bool is_branch(Op op);
bool is_load_store(Op op);
} // namespace gekko
