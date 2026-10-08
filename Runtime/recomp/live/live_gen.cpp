/**
 * @file live_gen.cpp
 * @brief The Live generator: guest functions -> native code with sljit (see live.h).
 *
 * Mirrors cgen.cpp: the same functions (normal, LR-mode bodies with their entry switch, stubs
 * for entry points into LR-mode bodies), the same control flow for every branch form, the same
 * calls (direct to recompiled functions, hle_<name> wrappers, the lookup for anything else) and
 * the same loop checks. Common instructions are emitted inline (integer, compares, loads and
 * stores, floating point and paired-single arithmetic); the rest calls gcnl_exec, which has
 * cgen.cpp's semantics one instruction at a time. GCNL_NO_INLINE=1 in the environment sends
 * every non-branch instruction there (to tell an inline bug from a recompiler bug).
 *
 * Register use: S0 = guest memory, S1 = the gcnr_ctx, S2 = the entry address (LR-mode bodies).
 * Guest registers stay in the gcnr_ctx; R0-R3 / FR0-FR2 hold values within one instruction.
 */
#include "live.h"

#include "../tool/analysis.h"

extern "C" {
#include "sljitLir.h"
}

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>

using gekko::Insn;
using gekko::Op;

extern "C" {
// GCNR_CALL_INDIRECT, and the out-of-line memory helpers of the inline slow paths (live_exec.cpp)
void gcnl_call(uint8_t* mem, gcnr_ctx* c, uint32_t target);
uint32_t gcnl_lw(const uint8_t* mem, uint32_t a);
uint32_t gcnl_lhz(const uint8_t* mem, uint32_t a);
uint32_t gcnl_lha(const uint8_t* mem, uint32_t a);
uint32_t gcnl_lbz(const uint8_t* mem, uint32_t a);
uint64_t gcnl_ld(const uint8_t* mem, uint32_t a);
void gcnl_sw(uint8_t* mem, uint32_t a, uint32_t v);
void gcnl_sh(uint8_t* mem, uint32_t a, uint32_t v);
void gcnl_sb(uint8_t* mem, uint32_t a, uint32_t v);
void gcnl_sd(uint8_t* mem, uint32_t a, uint64_t v);
}

namespace gcnr
{
namespace
{
const sljit_s32 MEM = SLJIT_S0, CTX = SLJIT_S1, ENTRY = SLJIT_S2;
const sljit_s32 R0 = SLJIT_R0, R1 = SLJIT_R1, R2 = SLJIT_R2, R3 = SLJIT_R3;
const sljit_s32 FR0 = SLJIT_FR0, FR1 = SLJIT_FR1, FR2 = SLJIT_FR2;
const sljit_s32 CM = SLJIT_MEM1(SLJIT_S1);    // [ctx + offset]
const sljit_s32 LOCAL = SLJIT_MEM1(SLJIT_SP); // the frame's scratch words
const sljit_s32 IMM = SLJIT_IMM;

sljit_sw OR_(int n) { return (sljit_sw)(offsetof(gcnr_ctx, r) + 4 * (size_t)n); }
sljit_sw OF0(int n) { return (sljit_sw)(offsetof(gcnr_ctx, f) + sizeof(gcnr_fpr) * (size_t)n); }
sljit_sw OF1(int n) { return OF0(n) + 8; }
#define OFF(field) ((sljit_sw)offsetof(gcnr_ctx, field))
sljit_sw I32(uint32_t v) { return (sljit_sw)(int32_t)v; }

std::string hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

uint32_t ppc_mask(int mb, int me)
{
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = (me >= 31) ? 0u : (0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & ~b) : (a | ~b);
}

bool uncond_branch(const Insn& i)
{
    return ((i.op == Op::B) && !i.lk) ||
           ((i.op == Op::Bc || i.op == Op::Bclr || i.op == Op::Bcctr) && !i.lk && (i.bo() & 0x14) == 0x14);
}

class LiveGen
{
public:
    LiveGen(Program& p, const LiveInputs& in, LiveResult& out) : P(p), mIn(in), mOut(out)
    {
        for (const gcnr_named_func* h = in.hle; h && h->name; h++)
        {
            mHleByName[h->name] = h->fn;
        }
        const char* env = std::getenv("GCNL_NO_INLINE");
        mInline = !(env && env[0] == '1');
        // a store watch (GCNR_WATCH=<hex> in a GCNR_WATCH build): stores through gcnr_sw & co
        mInlineStores = std::getenv("GCNR_WATCH") == nullptr;
        // GCNL_TRACE=<hex>,<hex>...: those functions log their registers on entry (GcnRecomp --trace)
        if (const char* t = std::getenv("GCNL_TRACE"))
        {
            for (const char* p = t; *p;)
            {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p) break;
                mTraced.insert((uint32_t)v);
                p = *end ? end + 1 : end;
            }
        }
    }

    bool run();

private:
    struct Pending
    {
        sljit_jump* jump;
        uint32_t target;
    };

    void function(const Function& f);
    void lr_stub(const Function& f, const Function& parent);
    void insn(const Analysis& an, const Insn& i);
    bool inline_op(const Insn& i, uint32_t hw);
    void helper(const Insn& i, uint32_t hw);

    // control flow
    void call_target(uint32_t target);
    void call_lookup_r2(); // GCNR_CALL_INDIRECT(mem, c, R2)
    void jump_local(uint32_t target, sljit_jump* jump = nullptr);
    void cond_false_jumps(const Insn& i, std::vector<sljit_jump*>& out);
    void land(std::vector<sljit_jump*>& jumps);
    void loop_check(bool spin);
    void loop_checks(const Analysis& an, uint32_t at);
    void unhandled(uint32_t addr, const char* what);
    void ret() { sljit_emit_return_void(C); }
    void args_mem_ctx()
    {
        sljit_emit_op1(C, SLJIT_MOV_P, R0, 0, MEM, 0);
        sljit_emit_op1(C, SLJIT_MOV_P, R1, 0, CTX, 0);
    }

    // inline pieces
    void set_crf(int field, bool sig, sljit_s32 b, sljit_sw bw); // CR[field] = compare(R0, b) | SO
    void cr0(int reg);
    void ea_d(const Insn& i);   // R0 = (rA|0) + d
    void ea_x(const Insn& i);   // R0 = (rA|0) + rB
    void load_int(const Insn& i, bool x, bool u, int bytes, bool sign);
    void store_int(const Insn& i, bool x, bool u, int bytes);
    void mem_load(int bytes, bool sign); // R2 = value at R0 (ea), via the fast path or a helper
    void mem_store(int bytes);           // [R0] = R2
    void round_single(sljit_s32 fr);
    void fp_madd(sljit_s32 dst, int a, int c, int b, sljit_sw ah, sljit_sw ch, sljit_sw bh, bool sub, bool neg);

    sljit_compiler* C = nullptr;
    Program& P;
    LiveInputs mIn;
    LiveResult& mOut;
    bool mInline = true;
    bool mInlineStores = true;
    std::set<uint32_t> mTraced;
    std::map<std::string, gcnr_func> mHleByName;
    std::map<uint32_t, sljit_label*> mEntry; // f_ADDR
    std::map<uint32_t, sljit_label*> mBody;  // <f>_body of LR-mode functions
    std::vector<Pending> mCalls, mBodyCalls;
    // per function
    const Analysis* mAn = nullptr;
    std::map<uint32_t, sljit_label*> mLabels;
    std::vector<Pending> mLocal;
    std::deque<gcnl_insn> mInsns; // gcnl_exec's arguments (addresses stay put)
};

// ---- control flow ----------------------------------------------------------------------------
void LiveGen::call_target(uint32_t target)
{
    const Function* f = P.function_at(target);
    gcnr_func ext = nullptr;
    if (f == nullptr && mIn.external)
    {
        ext = mIn.external(target);
    }
    args_mem_ctx();
    if (f != nullptr && f->hle)
    {
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(P, P), IMM, SLJIT_FUNC_ADDR(mHleByName[f->name]));
    }
    else if (f != nullptr && !mIn.hooked.count(target))
    {
        mCalls.push_back({sljit_emit_call(C, SLJIT_CALL, SLJIT_ARGS2V(P, P)), target});
    }
    else if (ext != nullptr)
    {
        // compiled before (the DOL, a module linked earlier; a hooked function's wrapper)
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(P, P), IMM, SLJIT_FUNC_ADDR(ext));
    }
    else
    {
        sljit_emit_op1(C, SLJIT_MOV32, R2, 0, IMM, I32(target));
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32), IMM, SLJIT_FUNC_ADDR(gcnl_call));
    }
}

void LiveGen::call_lookup_r2()
{
    args_mem_ctx();
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32), IMM, SLJIT_FUNC_ADDR(gcnl_call));
}

void LiveGen::jump_local(uint32_t target, sljit_jump* jump)
{
    mLocal.push_back({jump ? jump : sljit_emit_jump(C, SLJIT_JUMP), target});
}

// jumps taken when a bc-type instruction's condition is false (CTR decremented first)
void LiveGen::cond_false_jumps(const Insn& i, std::vector<sljit_jump*>& out)
{
    const int bo = i.bo();
    if (!(bo & 4))
    {
        sljit_emit_op2(C, SLJIT_SUB32 | SLJIT_SET_Z, R0, 0, CM, OFF(ctr), IMM, 1);
        sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(ctr), R0, 0); // (moves keep the flags)
        out.push_back(sljit_emit_jump(C, (bo & 2) ? SLJIT_NOT_ZERO : SLJIT_ZERO));
    }
    if (!(bo & 16))
    {
        sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, CM, OFF(cr), IMM, I32(0x80000000u >> i.bi()));
        out.push_back(sljit_emit_jump(C, (bo & 8) ? SLJIT_ZERO : SLJIT_NOT_ZERO));
    }
}

void LiveGen::land(std::vector<sljit_jump*>& jumps)
{
    if (jumps.empty())
    {
        return;
    }
    sljit_label* l = sljit_emit_label(C);
    for (sljit_jump* j : jumps)
    {
        sljit_set_label(j, l);
    }
    jumps.clear();
}

// GCNR_LOOP (busy-wait loops: wait for events) / GCNR_LOOP_ANY (any other loop: pending
// interrupts now and then), as cgen.cpp's loop_check
void LiveGen::loop_check(bool spin)
{
    sljit_emit_op2(C, SLJIT_ADD32, R0, 0, CM, OFF(loop), IMM, 1);
    sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(loop), R0, 0);
    sljit_jump* low = sljit_emit_cmp(C, SLJIT_LESS | SLJIT_32, R0, 0, IMM, (sljit_sw)GCNR_LOOP_LIMIT);
    args_mem_ctx();
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(P, P), IMM,
                     spin ? SLJIT_FUNC_ADDR(gcnr_loop_poll) : SLJIT_FUNC_ADDR(gcnr_loop_any));
    sljit_set_label(low, sljit_emit_label(C));
}

void LiveGen::loop_checks(const Analysis& an, uint32_t at)
{
    if (an.spinLoops.count(at)) loop_check(true);
    else if (an.backBranches.count(at)) loop_check(false);
}

void LiveGen::unhandled(uint32_t addr, const char* what)
{
    sljit_emit_op1(C, SLJIT_MOV_P, R0, 0, CTX, 0);
    sljit_emit_op1(C, SLJIT_MOV32, R1, 0, IMM, I32(addr));
    sljit_emit_op1(C, SLJIT_MOV_P, R2, 0, IMM, (sljit_sw)what);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, 32, P), IMM, SLJIT_FUNC_ADDR(gcnr_unhandled));
}

void LiveGen::helper(const Insn& i, uint32_t hw)
{
    mInsns.push_back(gcnl_insn{i, hw});
    args_mem_ctx();
    sljit_emit_op1(C, SLJIT_MOV_P, R2, 0, IMM, (sljit_sw)&mInsns.back());
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), IMM, SLJIT_FUNC_ADDR(gcnl_exec));
    mOut.helpers++;
}

// ---- inline pieces -----------------------------------------------------------------------------
// CR[field] = (R0 < b ? 8 : R0 > b ? 4 : 2) | SO, signed or unsigned (gcnr_cmp / gcnr_cmpl)
void LiveGen::set_crf(int field, bool sig, sljit_s32 b, sljit_sw bw)
{
    const int sh = 28 - 4 * field;
    sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z | (sig ? SLJIT_SET_SIG_LESS : SLJIT_SET_LESS), R0, 0, b, bw);
    sljit_emit_op_flags(C, SLJIT_MOV32, R1, 0, sig ? SLJIT_SIG_LESS : SLJIT_LESS);
    sljit_emit_op_flags(C, SLJIT_MOV32, R2, 0, SLJIT_EQUAL);
    // lt ? 8 : eq ? 2 : 4  ==  4 + 4*lt - 2*eq
    sljit_emit_op2(C, SLJIT_SHL32, R1, 0, R1, 0, IMM, 2);
    sljit_emit_op2(C, SLJIT_SHL32, R2, 0, R2, 0, IMM, 1);
    sljit_emit_op2(C, SLJIT_SUB32, R1, 0, R1, 0, R2, 0);
    sljit_emit_op2(C, SLJIT_ADD32, R1, 0, R1, 0, IMM, 4);
    sljit_emit_op2(C, SLJIT_OR32, R1, 0, R1, 0, CM, OFF(xer_so));
    if (sh)
    {
        sljit_emit_op2(C, SLJIT_SHL32, R1, 0, R1, 0, IMM, sh);
    }
    sljit_emit_op2(C, SLJIT_AND32, R0, 0, CM, OFF(cr), IMM, I32(~(0xFu << sh)));
    sljit_emit_op2(C, SLJIT_OR32, CM, OFF(cr), R0, 0, R1, 0);
}

void LiveGen::cr0(int reg)
{
    sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(reg));
    set_crf(0, true, IMM, 0);
}

void LiveGen::ea_d(const Insn& i)
{
    if (i.a == 0)
    {
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, IMM, I32((uint32_t)i.simm));
    }
    else if (i.simm == 0)
    {
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(i.a));
    }
    else
    {
        sljit_emit_op2(C, SLJIT_ADD32, R0, 0, CM, OR_(i.a), IMM, I32((uint32_t)i.simm));
    }
}

void LiveGen::ea_x(const Insn& i)
{
    if (i.a == 0)
    {
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(i.b));
    }
    else
    {
        sljit_emit_op2(C, SLJIT_ADD32, R0, 0, CM, OR_(i.a), CM, OR_(i.b));
    }
}

// R2 = the value at guest address R0. Below 0xC0000000 straight from the buffer (GCNR_OFFSET's
// RAM mirror fold); the uncached mirror, locked cache and hardware registers through the helpers.
void LiveGen::mem_load(int bytes, bool sign)
{
    sljit_jump* slow = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL | SLJIT_32, R0, 0, IMM, I32(0xC0000000u));
    sljit_emit_op1(C, SLJIT_MOV_U32, R1, 0, R0, 0);
    sljit_emit_op2(C, SLJIT_AND, R1, 0, R1, 0, IMM, (sljit_sw)GCNR_RAM_MASK);
    switch (bytes)
    {
    case 1:
        sljit_emit_op1(C, SLJIT_MOV_U8, R2, 0, SLJIT_MEM2(MEM, R1), 0);
        break;
    case 2:
        sljit_emit_op1(C, SLJIT_MOV_U16, R2, 0, SLJIT_MEM2(MEM, R1), 0);
        sljit_emit_op1(C, sign ? SLJIT_REV_S16 : SLJIT_REV_U16, R2, 0, R2, 0);
        break;
    case 4:
        sljit_emit_op1(C, SLJIT_MOV_U32, R2, 0, SLJIT_MEM2(MEM, R1), 0);
        sljit_emit_op1(C, SLJIT_REV_U32, R2, 0, R2, 0);
        break;
    default: // 8
        sljit_emit_op1(C, SLJIT_MOV, R2, 0, SLJIT_MEM2(MEM, R1), 0);
        sljit_emit_op1(C, SLJIT_REV, R2, 0, R2, 0);
        break;
    }
    sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);
    sljit_set_label(slow, sljit_emit_label(C));
    sljit_emit_op1(C, SLJIT_MOV32, R1, 0, R0, 0);
    sljit_emit_op1(C, SLJIT_MOV_P, R0, 0, MEM, 0);
    const sljit_sw fn = bytes == 1 ? SLJIT_FUNC_ADDR(gcnl_lbz)
                      : bytes == 2 ? (sign ? SLJIT_FUNC_ADDR(gcnl_lha) : SLJIT_FUNC_ADDR(gcnl_lhz))
                      : bytes == 4 ? SLJIT_FUNC_ADDR(gcnl_lw) : SLJIT_FUNC_ADDR(gcnl_ld);
    if (bytes == 8)
    {
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2(W, P, 32), IMM, fn);
        sljit_emit_op1(C, SLJIT_MOV, R2, 0, SLJIT_RETURN_REG, 0);
    }
    else
    {
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2(32, P, 32), IMM, fn);
        sljit_emit_op1(C, SLJIT_MOV32, R2, 0, SLJIT_RETURN_REG, 0);
    }
    sljit_set_label(done, sljit_emit_label(C));
}

// [guest address R0] = R2 (its low `bytes` bytes)
void LiveGen::mem_store(int bytes)
{
    sljit_jump* slow = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL | SLJIT_32, R0, 0, IMM, I32(0xC0000000u));
    sljit_emit_op1(C, SLJIT_MOV_U32, R1, 0, R0, 0);
    sljit_emit_op2(C, SLJIT_AND, R1, 0, R1, 0, IMM, (sljit_sw)GCNR_RAM_MASK);
    switch (bytes)
    {
    case 1:
        sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM2(MEM, R1), 0, R2, 0);
        break;
    case 2:
        sljit_emit_op1(C, SLJIT_REV_U16, R3, 0, R2, 0);
        sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_MEM2(MEM, R1), 0, R3, 0);
        break;
    case 4:
        sljit_emit_op1(C, SLJIT_REV_U32, R3, 0, R2, 0);
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM2(MEM, R1), 0, R3, 0);
        break;
    default:
        sljit_emit_op1(C, SLJIT_REV, R3, 0, R2, 0);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM2(MEM, R1), 0, R3, 0);
        break;
    }
    sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);
    sljit_set_label(slow, sljit_emit_label(C));
    sljit_emit_op1(C, SLJIT_MOV32, R1, 0, R0, 0);
    sljit_emit_op1(C, SLJIT_MOV_P, R0, 0, MEM, 0);
    const sljit_sw fn = bytes == 1 ? SLJIT_FUNC_ADDR(gcnl_sb) : bytes == 2 ? SLJIT_FUNC_ADDR(gcnl_sh)
                      : bytes == 4 ? SLJIT_FUNC_ADDR(gcnl_sw) : SLJIT_FUNC_ADDR(gcnl_sd);
    sljit_emit_icall(C, SLJIT_CALL, bytes == 8 ? SLJIT_ARGS3V(P, 32, W) : SLJIT_ARGS3V(P, 32, 32), IMM, fn);
    sljit_set_label(done, sljit_emit_label(C));
}

void LiveGen::load_int(const Insn& i, bool x, bool u, int bytes, bool sign)
{
    x ? ea_x(i) : ea_d(i);
    if (u)
    {
        sljit_emit_op1(C, SLJIT_MOV32, LOCAL, 0, R0, 0);
    }
    mem_load(bytes, sign);
    sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(i.d), R2, 0);
    if (u)
    {
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(i.a), LOCAL, 0);
    }
}

void LiveGen::store_int(const Insn& i, bool x, bool u, int bytes)
{
    sljit_emit_op1(C, SLJIT_MOV32, R2, 0, CM, OR_(i.d));
    x ? ea_x(i) : ea_d(i);
    if (u)
    {
        sljit_emit_op1(C, SLJIT_MOV32, LOCAL, 0, R0, 0);
    }
    mem_store(bytes);
    if (u)
    {
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(i.a), LOCAL, 0);
    }
}

void LiveGen::round_single(sljit_s32 fr)
{
    sljit_emit_fop1(C, SLJIT_CONV_F32_FROM_F64, fr, 0, fr, 0);
    sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_F32, fr, 0, fr, 0);
}

// dst = (+/-)(a * c (+/-) b), not fused (GCNR_FMA=0); ah/ch/bh: the halves' offsets
void LiveGen::fp_madd(sljit_s32 dst, int a, int c, int b, sljit_sw ah, sljit_sw ch, sljit_sw bh, bool sub, bool neg)
{
    (void)a;
    (void)c;
    (void)b;
    sljit_emit_fop2(C, SLJIT_MUL_F64, dst, 0, CM, ah, CM, ch);
    sljit_emit_fop2(C, sub ? SLJIT_SUB_F64 : SLJIT_ADD_F64, dst, 0, dst, 0, CM, bh);
    if (neg)
    {
        sljit_emit_fop1(C, SLJIT_NEG_F64, dst, 0, dst, 0);
    }
}

// ---- inline instructions ------------------------------------------------------------------------
bool LiveGen::inline_op(const Insn& i, uint32_t hw)
{
    const int d = i.d, a = i.a, b = i.b, cc = i.c;
    auto op2 = [&](sljit_s32 op, int dst, sljit_s32 s1, sljit_sw w1, sljit_s32 s2, sljit_sw w2) {
        sljit_emit_op2(C, op, CM, OR_(dst), s1, w1, s2, w2);
    };
    switch (i.op)
    {
    // ---- integer ----
    case Op::Addi:
        if (a == 0) sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(d), IMM, I32((uint32_t)i.simm));
        else op2(SLJIT_ADD32, d, CM, OR_(a), IMM, I32((uint32_t)i.simm));
        return true;
    case Op::Addis:
        if (a == 0) sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(d), IMM, I32((uint32_t)i.simm << 16));
        else op2(SLJIT_ADD32, d, CM, OR_(a), IMM, I32((uint32_t)i.simm << 16));
        return true;
    case Op::Add:
        if (i.oe) return false;
        op2(SLJIT_ADD32, d, CM, OR_(a), CM, OR_(b));
        if (i.rc) cr0(d);
        return true;
    case Op::Subf:
        if (i.oe) return false;
        op2(SLJIT_SUB32, d, CM, OR_(b), CM, OR_(a));
        if (i.rc) cr0(d);
        return true;
    case Op::Neg:
        if (i.oe) return false;
        op2(SLJIT_SUB32, d, IMM, 0, CM, OR_(a));
        if (i.rc) cr0(d);
        return true;
    case Op::Mulli:
        op2(SLJIT_MUL32, d, CM, OR_(a), IMM, I32((uint32_t)i.simm));
        return true;
    case Op::Mullw:
        if (i.oe) return false;
        op2(SLJIT_MUL32, d, CM, OR_(a), CM, OR_(b));
        if (i.rc) cr0(d);
        return true;
    case Op::And: case Op::Or: case Op::Xor:
        if (i.op == Op::Or && d == b) sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), CM, OR_(d));
        else op2(i.op == Op::And ? SLJIT_AND32 : i.op == Op::Or ? SLJIT_OR32 : SLJIT_XOR32, a, CM, OR_(d), CM, OR_(b));
        if (i.rc) cr0(a);
        return true;
    case Op::Andc: case Op::Orc:
        sljit_emit_op2(C, SLJIT_XOR32, R0, 0, CM, OR_(b), IMM, -1);
        op2(i.op == Op::Andc ? SLJIT_AND32 : SLJIT_OR32, a, CM, OR_(d), R0, 0);
        if (i.rc) cr0(a);
        return true;
    case Op::Nand: case Op::Nor: case Op::Eqv:
        sljit_emit_op2(C, i.op == Op::Nand ? SLJIT_AND32 : i.op == Op::Nor ? SLJIT_OR32 : SLJIT_XOR32, R0, 0, CM, OR_(d),
                       CM, OR_(b));
        op2(SLJIT_XOR32, a, R0, 0, IMM, -1);
        if (i.rc) cr0(a);
        return true;
    case Op::Ori: op2(SLJIT_OR32, a, CM, OR_(d), IMM, I32(i.uimm)); return true;
    case Op::Oris: op2(SLJIT_OR32, a, CM, OR_(d), IMM, I32(i.uimm << 16)); return true;
    case Op::Xori: op2(SLJIT_XOR32, a, CM, OR_(d), IMM, I32(i.uimm)); return true;
    case Op::Xoris: op2(SLJIT_XOR32, a, CM, OR_(d), IMM, I32(i.uimm << 16)); return true;
    case Op::AndiRc: op2(SLJIT_AND32, a, CM, OR_(d), IMM, I32(i.uimm)); cr0(a); return true;
    case Op::AndisRc: op2(SLJIT_AND32, a, CM, OR_(d), IMM, I32(i.uimm << 16)); cr0(a); return true;
    case Op::Extsb: case Op::Extsh:
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        sljit_emit_op1(C, i.op == Op::Extsb ? SLJIT_MOV32_S8 : SLJIT_MOV32_S16, R0, 0, R0, 0);
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), R0, 0);
        if (i.rc) cr0(a);
        return true;
    case Op::Cntlzw:
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        sljit_emit_op1(C, SLJIT_CLZ32, R0, 0, R0, 0);
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), R0, 0);
        if (i.rc) cr0(a);
        return true;
    case Op::Rlwinm:
    {
        const uint32_t m = ppc_mask(cc, i.e);
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        if (b) sljit_emit_op2(C, SLJIT_ROTL32, R0, 0, R0, 0, IMM, b);
        if (m != 0xFFFFFFFFu) sljit_emit_op2(C, SLJIT_AND32, R0, 0, R0, 0, IMM, I32(m));
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), R0, 0);
        if (i.rc) cr0(a);
        return true;
    }
    case Op::Rlwimi:
    {
        const uint32_t m = ppc_mask(cc, i.e);
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        if (b) sljit_emit_op2(C, SLJIT_ROTL32, R0, 0, R0, 0, IMM, b);
        sljit_emit_op2(C, SLJIT_AND32, R0, 0, R0, 0, IMM, I32(m));
        sljit_emit_op2(C, SLJIT_AND32, R1, 0, CM, OR_(a), IMM, I32(~m));
        op2(SLJIT_OR32, a, R0, 0, R1, 0);
        if (i.rc) cr0(a);
        return true;
    }
    case Op::Rlwnm:
    {
        const uint32_t m = ppc_mask(cc, i.e);
        sljit_emit_op1(C, SLJIT_MOV32, R1, 0, CM, OR_(b));
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        sljit_emit_op2(C, SLJIT_ROTL32, R0, 0, R0, 0, R1, 0);
        if (m != 0xFFFFFFFFu) sljit_emit_op2(C, SLJIT_AND32, R0, 0, R0, 0, IMM, I32(m));
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), R0, 0);
        if (i.rc) cr0(a);
        return true;
    }
    case Op::Slw: case Op::Srw:
        // (rB & 0x20) ? 0 : rS shifted by rB & 31
        sljit_emit_op1(C, SLJIT_MOV32, R1, 0, CM, OR_(b));
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        sljit_emit_op2(C, i.op == Op::Slw ? SLJIT_MSHL32 : SLJIT_MLSHR32, R0, 0, R0, 0, R1, 0);
        sljit_emit_op1(C, SLJIT_MOV32, R2, 0, IMM, 0);
        sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, R1, 0, IMM, 0x20);
        sljit_emit_select(C, SLJIT_NOT_ZERO | SLJIT_32, R0, R2, 0, R0);
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), R0, 0);
        if (i.rc) cr0(a);
        return true;
    case Op::Srawi:
        // gcnr_sraw(c, rS, SH): CA when a negative value lost 1 bits
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(d));
        if (b == 0)
        {
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(xer_ca), IMM, 0);
            sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), R0, 0);
        }
        else
        {
            sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, R0, 0, IMM, I32((1u << b) - 1u));
            sljit_emit_op_flags(C, SLJIT_MOV32, R2, 0, SLJIT_NOT_ZERO);
            sljit_emit_op2(C, SLJIT_LSHR32, R3, 0, R0, 0, IMM, 31);
            sljit_emit_op2(C, SLJIT_AND32, CM, OFF(xer_ca), R2, 0, R3, 0);
            op2(SLJIT_ASHR32, a, R0, 0, IMM, b);
        }
        if (i.rc) cr0(a);
        return true;

    // ---- compare ----
    case Op::Cmp: case Op::Cmpl:
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(a));
        set_crf(i.crfd(), i.op == Op::Cmp, CM, OR_(b));
        return true;
    case Op::Cmpi:
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(a));
        set_crf(i.crfd(), true, IMM, I32((uint32_t)i.simm));
        return true;
    case Op::Cmpli:
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OR_(a));
        set_crf(i.crfd(), false, IMM, I32(i.uimm));
        return true;

    // ---- LR / CTR ----
    case Op::Mfspr:
        if (i.spr != 8 && i.spr != 9) return false;
        sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(d), CM, i.spr == 8 ? OFF(lr) : OFF(ctr));
        return true;
    case Op::Mtspr:
        if (i.spr != 8 && i.spr != 9) return false;
        sljit_emit_op1(C, SLJIT_MOV32, CM, i.spr == 8 ? OFF(lr) : OFF(ctr), CM, OR_(d));
        return true;

    // ---- integer loads / stores (hardware registers proven by the analysis: gcnl_exec) ----
    case Op::Lbz: case Op::Lbzu: case Op::Lbzx: case Op::Lbzux:
    case Op::Lhz: case Op::Lhzu: case Op::Lhzx: case Op::Lhzux:
    case Op::Lha: case Op::Lhau: case Op::Lhax: case Op::Lhaux:
    case Op::Lwz: case Op::Lwzu: case Op::Lwzx: case Op::Lwzux:
    {
        if (hw) return false;
        const bool x = i.op == Op::Lbzx || i.op == Op::Lbzux || i.op == Op::Lhzx || i.op == Op::Lhzux ||
                       i.op == Op::Lhax || i.op == Op::Lhaux || i.op == Op::Lwzx || i.op == Op::Lwzux;
        const bool u = i.op == Op::Lbzu || i.op == Op::Lbzux || i.op == Op::Lhzu || i.op == Op::Lhzux ||
                       i.op == Op::Lhau || i.op == Op::Lhaux || i.op == Op::Lwzu || i.op == Op::Lwzux;
        const bool lb = i.op == Op::Lbz || i.op == Op::Lbzu || i.op == Op::Lbzx || i.op == Op::Lbzux;
        const bool lw = i.op == Op::Lwz || i.op == Op::Lwzu || i.op == Op::Lwzx || i.op == Op::Lwzux;
        const bool la = i.op == Op::Lha || i.op == Op::Lhau || i.op == Op::Lhax || i.op == Op::Lhaux;
        load_int(i, x, u, lb ? 1 : lw ? 4 : 2, la);
        return true;
    }
    case Op::Stb: case Op::Stbu: case Op::Stbx: case Op::Stbux:
    case Op::Sth: case Op::Sthu: case Op::Sthx: case Op::Sthux:
    case Op::Stw: case Op::Stwu: case Op::Stwx: case Op::Stwux:
    {
        if (hw || !mInlineStores) return false;
        const bool x = i.op == Op::Stbx || i.op == Op::Stbux || i.op == Op::Sthx || i.op == Op::Sthux ||
                       i.op == Op::Stwx || i.op == Op::Stwux;
        const bool u = i.op == Op::Stbu || i.op == Op::Stbux || i.op == Op::Sthu || i.op == Op::Sthux ||
                       i.op == Op::Stwu || i.op == Op::Stwux;
        const bool sb = i.op == Op::Stb || i.op == Op::Stbu || i.op == Op::Stbx || i.op == Op::Stbux;
        const bool sw = i.op == Op::Stw || i.op == Op::Stwu || i.op == Op::Stwx || i.op == Op::Stwux;
        store_int(i, x, u, sb ? 1 : sw ? 4 : 2);
        return true;
    }

    // ---- floating point loads / stores ----
    case Op::Lfs: case Op::Lfsu: case Op::Lfsx: case Op::Lfsux:
    case Op::Lfd: case Op::Lfdu: case Op::Lfdx: case Op::Lfdux:
    {
        if (hw) return false;
        const bool x = i.op == Op::Lfsx || i.op == Op::Lfsux || i.op == Op::Lfdx || i.op == Op::Lfdux;
        const bool u = i.op == Op::Lfsu || i.op == Op::Lfsux || i.op == Op::Lfdu || i.op == Op::Lfdux;
        const bool single = i.op == Op::Lfs || i.op == Op::Lfsu || i.op == Op::Lfsx || i.op == Op::Lfsux;
        x ? ea_x(i) : ea_d(i);
        if (u) sljit_emit_op1(C, SLJIT_MOV32, LOCAL, 0, R0, 0);
        mem_load(single ? 4 : 8, false);
        if (single)
        {
            // (double)gcnr_f32(word) into both halves
            sljit_emit_fcopy(C, SLJIT_COPY32_TO_F32, FR0, R2);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_F32, FR0, 0, FR0, 0);
            sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
            sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR0, 0);
        }
        else
        {
            sljit_emit_op1(C, SLJIT_MOV, CM, OF0(d), R2, 0);
        }
        if (u) sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), LOCAL, 0);
        return true;
    }
    case Op::Stfs: case Op::Stfsu: case Op::Stfsx: case Op::Stfsux:
    case Op::Stfd: case Op::Stfdu: case Op::Stfdx: case Op::Stfdux:
    {
        if (hw || !mInlineStores) return false;
        const bool x = i.op == Op::Stfsx || i.op == Op::Stfsux || i.op == Op::Stfdx || i.op == Op::Stfdux;
        const bool u = i.op == Op::Stfsu || i.op == Op::Stfsux || i.op == Op::Stfdu || i.op == Op::Stfdux;
        const bool single = i.op == Op::Stfs || i.op == Op::Stfsu || i.op == Op::Stfsx || i.op == Op::Stfsux;
        if (single)
        {
            // gcnr_bits32((float)ps0)
            sljit_emit_fop1(C, SLJIT_CONV_F32_FROM_F64, FR0, 0, CM, OF0(d));
            sljit_emit_fcopy(C, SLJIT_COPY32_FROM_F32, FR0, R2);
        }
        else
        {
            sljit_emit_op1(C, SLJIT_MOV, R2, 0, CM, OF0(d));
        }
        x ? ea_x(i) : ea_d(i);
        if (u) sljit_emit_op1(C, SLJIT_MOV32, LOCAL, 0, R0, 0);
        mem_store(single ? 4 : 8);
        if (u) sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(a), LOCAL, 0);
        return true;
    }

    // ---- floating point arithmetic (record forms: gcnl_exec) ----
    case Op::Fadd: case Op::Fsub: case Op::Fmul: case Op::Fdiv:
    case Op::Fadds: case Op::Fsubs: case Op::Fmuls: case Op::Fdivs:
    {
        if (i.rc) return false;
        const bool mul = i.op == Op::Fmul || i.op == Op::Fmuls;
        const sljit_s32 op = (i.op == Op::Fadd || i.op == Op::Fadds) ? SLJIT_ADD_F64
                           : (i.op == Op::Fsub || i.op == Op::Fsubs) ? SLJIT_SUB_F64
                           : mul ? SLJIT_MUL_F64 : SLJIT_DIV_F64;
        sljit_emit_fop2(C, op, FR0, 0, CM, OF0(a), CM, OF0(mul ? cc : b));
        if (i.op == Op::Fadd || i.op == Op::Fsub || i.op == Op::Fmul || i.op == Op::Fdiv)
        {
            sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        }
        else
        {
            round_single(FR0);
            sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
            sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR0, 0);
        }
        return true;
    }
    case Op::Fmadd: case Op::Fmsub: case Op::Fnmadd: case Op::Fnmsub:
    case Op::Fmadds: case Op::Fmsubs: case Op::Fnmadds: case Op::Fnmsubs:
    {
        if (i.rc) return false;
        const bool sub = i.op == Op::Fmsub || i.op == Op::Fnmsub || i.op == Op::Fmsubs || i.op == Op::Fnmsubs;
        const bool neg = i.op == Op::Fnmadd || i.op == Op::Fnmsub || i.op == Op::Fnmadds || i.op == Op::Fnmsubs;
        const bool single = i.op == Op::Fmadds || i.op == Op::Fmsubs || i.op == Op::Fnmadds || i.op == Op::Fnmsubs;
        fp_madd(FR0, a, cc, b, OF0(a), OF0(cc), OF0(b), sub, neg);
        if (single) round_single(FR0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        if (single) sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR0, 0);
        return true;
    }
    case Op::Frsp:
        if (i.rc) return false;
        sljit_emit_fop1(C, SLJIT_MOV_F64, FR0, 0, CM, OF0(b));
        round_single(FR0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR0, 0);
        return true;
    case Op::Fmr: case Op::Fneg: case Op::Fabs: case Op::Fnabs:
        if (i.rc) return false;
        sljit_emit_fop1(C, SLJIT_MOV_F64, FR0, 0, CM, OF0(b));
        if (i.op == Op::Fneg) sljit_emit_fop1(C, SLJIT_NEG_F64, FR0, 0, FR0, 0);
        if (i.op == Op::Fabs || i.op == Op::Fnabs) sljit_emit_fop1(C, SLJIT_ABS_F64, FR0, 0, FR0, 0);
        if (i.op == Op::Fnabs) sljit_emit_fop1(C, SLJIT_NEG_F64, FR0, 0, FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        return true;

    // ---- paired singles (record forms: gcnl_exec) ----
    case Op::PsAdd: case Op::PsSub: case Op::PsMul: case Op::PsDiv:
    {
        if (i.rc) return false;
        const sljit_s32 op = i.op == Op::PsAdd ? SLJIT_ADD_F64 : i.op == Op::PsSub ? SLJIT_SUB_F64
                           : i.op == Op::PsMul ? SLJIT_MUL_F64 : SLJIT_DIV_F64;
        const int s = i.op == Op::PsMul ? cc : b;
        sljit_emit_fop2(C, op, FR0, 0, CM, OF0(a), CM, OF0(s));
        sljit_emit_fop2(C, op, FR1, 0, CM, OF1(a), CM, OF1(s));
        round_single(FR0);
        round_single(FR1);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR1, 0);
        return true;
    }
    case Op::PsMadd: case Op::PsMsub: case Op::PsNmadd: case Op::PsNmsub: case Op::PsMadds0: case Op::PsMadds1:
    {
        if (i.rc) return false;
        const bool sub = i.op == Op::PsMsub || i.op == Op::PsNmsub;
        const bool neg = i.op == Op::PsNmadd || i.op == Op::PsNmsub;
        const sljit_sw c0 = i.op == Op::PsMadds1 ? OF1(cc) : OF0(cc);
        const sljit_sw c1 = i.op == Op::PsMadds0 ? OF0(cc) : OF1(cc);
        fp_madd(FR0, a, cc, b, OF0(a), c0, OF0(b), sub, neg);
        fp_madd(FR1, a, cc, b, OF1(a), c1, OF1(b), sub, neg);
        round_single(FR0);
        round_single(FR1);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR1, 0);
        return true;
    }
    case Op::PsMuls0: case Op::PsMuls1:
    {
        if (i.rc) return false;
        const sljit_sw cs = i.op == Op::PsMuls0 ? OF0(cc) : OF1(cc);
        sljit_emit_fop2(C, SLJIT_MUL_F64, FR0, 0, CM, OF0(a), CM, cs);
        sljit_emit_fop2(C, SLJIT_MUL_F64, FR1, 0, CM, OF1(a), CM, cs);
        round_single(FR0);
        round_single(FR1);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR1, 0);
        return true;
    }
    case Op::PsSum0: case Op::PsSum1:
    {
        // sum0: (a0 + b1, c1), sum1: (c0, a0 + b1), both rounded
        if (i.rc) return false;
        const bool s0 = i.op == Op::PsSum0;
        sljit_emit_fop2(C, SLJIT_ADD_F64, s0 ? FR0 : FR1, 0, CM, OF0(a), CM, OF1(b));
        sljit_emit_fop1(C, SLJIT_MOV_F64, s0 ? FR1 : FR0, 0, CM, s0 ? OF1(cc) : OF0(cc));
        round_single(FR0);
        round_single(FR1);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR1, 0);
        return true;
    }
    case Op::PsMr: case Op::PsNeg: case Op::PsAbs: case Op::PsNabs:
    case Op::PsMerge00: case Op::PsMerge01: case Op::PsMerge10: case Op::PsMerge11:
    {
        if (i.rc) return false;
        sljit_sw s0 = OF0(b), s1 = OF1(b);
        if (i.op == Op::PsMerge00) { s0 = OF0(a); s1 = OF0(b); }
        if (i.op == Op::PsMerge01) { s0 = OF0(a); s1 = OF1(b); }
        if (i.op == Op::PsMerge10) { s0 = OF1(a); s1 = OF0(b); }
        if (i.op == Op::PsMerge11) { s0 = OF1(a); s1 = OF1(b); }
        sljit_emit_fop1(C, SLJIT_MOV_F64, FR0, 0, CM, s0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, FR1, 0, CM, s1);
        for (sljit_s32 fr : {FR0, FR1})
        {
            if (i.op == Op::PsNeg) sljit_emit_fop1(C, SLJIT_NEG_F64, fr, 0, fr, 0);
            if (i.op == Op::PsAbs || i.op == Op::PsNabs) sljit_emit_fop1(C, SLJIT_ABS_F64, fr, 0, fr, 0);
            if (i.op == Op::PsNabs) sljit_emit_fop1(C, SLJIT_NEG_F64, fr, 0, fr, 0);
        }
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF0(d), FR0, 0);
        sljit_emit_fop1(C, SLJIT_MOV_F64, CM, OF1(d), FR1, 0);
        return true;
    }

    case Op::Mtsr: case Op::Mtsrin: case Op::Tlbie: case Op::Tlbsync: case Op::Sync: case Op::Isync: case Op::Eieio:
    case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Icbi:
        return true; // no effect here
    default:
        return false;
    }
}

// ---- one instruction ----------------------------------------------------------------------------
void LiveGen::insn(const Analysis& an, const Insn& i)
{
    const uint32_t next = i.addr + 4;
    std::vector<sljit_jump*> no;
    if (mIn.dynamic.count(i.addr))
    {
        // relocated against another module: OSLink / OSUnlink of that module rewrite it, so it is
        // decoded when it runs
        sljit_emit_op1(C, SLJIT_MOV_P, R0, 0, MEM, 0);
        sljit_emit_op1(C, SLJIT_MOV_P, R1, 0, CTX, 0);
        sljit_emit_op1(C, SLJIT_MOV32, R2, 0, IMM, I32(i.addr));
        if (i.op == Op::B)
        {
            if (i.lk)
            {
                sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            }
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32), IMM, SLJIT_FUNC_ADDR(gcnl_call_insn));
            if (!i.lk)
            {
                ret();
            }
        }
        else if (gekko::is_branch(i.op))
        {
            unhandled(i.addr, "conditional branch relocated against another module");
        }
        else
        {
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32), IMM, SLJIT_FUNC_ADDR(gcnl_exec_at));
        }
        mOut.helpers++;
        return;
    }
    switch (i.op)
    {
    case Op::B:
        if (i.lk && an.lrMode && i.target != next && an.local(i.target))
        {
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            jump_local(i.target);
        }
        else if (i.lk)
        {
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            if (i.target != next)
            {
                call_target(i.target);
            }
        }
        else if (an.local(i.target))
        {
            loop_checks(an, i.addr);
            jump_local(i.target);
        }
        else
        {
            call_target(i.target);
            ret();
        }
        return;
    case Op::Bc:
    {
        const bool local = an.local(i.target);
        if (!i.lk && local && !an.spinLoops.count(i.addr) && (i.bo() & 0x14) != 0)
        {
            // one condition (CTR or a CR bit): straight to the target when it holds (a loop's
            // count goes up on the way out too: harmless)
            if (an.backBranches.count(i.addr)) loop_check(false);
            const int bo = i.bo();
            if (!(bo & 4))
            {
                sljit_emit_op2(C, SLJIT_SUB32 | SLJIT_SET_Z, R0, 0, CM, OFF(ctr), IMM, 1);
                sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(ctr), R0, 0);
                jump_local(i.target, sljit_emit_jump(C, (bo & 2) ? SLJIT_ZERO : SLJIT_NOT_ZERO));
            }
            else if (!(bo & 16))
            {
                sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, CM, OFF(cr), IMM, I32(0x80000000u >> i.bi()));
                jump_local(i.target, sljit_emit_jump(C, (bo & 8) ? SLJIT_NOT_ZERO : SLJIT_ZERO));
            }
            else
            {
                jump_local(i.target);
            }
            return;
        }
        std::vector<sljit_jump*> f;
        cond_false_jumps(i, f);
        if (i.lk && an.lrMode && i.target != next && local)
        {
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            jump_local(i.target);
        }
        else if (i.lk)
        {
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            call_target(i.target);
        }
        else if (local)
        {
            loop_checks(an, i.addr);
            jump_local(i.target);
        }
        else
        {
            call_target(i.target);
            ret();
        }
        land(f);
        return;
    }
    case Op::Bclr:
    {
        std::vector<sljit_jump*> f;
        cond_false_jumps(i, f);
        if (i.lk)
        {
            // t = LR; LR = next; call t
            sljit_emit_op1(C, SLJIT_MOV32, R2, 0, CM, OFF(lr));
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            call_lookup_r2();
        }
        else if (an.lrMode && !an.lrSites.empty())
        {
            sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OFF(lr));
            for (uint32_t s : an.lrSites)
            {
                jump_local(s, sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, R0, 0, IMM, I32(s)));
            }
            ret();
        }
        else
        {
            ret();
        }
        land(f);
        return;
    }
    case Op::Bcctr:
    {
        std::vector<sljit_jump*> f;
        cond_false_jumps(i, f);
        auto sw = an.switches.find(i.addr);
        if (i.lk)
        {
            sljit_emit_op1(C, SLJIT_MOV32, CM, OFF(lr), IMM, I32(next));
            sljit_emit_op1(C, SLJIT_MOV32, R2, 0, CM, OFF(ctr));
            call_lookup_r2();
        }
        else if (sw != an.switches.end())
        {
            sljit_emit_op1(C, SLJIT_MOV32, R0, 0, CM, OFF(ctr));
            std::set<uint32_t> done;
            for (uint32_t t : sw->second)
            {
                if (done.insert(t).second)
                {
                    jump_local(t, sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, R0, 0, IMM, I32(t)));
                }
            }
            unhandled(i.addr, "jump table target");
            ret();
        }
        else
        {
            sljit_emit_op1(C, SLJIT_MOV32, R2, 0, CM, OFF(ctr));
            call_lookup_r2();
            ret();
        }
        land(f);
        return;
    }
    case Op::Rfi:
        unhandled(i.addr, "rfi");
        ret();
        return;
    case Op::Invalid:
        helper(i, 0); // gcnl_exec reports it
        return;
    default:
        break;
    }
    uint32_t hw = 0;
    auto known = an.knownEa.find(i.addr);
    if (known != an.knownEa.end() && (known->second >> 24) == 0xCC)
    {
        hw = known->second;
    }
    if (mInline && inline_op(i, hw))
    {
        mOut.inlined++;
        return;
    }
    helper(i, hw);
}

// ---- functions ----------------------------------------------------------------------------------
void LiveGen::function(const Function& f)
{
    const Analysis an = analyze(P, f);
    mAn = &an;
    mLabels.clear();
    mLocal.clear();
    for (const std::string& w : an.warnings)
    {
        mOut.warnings.push_back((f.name.empty() ? hex(f.addr) : f.name) + ": " + w);
    }
    if (an.lrMode)
    {
        mBody[f.addr] = sljit_emit_label(C);
        sljit_emit_enter(C, 0, SLJIT_ARGS3V(P, P, 32), 4 | SLJIT_ENTER_FLOAT(3), 3, 16);
        for (uint32_t e : an.entries)
        {
            jump_local(e, sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, ENTRY, 0, IMM, I32(e)));
        }
        sljit_emit_op1(C, SLJIT_MOV_P, R0, 0, CTX, 0);
        sljit_emit_op1(C, SLJIT_MOV32, R1, 0, ENTRY, 0);
        sljit_emit_op1(C, SLJIT_MOV_P, R2, 0, IMM, (sljit_sw) "entry into an LR-mode body");
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, 32, P), IMM, SLJIT_FUNC_ADDR(gcnr_unhandled));
        ret();
    }
    else
    {
        mEntry[f.addr] = sljit_emit_label(C);
        sljit_emit_enter(C, 0, SLJIT_ARGS2V(P, P), 4 | SLJIT_ENTER_FLOAT(3), 2, 16);
    }
    if (mTraced.count(f.addr))
    {
        sljit_emit_op1(C, SLJIT_MOV32, R0, 0, IMM, I32(f.addr));
        sljit_emit_op1(C, SLJIT_MOV_P, R1, 0, MEM, 0);
        sljit_emit_op1(C, SLJIT_MOV_P, R2, 0, CTX, 0);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(32, P, P), IMM, SLJIT_FUNC_ADDR(gcnr_trace_func));
    }
    bool endsInBranch = false;
    for (const Insn& i : an.insns)
    {
        if (an.labels.count(i.addr))
        {
            mLabels[i.addr] = sljit_emit_label(C);
        }
        insn(an, i);
        endsInBranch = uncond_branch(i);
    }
    mOut.instructions += an.insns.size();
    if (!endsInBranch)
    {
        // runs off the end: into whatever follows (normally unreachable)
        if (P.function_at(f.end) != nullptr)
        {
            call_target(f.end);
        }
        ret();
    }
    for (const Pending& p : mLocal)
    {
        auto l = mLabels.find(p.target);
        if (l == mLabels.end())
        {
            mOut.error = "live: no label at " + hex(p.target) + " in function " + hex(f.addr);
            return;
        }
        sljit_set_label(p.jump, l->second);
    }
    if (an.lrMode)
    {
        // f_ADDR: the body entered at its start
        mEntry[f.addr] = sljit_emit_label(C);
        sljit_emit_enter(C, 0, SLJIT_ARGS2V(P, P), 4, 2, 0);
        args_mem_ctx();
        sljit_emit_op1(C, SLJIT_MOV32, R2, 0, IMM, I32(f.addr));
        mBodyCalls.push_back({sljit_emit_call(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32)), f.addr});
        ret();
    }
    mOut.recompiled++;
    mAn = nullptr;
}

void LiveGen::lr_stub(const Function& f, const Function& parent)
{
    mEntry[f.addr] = sljit_emit_label(C);
    sljit_emit_enter(C, 0, SLJIT_ARGS2V(P, P), 4, 2, 0);
    args_mem_ctx();
    sljit_emit_op1(C, SLJIT_MOV32, R2, 0, IMM, I32(f.addr));
    mBodyCalls.push_back({sljit_emit_call(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32)), parent.addr});
    ret();
}

bool LiveGen::run()
{
    C = sljit_create_compiler(nullptr);
    if (C == nullptr)
    {
        mOut.error = "live: sljit_create_compiler failed";
        return false;
    }
    for (const auto& [addr, f] : P.functions)
    {
        if (f.hle)
        {
            if (mHleByName.count(f.name) == 0)
            {
                mOut.error = "live: no HLE wrapper hle_" + f.name + " (the HLE module and syms.txt disagree)";
                break;
            }
            continue;
        }
        if (f.stub)
        {
            // syms.txt `stub`: hardware the runtime does not have (returns the value in r3)
            mEntry[f.addr] = sljit_emit_label(C);
            sljit_emit_enter(C, 0, SLJIT_ARGS2V(P, P), 4, 2, 0);
            sljit_emit_op1(C, SLJIT_MOV32, CM, OR_(3), IMM, I32(f.stubValue));
            ret();
            continue;
        }
        if (f.extra)
        {
            const Function* parent = P.function_containing(f.addr);
            if (parent != nullptr && !parent->hle && needs_lr_mode(P, *parent))
            {
                lr_stub(f, *parent);
                continue;
            }
        }
        function(f);
        if (!mOut.error.empty() || sljit_get_compiler_error(C) != SLJIT_SUCCESS)
        {
            break;
        }
    }
    if (mOut.error.empty() && sljit_get_compiler_error(C) == SLJIT_SUCCESS)
    {
        for (const Pending& p : mCalls)
        {
            auto l = mEntry.find(p.target);
            if (l == mEntry.end())
            {
                mOut.error = "live: call to " + hex(p.target) + ", which was not recompiled";
                break;
            }
            sljit_set_label(p.jump, l->second);
        }
        for (const Pending& p : mBodyCalls)
        {
            auto l = mBody.find(p.target);
            if (l == mBody.end())
            {
                mOut.error = "live: no LR-mode body at " + hex(p.target);
                break;
            }
            sljit_set_label(p.jump, l->second);
        }
    }
    if (mOut.error.empty() && sljit_get_compiler_error(C) != SLJIT_SUCCESS)
    {
        mOut.error = "live: sljit error " + std::to_string(sljit_get_compiler_error(C));
    }
    if (mOut.error.empty())
    {
        mOut.code = sljit_generate_code(C, 0, nullptr);
        mOut.codeSize = sljit_get_generated_code_size(C);
        if (mOut.code == nullptr)
        {
            mOut.error = "live: sljit_generate_code failed (" + std::to_string(sljit_get_compiler_error(C)) + ")";
        }
    }
    if (mOut.error.empty())
    {
        for (const auto& [addr, f] : P.functions)
        {
            LiveFunction lf;
            lf.addr = addr;
            if (f.hle)
            {
                lf.fn = mHleByName[f.name];
            }
            else
            {
                lf.fn = reinterpret_cast<gcnr_func>(sljit_get_label_addr(mEntry[addr]));
            }
            mOut.functions.push_back(lf);
        }
    }
    sljit_free_compiler(C);
    C = nullptr;
    return mOut.error.empty();
}
} // namespace

namespace
{
// the code and gcnl_exec's arguments (in the generator) live as long as the result
struct Owner
{
    std::unique_ptr<LiveGen> gen;
    void* code = nullptr;
    ~Owner()
    {
        if (code != nullptr)
        {
            sljit_free_code(code, nullptr);
        }
    }
};
} // namespace

LiveResult live_recompile(Program& program, const LiveInputs& inputs)
{
    LiveResult out;
    auto owner = std::make_shared<Owner>();
    owner->gen = std::make_unique<LiveGen>(program, inputs, out);
    owner->gen->run();
    owner->code = out.code;
    out.owner = owner;
    return out;
}
} // namespace gcnr
