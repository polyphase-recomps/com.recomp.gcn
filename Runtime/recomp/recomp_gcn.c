/**
 * @file recomp_gcn.c
 * @brief The recompiled game's runtime (gcn_recomp.h): function lookup, calls between the
 *        recompiled code and the HLE module, loop checks, hardware registers, SPRs.
 *
 * The game is two halves in one GcnwModule:
 *  - the HLE module (Runtime/tools/recomp/gcn_hle_build.py): com.recomp.gcn's SDK replacement,
 *    a wasm2c module whose descriptor GcnGuestHost / the runner run as usual. Its
 *    gcn_game_entry loads the DOL and boots; its main() is an import, the recompiled main.
 *  - the recompiled game (GcnRecomp): f_ADDR functions on a gcnr_ctx, calling hle_NAME
 *    wrappers (gen_hle_glue.py) for the SDK functions the runtime supplies.
 *
 * Stacks: one guest stack per guest thread, shared by both halves. A wrapper puts the HLE's
 * shadow stack just below the calling code's r1; a call back into recompiled code (main,
 * callbacks, thread entries) gets an r1 just below the HLE's frames.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcn_recomp.h"
#include "gcn_platform.h"
#include "gcnw.h"
#include "gcnw_module.h"

struct w2c_env;
uint32_t w2c_env_0x5F_gcn_vload8(struct w2c_env* env, uint32_t addr);
uint32_t w2c_env_0x5F_gcn_vload16(struct w2c_env* env, uint32_t addr);
uint32_t w2c_env_0x5F_gcn_vload32(struct w2c_env* env, uint32_t addr);
void w2c_env_0x5F_gcn_vstore8(struct w2c_env* env, uint32_t addr, uint32_t v);
void w2c_env_0x5F_gcn_vstore16(struct w2c_env* env, uint32_t addr, uint32_t v);
void w2c_env_0x5F_gcn_vstore32(struct w2c_env* env, uint32_t addr, uint32_t v);
uint64_t w2c_env_gcn_host_ticks(struct w2c_env* env);

/* the generated tables (GcnRecomp's functable.c and game.c, gen_hle_glue.py); a Live build
 * (GCNR_LIVE, live/recomp_live.cpp) fills them when the game first needs them */
typedef struct gcnr_function
{
    uint32_t addr;
    gcnr_func fn;
} gcnr_function;
#if GCNR_LIVE
extern const gcnr_function* gcnr_functions;
extern uint32_t gcnr_function_count;
extern uint32_t gcnr_r2, gcnr_r13, gcnr_ctors;
void gcnr_live_ensure(void);
#define LIVE_ENSURE() gcnr_live_ensure()
#else
extern const gcnr_function gcnr_functions[];
extern const uint32_t gcnr_function_count;
extern const uint32_t gcnr_r2, gcnr_r13, gcnr_ctors;
#define LIVE_ENSURE() ((void)0)
#endif
const GcnwModule* gcnr_module(void);

uint32_t gcnr_gqr[8];
static uint32_t sSpr[1024];
static void watch_init(void);

/* same folding of guest addresses as the wasm2c side (gcnw.h) */
typedef char gcnr_offset_check[(GCNR_LC_OFFSET == GCNW_LC_OFFSET && GCNR_RAM_MASK == GCNW_RAM_MASK &&
                                GCNR_LC_MASK == GCNW_LC_MASK) ? 1 : -1];

static uint8_t* guest_mem(void)
{
    static uint8_t* sMem;
    if (!sMem)
    {
        sMem = gcnr_module()->memory()->data;
        watch_init();
    }
    return sMem;
}

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

static void logf_(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gcnp_log(buf);
}

/* ---- function lookup ---------------------------------------------------------------------- */
#define CACHE_SIZE 4096u
static struct
{
    uint32_t addr;
    gcnr_func fn;
} sCache[CACHE_SIZE];

static const gcnr_function* find(uint32_t addr)
{
    uint32_t lo = 0, hi = gcnr_function_count;
    while (lo < hi)
    {
        const uint32_t mid = (lo + hi) / 2;
        if (gcnr_functions[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < gcnr_function_count ? &gcnr_functions[lo] : NULL;
}

gcnr_func gcnr_lookup(uint32_t addr)
{
    const uint32_t slot = (addr >> 2) & (CACHE_SIZE - 1);
    const gcnr_function* f;
    addr = 0x80000000u | (addr & 0x01FFFFFFu); /* uncached / physical views of code: the cached address */
    if (sCache[slot].addr == addr && sCache[slot].fn)
    {
        return sCache[slot].fn;
    }
    LIVE_ENSURE();
    f = find(addr);
    if (!f || f->addr != addr)
    {
        const gcnr_function* before = f && f > gcnr_functions ? f - 1 : f;
        fatal("recomp: call to %08X, not the start of a recompiled function (nearest %08X)", addr,
              before ? before->addr : 0);
    }
    sCache[slot].addr = addr;
    sCache[slot].fn = f->fn;
    return f->fn;
}

/* the table changed (Live builds: a module linked or unlinked) */
void gcnr_lookup_reset(void)
{
    memset(sCache, 0, sizeof(sCache));
}

gcnr_func gcnr_lookup_from(uint32_t addr, const gcnr_ctx* c)
{
    const uint32_t a = 0x80000000u | (addr & 0x01FFFFFFu);
    const uint32_t slot = (a >> 2) & (CACHE_SIZE - 1);
    const gcnr_function* f;
    if (sCache[slot].addr == a && sCache[slot].fn)
    {
        return sCache[slot].fn;
    }
    LIVE_ENSURE();
    f = find(a);
    if (!f || f->addr != a)
    {
        logf_("recomp: indirect call to %08X from lr %08X: r3 %08X r4 %08X r5 %08X r28 %08X r29 %08X r30 %08X r31 %08X",
              addr, c->lr, c->r[3], c->r[4], c->r[5], c->r[28], c->r[29], c->r[30], c->r[31]);
    }
    return gcnr_lookup(addr);
}

/* ---- store watchpoint (GCNR_WATCH builds, GCNR_WATCH=<hex address>) ------------------------- */
uint32_t gcnr_watch_addr = 0xFFFFFFFFu;
uint32_t gcnr_watch_pc; /* Live builds: the guest instruction gcnl_exec runs */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

void gcnr_watch_hit(uint32_t addr, uint64_t value, int bytes)
{
    static int sCount;
    if (sCount++ > 64) return;
    logf_("recomp watch: %d-byte store to %08X: %llX (guest pc %08X)", bytes, addr, (unsigned long long)value,
          gcnr_watch_pc);
#if defined(_WIN32)
    {
        void* frames[12];
        const USHORT n = CaptureStackBackTrace(1, 12, frames, NULL);
        HANDLE proc = GetCurrentProcess();
        static int sInit;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        USHORT i;
        if (!sInit) { SymInitialize(proc, NULL, TRUE); sInit = 1; }
        for (i = 0; i < n; i++)
        {
            DWORD64 disp = 0;
            memset(buf, 0, sizeof(buf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, (DWORD64)frames[i], &disp, sym)) logf_("    %s+0x%llx", sym->Name, (unsigned long long)disp);
        }
    }
#endif
}

static void watch_init(void)
{
    const char* env = getenv("GCNR_WATCH");
    if (env) gcnr_watch_addr = (uint32_t)strtoul(env, NULL, 16) & 0x01FFFFFCu;
}

/* ---- calls from the HLE into recompiled code ---------------------------------------------- */
void gcnr_init_ctx(gcnr_ctx* c)
{
    LIVE_ENSURE(); /* r2 / r13 come from the symbols */
    const uint32_t hleSp = *gcnr_module()->stack_pointer();
    memset(c, 0, sizeof(*c));
    c->r[1] = (hleSp - 64u) & ~15u; /* below the HLE's frames */
    c->r[2] = gcnr_r2;
    c->r[13] = gcnr_r13;
    c->msr = 0x9032u; /* EE | ME | IR | DR | RI, as a running game sees it */
}

/* calls recompiled code at addr with the registers in c (gcnr_init_ctx, then the arguments);
 * the results stay in c (r3, r3:r4, f1) */
uint32_t gcnr_call_ctx(gcnr_ctx* c, uint32_t addr)
{
    uint8_t* mem = guest_mem();
    /* the back chain and LR slot of the frame the callee hangs below */
    gcnr_sw(mem, c->r[1], 0);
    gcnr_sw(mem, c->r[1] + 4, 0);
    c->lr = 0;
    gcnr_lookup(addr)(mem, c);
    return c->r[3];
}

uint32_t gcnr_call_guest(uint32_t addr, int n, const uint32_t* args)
{
    gcnr_ctx c;
    int i;
    gcnr_init_ctx(&c);
    if (n > 8)
    {
        c.r[1] -= (uint32_t)((n - 8) * 4 + 16) & ~15u;
        for (i = 8; i < n; i++)
        {
            gcnr_sw(guest_mem(), c.r[1] + 8 + 4 * (uint32_t)(i - 8), args[i]);
        }
    }
    for (i = 0; i < n && i < 8; i++)
    {
        c.r[3 + i] = args[i];
    }
    return gcnr_call_ctx(&c, addr);
}

/* The static constructors (.ctors, from _ctors to the null entry), as __start's __init_cpp runs
 * them: MWCC fills some tables there (e.g. the C library's trig constants). The recomp build
 * skips __start (the HLE boots), so the main import runs them first (gen_hle_glue.py). */
void gcnr_run_ctors(void)
{
    static int sDone;
    uint32_t p;
    LIVE_ENSURE();
    if (sDone || !gcnr_ctors) return;
    sDone = 1;
    for (p = gcnr_ctors;; p += 4)
    {
        const uint32_t fn = gcnr_lw(guest_mem(), p);
        uint32_t none = 0;
        if (!fn) break;
        gcnr_call_guest(fn, 0, &none);
    }
}

/* vsprintf(buf, fmt, ap) with the HLE's va_list (a big-endian buffer laid out as an AAPCS va_list
 * walks it) as a PowerPC va_list whose register save area is used up: every va_arg then reads
 * the overflow area, which is laid out the same way (ints 4 bytes, doubles 8-aligned). */
uint32_t gcnr_call_vsprintf(uint32_t addr, uint32_t buf, uint32_t fmt, uint32_t ap)
{
    gcnr_ctx c;
    uint8_t* mem = guest_mem();
    uint32_t va;
    gcnr_init_ctx(&c);
    va = c.r[1] - 16u;
    gcnr_sb(mem, va + 0, 8); /* gpr */
    gcnr_sb(mem, va + 1, 8); /* fpr */
    gcnr_sh(mem, va + 2, 0);
    gcnr_sw(mem, va + 4, ap);  /* overflow_arg_area */
    gcnr_sw(mem, va + 8, 0);   /* reg_save_area */
    c.r[1] = (va - 64u) & ~15u;
    c.r[3] = buf;
    c.r[4] = fmt;
    c.r[5] = va;
    return gcnr_call_ctx(&c, addr);
}

/* A function pointer the HLE calls (thread entries, DVD / VI / alarm / interrupt callbacks) holds
 * a PowerPC code address, not a wasm table slot: gcnw_ops.h (GCNW_RECOMP) sends such calls here.
 * The arguments are integers (the SDK's callbacks take ints and pointers); unused ones are
 * whatever the caller left in the registers, as on the console. */
static uint32_t sCallbackTarget;

static uint64_t callback_trampoline(void* instance, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                    uint32_t a5, uint32_t a6, uint32_t a7)
{
    const uint32_t target = sCallbackTarget;
    gcnr_ctx c;
    (void)instance;
    gcnr_init_ctx(&c);
    c.r[3] = a0;
    c.r[4] = a1;
    c.r[5] = a2;
    c.r[6] = a3;
    c.r[7] = a4;
    c.r[8] = a5;
    c.r[9] = a6;
    c.r[10] = a7;
    return gcnr_call_ctx(&c, target);
}

void* gcnw_guest_callback(uint32_t address)
{
    if ((address >> 24) != 0x80u && (address >> 24) != 0x81u)
    {
        fatal("recomp: call through a function pointer to %08X (not code)", address);
    }
    sCallbackTarget = address;
    return (void*)callback_trampoline;
}

/* ---- the variadic buffer the HLE's OSReport & co take ------------------------------------- */
/* Lays out the arguments that follow `fixed` integer arguments, as the format string says, the
 * way gcn_ir.py lays out a variadic call (ints 4 bytes, doubles 8-aligned), big-endian, below
 * `top`. PowerPC passes them in r3.. / f1.. and then in the caller's parameter area at 8(r1). */
uint32_t gcnr_va_build(uint8_t* mem, gcnr_ctx* c, int fixed, uint32_t top)
{
    enum { MAX = 32 };
    int kind[MAX]; /* 0 int, 1 double, 2 long long */
    uint64_t val[MAX];
    int n = 0, gpr = 3 + fixed, fpr = 1;
    uint32_t over = c->r[1] + 8, fmt = c->r[3 + fixed - 1], size = 0, off, buf, i;
    char ch;

    while (n < MAX && (ch = (char)gcnr_lbz(mem, fmt++)) != 0)
    {
        int ll = 0, isd = 0;
        if (ch != '%') continue;
        ch = (char)gcnr_lbz(mem, fmt++);
        if (ch == '%') continue;
        while (ch && strchr("-+ #0", ch)) ch = (char)gcnr_lbz(mem, fmt++);
        for (;;) /* width, precision; '*' takes an int */
        {
            if (ch == '*')
            {
                kind[n] = 0;
                val[n++] = gpr <= 10 ? c->r[gpr++] : (over += 4, gcnr_lw(mem, over - 4));
                ch = (char)gcnr_lbz(mem, fmt++);
            }
            else if ((ch >= '0' && ch <= '9') || ch == '.') ch = (char)gcnr_lbz(mem, fmt++);
            else break;
            if (n >= MAX) break;
        }
        while (ch && strchr("hlLqjzt", ch))
        {
            if (ch == 'l' || ch == 'q' || ch == 'L') ll++;
            ch = (char)gcnr_lbz(mem, fmt++);
        }
        if (!ch || n >= MAX) break;
        isd = strchr("fFeEgGaA", ch) != NULL;
        if (isd)
        {
            double d;
            if (fpr <= 8) d = c->f[fpr++].ps0;
            else
            {
                over = (over + 7) & ~7u;
                d = gcnr_f64(gcnr_ld(mem, over));
                over += 8;
            }
            kind[n] = 1;
            memcpy(&val[n++], &d, 8);
        }
        else if (ll >= 2 && strchr("diouxX", ch))
        {
            if (gpr % 2 == 0) gpr++;
            if (gpr + 1 <= 10)
            {
                val[n] = (uint64_t)c->r[gpr] << 32 | c->r[gpr + 1];
                gpr += 2;
            }
            else
            {
                over = (over + 7) & ~7u;
                val[n] = gcnr_ld(mem, over);
                over += 8;
            }
            kind[n++] = 2;
        }
        else if (strchr("diouxXcspn", ch))
        {
            kind[n] = 0;
            val[n++] = gpr <= 10 ? c->r[gpr++] : (over += 4, gcnr_lw(mem, over - 4));
        }
        else break;
    }
    for (i = 0; i < (uint32_t)n; i++)
    {
        if (kind[i]) size = ((size + 7) & ~7u) + 8;
        else size += 4;
    }
    buf = (top - size - 8u) & ~7u;
    for (i = 0, off = 0; i < (uint32_t)n; i++)
    {
        if (kind[i])
        {
            off = (off + 7) & ~7u;
            gcnr_sd(mem, buf + off, val[i]);
            off += 8;
        }
        else
        {
            gcnr_sw(mem, buf + off, (uint32_t)val[i]);
            off += 4;
        }
    }
    return buf;
}

/* ---- tracing (GCNR_TRACE=N: the first N calls into the HLE) ------------------------------- */
int gcnr_trace_left = -1; /* -1: not read from the environment yet */

static void trace_string(uint8_t* mem, uint32_t addr, char* out, size_t cap)
{
    size_t i = 0;
    if ((addr >> 24) != 0x80 && (addr >> 24) != 0x81)
    {
        out[0] = 0;
        return;
    }
    for (; i + 1 < cap; i++)
    {
        const char ch = (char)gcnr_lbz(mem, addr + (uint32_t)i);
        if (!ch) break;
        out[i] = (ch >= 32 && ch < 127) ? ch : '?';
    }
    out[i] = 0;
}

void gcnr_trace_hle(const char* name, uint8_t* mem, gcnr_ctx* c)
{
    char s3[64], s4[64];
    if (gcnr_trace_left < 0)
    {
        const char* env = getenv("GCNR_TRACE");
        gcnr_trace_left = env ? atoi(env) : 0;
        if (!gcnr_trace_left) return;
    }
    gcnr_trace_left--;
    trace_string(mem, c->r[3], s3, sizeof(s3));
    trace_string(mem, c->r[4], s4, sizeof(s4));
    logf_("hle %-24s lr %08X  r3 %08X r4 %08X r5 %08X r6 %08X  \"%s\" \"%s\"", name, c->lr, c->r[3], c->r[4], c->r[5],
          c->r[6], s3, s4);
}

void gcnr_trace_func(uint32_t addr, uint8_t* mem, const gcnr_ctx* c)
{
    static int sCount;
    (void)mem;
    if (sCount++ > 400) return;
    logf_("enter %08X lr %08X r1 %08X | r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X | f1 %g", addr, c->lr, c->r[1],
          c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->f[1].ps0);
}

void gcnr_sp_changed(uint32_t at, uint32_t callee, uint32_t before, uint32_t after)
{
    static int sCount;
    if (sCount++ < 20)
    {
        logf_("recomp: the call at %08X to %08X returned with r1 %08X (was %08X)", at, callee, after, before);
    }
}

/* ---- generated-code hooks ------------------------------------------------------------------ */
void gcnr_loop_poll(uint8_t* mem, gcnr_ctx* c)
{
    (void)mem;
    c->loop = 0;
    gcnr_module()->spin();
}

uint32_t gcnr_mmio_read(uint32_t addr, int bytes)
{
    return bytes == 1 ? w2c_env_0x5F_gcn_vload8(NULL, addr)
         : bytes == 2 ? w2c_env_0x5F_gcn_vload16(NULL, addr)
                      : w2c_env_0x5F_gcn_vload32(NULL, addr);
}

void gcnr_mmio_write(uint32_t addr, uint32_t value, int bytes)
{
    if (bytes == 1) w2c_env_0x5F_gcn_vstore8(NULL, addr, value & 0xFFu);
    else if (bytes == 2) w2c_env_0x5F_gcn_vstore16(NULL, addr, value & 0xFFFFu);
    else w2c_env_0x5F_gcn_vstore32(NULL, addr, value);
}

void gcnr_psq_st_mmio(gcnr_ctx* c, int fs, uint32_t addr, int w, int i)
{
    /* quantise into a scratch line, then write it to the hardware in its element size */
    uint8_t line[8];
    const uint32_t g = gcnr_gqr[i];
    const uint32_t type = g & 7, size = gcnr_qsize(type), n = w ? 1u : 2u;
    uint8_t* mem = guest_mem();
    const uint32_t scratch = 0x817FFFF0u; /* never used by the game (just below the HLE) */
    uint32_t k;
    memcpy(line, GCNR_PTR(mem, scratch), 8);
    gcnr_psq_st(mem, c, fs, scratch, w, i);
    for (k = 0; k < n; k++)
    {
        const uint32_t a = scratch + k * size;
        gcnr_mmio_write(addr, size == 1 ? gcnr_lbz(mem, a) : size == 2 ? gcnr_lhz(mem, a) : gcnr_lw(mem, a), (int)size);
    }
    memcpy(GCNR_PTR(mem, scratch), line, 8);
}

void gcnr_checked_access(uint32_t addr, int write)
{
    static int said;
    if (said < 32)
    {
        said++;
        logf_("recomp: unproven %s of hardware register %08X (GCNR_CHECKED)", write ? "write" : "read", addr);
    }
}

uint32_t gcnr_mfspr(gcnr_ctx* c, uint32_t spr)
{
    (void)c;
    if (spr == 22) return (uint32_t)(0x7FFFFFFF - (w2c_env_gcn_host_ticks(NULL) & 0x7FFFFFFF)); /* DEC */
    if (spr == 287) return 0x00083214u; /* PVR: Gekko */
    return sSpr[spr & 1023];
}

void gcnr_mtspr(gcnr_ctx* c, uint32_t spr, uint32_t value)
{
    (void)c;
    sSpr[spr & 1023] = value;
}

uint64_t gcnr_timebase(void)
{
    return w2c_env_gcn_host_ticks(NULL);
}

void gcnr_syscall(uint8_t* mem, gcnr_ctx* c)
{
    (void)mem;
    (void)c;
    /* sc is how the SDK flushes the instruction cache on some paths: nothing to do here */
}

void gcnr_trap(gcnr_ctx* c, uint32_t addr)
{
    fatal("recomp: trap instruction at %08X (lr %08X, r3 %08X)", addr, c->lr, c->r[3]);
}

void gcnr_unhandled(gcnr_ctx* c, uint32_t addr, const char* what)
{
    fatal("recomp: %s at %08X (lr %08X, r1 %08X)", what, addr, c->lr, c->r[1]);
}
