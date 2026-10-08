/*
 * Functions the recomp runtime implements itself (syms.txt `native <name>`, from a game package's
 * names.txt): hand-written guest code the recompiler cannot express, done here with the guest
 * registers in hand. gen_hle_glue.py makes hle_<name>, which calls gcnr_native_call; the
 * recompiled code calls that as it calls the HLE's functions (AOT and Live alike).
 *
 * ---- GS engine threads (Genius Sonority: Pokemon Colosseum, Pokemon XD) ---------------------
 * The engine runs cooperative threads of its own on top of the SDK's. Its scheduler calls
 * threadExecute, which saves the scheduler's registers into a static block and jumps into the
 * thread on the thread's stack, at the resume address in the thread's context block with the
 * thread's exit routine as LR. The thread gives the CPU back with _threadSwitch: it saves its
 * registers and its return address into its context block and "returns" into the scheduler by
 * loading the scheduler's stack pointer and return address. Recompiled code returns natively, so
 * each GS thread runs on a host coroutine of its own here instead:
 *   threadExecute  starts the thread's coroutine (registers from the context block, at its
 *                  resume address, LR its exit routine), or resumes the one suspended in
 *                  _threadSwitch, its registers reloaded from the context block as the original
 *                  reloads them (the scheduler rebases the saved stack pointer between runs, its
 *                  memory manager may move the stack; GSthreadSetArgs writes the argument slots)
 *   _threadSwitch  saves the registers and the resume address into the context block, as the
 *                  original does, and switches back to the scheduler's coroutine.
 * Context block: r0..r31 at 0, the resume address at 128, LR at 132, f0..f31 (doubles) at 136
 * when the thread uses the FPU. The scheduler's globals (r13-relative) come from threadExecute's
 * own instructions. A context block can move, so the coroutine is found by a tag left in its r0
 * slot (r0 holds nothing across the call) together with the resume address.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gcn_platform.h"
#include "gcn_recomp.h"
#include "gcnw_backend.h"
#include "gcnw_module.h"

const GcnwModule* gcnr_module(void);

static void fatal(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gcnp_log(buf);
    gcnp_crashed();
}

/* ---- GS engine threads ---------------------------------------------------------------------- */
#define GS_MAX 64
#define GS_TAG 0x47530000u /* 'GS' + the coroutine's index, in the context block's r0 slot */
#define GS_STACK (1u << 20)

typedef struct
{
    GcnpCoro* coro;
    GcnpCoro* sched;  /* the coroutine that ran threadExecute */
    gcnr_ctx ctx;     /* the thread's registers (the recompiled code works on these) */
    uint32_t startPc, startLr;
    uint32_t yieldPc; /* where _threadSwitch returns to while suspended */
    uint32_t last;    /* when threadExecute last ran it */
    int used, suspended, done;
} GsFiber;

static GsFiber sGs[GS_MAX];
static uint32_t sGsClock, sGsRun;
static int sGsCurrent = -1;
static uint32_t sOffS, sOffT, sOffCur, sOffFpu; /* r13 + these: scheduler block, thread block, ... */
static uint8_t* sGsMem;

/* lwz / stw rD,d(r13) at addr: d (sign-extended), or fatal */
static uint32_t r13_offset(uint8_t* mem, uint32_t addr, uint32_t op, uint32_t rd, const char* what)
{
    const uint32_t w = gcnr_lw(mem, addr);
    if ((w >> 26) != op || ((w >> 21) & 31) != rd || ((w >> 16) & 31) != 13)
    {
        fatal("recomp: threadExecute at %08X is not the GS engine's (%s: %08X at %08X)", addr & ~0xFFu, what, w,
              addr);
    }
    return (uint32_t)(int32_t)(int16_t)(w & 0xFFFF);
}

static void gs_setup(uint8_t* mem, uint32_t self)
{
    if (sGsRun == gcnw_run_count() && sGsMem) return;
    /* a new game run: the host destroyed every coroutine */
    memset(sGs, 0, sizeof(sGs));
    sGsCurrent = -1;
    sGsRun = gcnw_run_count();
    sGsMem = mem;
    if (!self) fatal("recomp: threadExecute has no address in syms.txt");
    sOffS = r13_offset(mem, self + 0x10, 32, 3, "scheduler block");
    sOffCur = r13_offset(mem, self + 0x14, 36, 3, "save target");
    sOffT = r13_offset(mem, self + 0x20, 32, 3, "thread block");
    sOffFpu = r13_offset(mem, self + 0x2C, 32, 5, "FPU flag");
}

static void gs_save(uint8_t* mem, uint32_t block, const gcnr_ctx* c, int fpu)
{
    int i;
    for (i = 0; i < 32; i++) gcnr_sw(mem, block + 4u * (uint32_t)i, c->r[i]);
    if (fpu)
    {
        for (i = 0; i < 32; i++)
        {
            uint64_t bits;
            memcpy(&bits, &c->f[i].ps0, 8);
            gcnr_sd(mem, block + 136u + 8u * (uint32_t)i, bits);
        }
    }
}

static void gs_load(uint8_t* mem, uint32_t block, gcnr_ctx* c, int fpu)
{
    int i;
    for (i = 0; i < 32; i++) c->r[i] = gcnr_lw(mem, block + 4u * (uint32_t)i);
    if (fpu)
    {
        for (i = 0; i < 32; i++)
        {
            const uint64_t bits = gcnr_ld(mem, block + 136u + 8u * (uint32_t)i);
            memcpy(&c->f[i].ps0, &bits, 8);
        }
    }
}

/* back to the scheduler (from _threadSwitch, or when the thread has ended) */
static void gs_back(GsFiber* f)
{
    GcnpCoro* to = f->sched;
    gcnw_ctx_swap_coro(to); /* the guest thread runs on the scheduler's coroutine again */
    sGsCurrent = -1;
    gcnp_coro_switch(to);
    /* resumed by threadExecute */
}

static void gs_main(void* p)
{
    GsFiber* f = (GsFiber*)p;
    for (;;)
    {
        gcnr_ctx* c = &f->ctx;
        c->lr = f->startLr;
        gcnr_lookup(f->startPc)(sGsMem, c);
        /* the entry returned: the original lands in the exit routine its LR held */
        if (f->startLr)
        {
            const uint32_t exitFn = f->startLr;
            c->lr = 0;
            gcnr_lookup(exitFn)(sGsMem, c);
        }
        f->done = 1;
        gs_back(f); /* resumed only to run a new thread (threadExecute set startPc) */
    }
}

/* a coroutine for a new thread: a free one, one whose thread ended, else the one suspended the
 * longest (a thread the game ended without letting it finish) */
static int gs_alloc(void)
{
    int i, best = -1;
    for (i = 0; i < GS_MAX; i++)
    {
        if (!sGs[i].used || sGs[i].done) return i;
    }
    for (i = 0; i < GS_MAX; i++)
    {
        if (sGs[i].suspended && (best < 0 || sGs[i].last < sGs[best].last)) best = i;
    }
    if (best < 0) fatal("recomp: more than %d GS engine threads", GS_MAX);
    gcnp_coro_destroy(sGs[best].coro);
    sGs[best].coro = NULL;
    sGs[best].used = 0;
    return best;
}

static void gs_execute(uint8_t* mem, gcnr_ctx* c, uint32_t self)
{
    gs_setup(mem, self);
    if (sGsCurrent >= 0) fatal("recomp: threadExecute inside a GS engine thread (lr %08X)", c->lr);
    const uint32_t r13 = c->r[13];
    const uint32_t s = gcnr_lw(mem, r13 + sOffS), t = gcnr_lw(mem, r13 + sOffT);
    const int fpu = gcnr_lw(mem, r13 + sOffFpu) != 0;
    const uint32_t tag = gcnr_lw(mem, t), pc = gcnr_lw(mem, t + 128);
    int id = -1;
    GsFiber* f;

    /* the scheduler's side, as the original saves it (nothing here reads it back) */
    gcnr_sw(mem, r13 + sOffCur, s);
    gs_save(mem, s, c, 0);
    gcnr_sw(mem, s + 4, c->r[1]);

    if ((tag & 0xFFFF0000u) == GS_TAG)
    {
        id = (int)(tag & 0xFFFFu);
        if (id >= GS_MAX || !sGs[id].used || !sGs[id].suspended || sGs[id].yieldPc != pc) id = -1;
    }
    if (id >= 0)
    {
        /* resume where _threadSwitch left it, with the registers the context block holds now */
        f = &sGs[id];
        gs_load(mem, t, &f->ctx, fpu);
        f->ctx.lr = gcnr_lw(mem, t + 132);
    }
    else
    {
        id = gs_alloc();
        f = &sGs[id];
        memset(&f->ctx, 0, sizeof(f->ctx));
        f->ctx.msr = c->msr;
        f->ctx.fpscr = c->fpscr;
        gs_load(mem, t, &f->ctx, fpu);
        gcnr_sw(mem, f->ctx.r[1], 0xFFFFFFFFu); /* the end of the back chain, as the original */
        f->startPc = pc;
        f->startLr = gcnr_lw(mem, t + 132);
        f->done = 0;
        if (!f->coro)
        {
            f->coro = gcnp_coro_create(gs_main, f, GS_STACK);
            if (!f->coro) fatal("recomp: no memory for a GS engine thread's stack");
        }
        f->used = 1;
    }
    gcnr_sw(mem, t, GS_TAG | (uint32_t)id);
    f->suspended = 0;
    f->yieldPc = 0;
    f->last = ++sGsClock;
    f->sched = gcnp_coro_current();
    sGsCurrent = id;
    {
        uint32_t* hleSp = gcnr_module()->stack_pointer();
        const uint32_t savedSp = *hleSp;
        gcnw_ctx_swap_coro(f->coro);
        gcnp_coro_switch(f->coro);
        *hleSp = savedSp;
    }
    /* back: the scheduler's registers are c's, untouched; the original leaves the save target here */
    gcnr_sw(mem, r13 + sOffCur, s);
}

static void gs_switch(uint8_t* mem, gcnr_ctx* c, uint32_t self)
{
    (void)self;
    if (sGsCurrent < 0) fatal("recomp: _threadSwitch outside a GS engine thread (lr %08X)", c->lr);
    GsFiber* f = &sGs[sGsCurrent];
    if (c != &f->ctx) fatal("recomp: _threadSwitch with another register set than its thread's (lr %08X)", c->lr);
    const uint32_t r13 = c->r[13];
    const uint32_t t = gcnr_lw(mem, r13 + sOffT);
    const int fpu = gcnr_lw(mem, r13 + sOffFpu) != 0;
    const uint32_t s = gcnr_lw(mem, r13 + sOffS);

    gcnr_sw(mem, r13 + sOffCur, t);
    gs_save(mem, t, c, fpu); /* r1 at 4, r3 at 12, r5 at 20 among them */
    gcnr_sw(mem, t + 128, c->lr);
    gcnr_sw(mem, t, GS_TAG | (uint32_t)sGsCurrent);
    gcnr_sw(mem, r13 + sOffCur, s);
    f->yieldPc = c->lr;
    f->suspended = 1;
    gs_back(f);
    /* resumed: threadExecute reloaded c from the context block */
}

/* ---- the table ------------------------------------------------------------------------------ */
typedef void (*gcnr_native_fn)(uint8_t* mem, gcnr_ctx* c, uint32_t self);
static const struct
{
    const char* name;
    gcnr_native_fn fn;
} sNatives[] = {
    {"threadExecute", gs_execute},
    {"_threadSwitch", gs_switch},
};

void gcnr_native_call(const char* name, uint32_t self, uint8_t* mem, gcnr_ctx* c)
{
    size_t i;
    for (i = 0; i < sizeof(sNatives) / sizeof(sNatives[0]); i++)
    {
        if (!strcmp(sNatives[i].name, name))
        {
            sNatives[i].fn(mem, c, self);
            return;
        }
    }
    fatal("recomp: no native implementation of %s (syms.txt native lines: recomp_native.c)", name);
}
