/*
 * wasm2c guest backend: the registry of translated games, the running instance, and the
 * guest's imports (Runtime/include/gcn_guest.h): memory-mapped hardware, threads as host
 * coroutines, and the host services behind them (gcn_platform.h).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcnw.h"
#include "gcnw_module.h"
#include "gcn_gpu.h"
#include "gcn_platform.h"
#include "gcnw_backend.h"
#include "gcn_thp.h"
#include "gcn_overlay.h"

typedef uint32_t u32;
typedef uint64_t u64;

/* ---- where the game's time goes in host calls (consoles: diagnostics) ---- */
enum { PROF_DISC, PROF_ARAM, PROF_AUDIO, PROF_DCACHE, PROF_VIO, PROF_IRQ, PROF_THP, PROF_TICKS, PROF_N };
static const char *const kProfNames[PROF_N] = {"disc", "aram", "audio", "dcache", "volatile", "irq", "thp", "time"};
static u64 sProfTicks[PROF_N];
static u32 sProfCalls[PROF_N];
static u32 sProfSpins, sProfSwitches;
#if defined(GEKKO)
#include <ogc/lwp_watchdog.h>
#define PROF_BEGIN() const u64 prof_t0_ = gettime()
#define PROF_END(c) (sProfTicks[c] += gettime() - prof_t0_, sProfCalls[c]++)
#else
#define PROF_BEGIN() (void)0
#define PROF_END(c) (sProfCalls[c]++)
#endif

/* "disc 3.1 ms (12), ..." per frame over `frames`, for the categories that took time; resets */
void gcnw_take_import_profile(char *out, size_t cap, u32 frames)
{
    size_t len = 0;
    int i;

    if (!frames) frames = 1;
    out[0] = 0;
    for (i = 0; i < PROF_N && len + 48 < cap; i++)
    {
#if defined(GEKKO)
        const double ms = ticks_to_microsecs(sProfTicks[i]) / 1000.0 / frames;
#else
        const double ms = 0.0;
#endif
        if (sProfCalls[i] == 0) continue;
        len += (size_t)snprintf(out + len, cap - len, "%s%s %.1f ms (%u)", len ? ", " : "", kProfNames[i], ms,
                                (unsigned)(sProfCalls[i] / frames));
        sProfTicks[i] = 0;
        sProfCalls[i] = 0;
    }
    if (len + 48 < cap)
        snprintf(out + len, cap - len, "%sswitches %u, spins %u", len ? ", " : "", (unsigned)(sProfSwitches / frames),
                 (unsigned)(sProfSpins / frames));
    sProfSwitches = sProfSpins = 0;
}

void gcnw_rt_reset(void);

/* imports come from the "env" module; nothing to keep per instance */
struct w2c_env
{
    int unused;
};

/* ---- logging (also used by gcnw_rt.c) ----------------------------------------------- */
void host_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gcnp_log(buf);
}

void host_crashed(void)
{
    gcnp_crashed();
    abort();
}

/* ---- registry ------------------------------------------------------------------------- */
#define MAX_MODULES 8
static const GcnwModule *sModules[MAX_MODULES];
static int sModuleCount;

void gcnw_register_module(const GcnwModule *module)
{
    if (sModuleCount < MAX_MODULES) sModules[sModuleCount++] = module;
}

const GcnwModule *gcnw_find_module(const char *package)
{
    int i;

    for (i = 0; i < sModuleCount; i++)
    {
        if (package == NULL || package[0] == 0 || strcmp(sModules[i]->package, package) == 0) return sModules[i];
    }
    return NULL;
}

/* ---- the running instance ----------------------------------------------------------------- */
static const GcnwModule *sModule;
static struct w2c_env sEnv;
static uint8_t *sMem;
static uint8_t *sAram;
static u32 sRetraces;
static u32 sSpins; /* volatile RAM loads since the game last waited properly */

uint8_t *gcnw_memory(void) { return sMem; }

static uint8_t *guest_ptr(u32 addr, u32 len)
{
    u32 off = GCNW_OFFSET(addr);

    if ((u64)off + len > GCNW_MEM_BYTES)
    {
        host_log("guest buffer %08X+%X runs past guest memory", (unsigned)addr, (unsigned)len);
        host_crashed();
    }
    return sMem + off;
}

uint32_t gcnw_read32(uint32_t addr)
{
    const uint8_t *p = guest_ptr(addr, 4);
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

void gcnw_write32(uint32_t addr, uint32_t v)
{
    uint8_t *p = guest_ptr(addr, 4);
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void gcnw_report_trap(const char *what)
{
    host_log("CRASH: wasm trap: %s", what);
    host_crashed();
}

static void bridge_reset(void);

int gcnw_instantiate(const GcnwModule *module)
{
    if (sModule) gcnw_free();
    bridge_reset();
    wasm_rt_init();
    gcnw_rt_reset();
    module->instantiate(&sEnv);
    sModule = module;
    sMem = module->memory()->data;
    if (!sAram) sAram = (uint8_t *)gcnw_big_alloc(0x01000000);
    gcn_gpu_init(sMem);
    sRetraces = 0;
    return sMem != NULL && sAram != NULL;
}

void gcnw_free(void)
{
    if (sModule) sModule->free();
    sModule = NULL;
    sMem = NULL;
    bridge_reset();
}

/* ---- threads -------------------------------------------------------------------------------- */
#define MAX_CTX 64
typedef struct
{
    GcnpCoro *coro;
    u32 fn, arg, stack_top;
    u32 sp;      /* guest stack pointer while switched out */
    int used;
} Ctx;

static Ctx sCtx[MAX_CTX];
static int sCurrentCtx;

static void ctx_main(void *p)
{
    Ctx *c = (Ctx *)p;

    *sModule->stack_pointer() = c->stack_top & ~15u;
    sModule->thread_entry(c->fn, c->arg);
    host_log("guest thread returned to the host");
    host_crashed();
}

void gcnw_run(void)
{
    memset(sCtx, 0, sizeof(sCtx));
    sCtx[0].coro = gcnp_coro_current();
    sCtx[0].used = 1;
    sCurrentCtx = 0;
    sModule->run();
}

u32 w2c_env_gcn_host_ctx_create(struct w2c_env *env, u32 fn, u32 arg, u32 stack_top)
{
    int i;

    for (i = 1; i < MAX_CTX; i++)
    {
        if (!sCtx[i].used) break;
    }
    if (i == MAX_CTX)
    {
        host_log("too many guest threads");
        host_crashed();
    }
    sCtx[i].used = 1;
    sCtx[i].fn = fn;
    sCtx[i].arg = arg;
    sCtx[i].stack_top = stack_top;
#if defined(GEKKO) || defined(__3DS__)
    sCtx[i].coro = gcnp_coro_create(ctx_main, &sCtx[i], 512u << 10); /* memory is short there */
    host_log("gcn: guest thread %d (%s)", i, sCtx[i].coro ? "ok" : "NO MEMORY for its stack");
    if (!sCtx[i].coro) host_crashed();
#else
    sCtx[i].coro = gcnp_coro_create(ctx_main, &sCtx[i], 1u << 20);
#endif
    return (u32)i;
}

void w2c_env_gcn_host_ctx_switch(struct w2c_env *env, u32 ctx)
{
    sProfSwitches++;
    Ctx *from = &sCtx[sCurrentCtx], *to;

    if (ctx >= MAX_CTX || !sCtx[ctx].used)
    {
        host_log("switch to a dead guest thread (%u)", (unsigned)ctx);
        host_crashed();
    }
    if ((int)ctx == sCurrentCtx) return;
    to = &sCtx[ctx];
    sSpins = 0;
    from->sp = *sModule->stack_pointer();
    sCurrentCtx = (int)ctx;
    gcnp_coro_switch(to->coro);
    /* resumed: restore this context's guest stack */
    *sModule->stack_pointer() = from->sp;
}

void w2c_env_gcn_host_ctx_destroy(struct w2c_env *env, u32 ctx)
{
    if (ctx == 0 || ctx >= MAX_CTX || (int)ctx == sCurrentCtx) return;
    if (sCtx[ctx].coro) gcnp_coro_destroy(sCtx[ctx].coro);
    memset(&sCtx[ctx], 0, sizeof(Ctx));
}

/* ---- memory-mapped hardware and volatile accesses ---------------------------------------- */
static int is_mmio(u32 addr) { return (addr >> 24) == 0xCC; }

static u32 mmio_read(u32 addr, int bytes)
{
    if (addr >= 0xCC000000 && addr < 0xCC004000) return gcn_gpu_reg_read(addr, bytes);
    return 0;
}

static void mmio_write(u32 addr, u32 v, int bytes)
{
    if ((addr & 0xFFFFFFE0u) == 0xCC008000u)
    {
        gcn_gpu_fifo_write(v, bytes);
#if defined(GEKKO)
        /* a finished picture in the GPU's frame buffer: the host draws over it and shows it
         * (GcnGuestHost.cpp) before the game draws on */
        if (gcn_gpu_take_frame_done()) gcnp_frame_done();
#endif
        return;
    }
    if (addr >= 0xCC000000 && addr < 0xCC004000) gcn_gpu_reg_write(addr, v, bytes);
}

/* A loop that keeps reading volatile RAM waits for an interrupt: let one happen. */
static u32 sSpinAddr;

static void spun(u32 addr)
{
    if (addr != sSpinAddr)
    {
        sSpinAddr = addr;
        sSpins = 0;
        return;
    }
    if (++sSpins >= 2048)
    {
        sSpins = 0;
        sProfSpins++;
        sModule->spin();
    }
}

u32 w2c_env_0x5F_gcn_vload8(struct w2c_env *env, u32 addr)
{
    u32 v;
    PROF_BEGIN();
    if (is_mmio(addr)) v = mmio_read(addr, 1) & 0xFF;
    else
    {
        spun(addr);
        v = *guest_ptr(addr, 1);
    }
    PROF_END(PROF_VIO);
    return v;
}

u32 w2c_env_0x5F_gcn_vload16(struct w2c_env *env, u32 addr)
{
    const uint8_t *p;
    u32 v;
    PROF_BEGIN();
    if (is_mmio(addr)) v = mmio_read(addr, 2) & 0xFFFF;
    else
    {
        spun(addr);
        p = guest_ptr(addr, 2);
        v = ((u32)p[0] << 8) | p[1];
    }
    PROF_END(PROF_VIO);
    return v;
}

/* the GX write-gather pipe (stores to 0xCC008000, gcn_ir.wgpipe_store) */
void gcnw_wgpipe_slow(u32 v, int bytes)
{
    gcn_gpu_fifo_write(v, bytes);
#if defined(GEKKO)
    if (gcn_gpu_take_frame_done()) gcnp_frame_done();
#endif
}

void w2c_env_0x5F_gcn_wgpipe8(struct w2c_env *env, u32 v)
{
    (void)env;
    mmio_write(0xCC008000u, v & 0xFF, 1);
}

void w2c_env_0x5F_gcn_wgpipe16(struct w2c_env *env, u32 v)
{
    (void)env;
    mmio_write(0xCC008000u, v & 0xFFFF, 2);
}

void w2c_env_0x5F_gcn_wgpipe32(struct w2c_env *env, u32 v)
{
    (void)env;
    mmio_write(0xCC008000u, v, 4);
}

/* byte swaps: the generated code uses gcnw_ops.h's inline versions; these back any other use */
u32 w2c_env_0x5F_gcn_bswap16(struct w2c_env *env, u32 x)
{
    (void)env;
    return ((x >> 8) & 0xFFu) | ((x & 0xFFu) << 8);
}

u32 w2c_env_0x5F_gcn_bswap32(struct w2c_env *env, u32 x)
{
    (void)env;
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x & 0xFF00u) << 8) | (x << 24);
}

u64 w2c_env_0x5F_gcn_bswap64(struct w2c_env *env, u64 x)
{
    (void)env;
    return ((u64)w2c_env_0x5F_gcn_bswap32(env, (u32)x) << 32) | w2c_env_0x5F_gcn_bswap32(env, (u32)(x >> 32));
}

u32 w2c_env_0x5F_gcn_vload32(struct w2c_env *env, u32 addr)
{
    u32 v;
    PROF_BEGIN();
    if (is_mmio(addr)) v = mmio_read(addr, 4);
    else
    {
        spun(addr);
        v = gcnw_read32(addr);
    }
    PROF_END(PROF_VIO);
    return v;
}

void w2c_env_0x5F_gcn_vstore8(struct w2c_env *env, u32 addr, u32 v)
{
    if (is_mmio(addr))
    {
        mmio_write(addr, v & 0xFF, 1);
        return;
    }
    *guest_ptr(addr, 1) = (uint8_t)v;
}

void w2c_env_0x5F_gcn_vstore16(struct w2c_env *env, u32 addr, u32 v)
{
    uint8_t *p;
    if (is_mmio(addr))
    {
        mmio_write(addr, v & 0xFFFF, 2);
        return;
    }
    p = guest_ptr(addr, 2);
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void w2c_env_0x5F_gcn_vstore32(struct w2c_env *env, u32 addr, u32 v)
{
#if defined(GEKKO)
    /* the pipe through a pointer (WriteMTXPS4x3(src, &GXWGFifo)...): vertex data inline */
    if (addr == 0xCC008000u && gcn_gpu_payload >= 4)
    {
        *(volatile u32 *)0xCC008000 = v;
        gcn_gpu_payload -= 4;
        return;
    }
#endif
    if (addr == 0xCC008000u && gcn_gpu_dl_ptr && gcn_gpu_dl_end - gcn_gpu_dl_ptr >= 4)
    {
        /* into a display list being built (big-endian, as the hardware writes it) */
        gcn_gpu_dl_ptr[0] = (uint8_t)(v >> 24);
        gcn_gpu_dl_ptr[1] = (uint8_t)(v >> 16);
        gcn_gpu_dl_ptr[2] = (uint8_t)(v >> 8);
        gcn_gpu_dl_ptr[3] = (uint8_t)v;
        gcn_gpu_dl_ptr += 4;
        return;
    }
    if (is_mmio(addr))
    {
        mmio_write(addr, v, 4);
        return;
    }
    gcnw_write32(addr, v);
}

/* ---- host services ------------------------------------------------------------------------- */
void w2c_env_gcn_host_log(struct w2c_env *env, u32 text)
{
    char buf[1024];
    u32 i;

    for (i = 0; i + 1 < sizeof(buf); i++)
    {
        char c = (char)*guest_ptr(text + i, 1);
        if (!c) break;
        buf[i] = c;
    }
    buf[i] = 0;
    while (i && (buf[i - 1] == '\n' || buf[i - 1] == '\r')) buf[--i] = 0;
    gcnp_log(buf);
    {
        /* debugging: GCN_BREAK_ON_LOG=text stops (with a backtrace) at the first such line */
        static const char *brk = (const char *)-1;
        if (brk == (const char *)-1) brk = getenv("GCN_BREAK_ON_LOG");
        if (brk && *brk && strstr(buf, brk))
        {
            host_log("CRASH: GCN_BREAK_ON_LOG matched");
            host_crashed();
        }
    }
}

void w2c_env_gcn_host_fatal(struct w2c_env *env, u32 text)
{
    w2c_env_gcn_host_log(env, text);
    host_log("CRASH: the game stopped (see above)");
    host_crashed();
}

u64 w2c_env_gcn_host_ticks(struct w2c_env *env)
{
    /* the time base runs at a quarter of the 162 MHz bus clock */
    u64 us;
    PROF_BEGIN();
    us = gcnp_time_us();
    PROF_END(PROF_TICKS);
    return us * 405 / 10;
}

u32 w2c_env_gcn_host_disc_read(struct w2c_env *env, u32 dst, u32 offset, u32 size)
{
    /* the drive DMAs into memory: written behind the CPU cache, so the GPU sees it */
    uint8_t *p = guest_ptr(dst, size);
    u32 got;
    PROF_BEGIN();
    got = gcnp_disc_read(p, offset, size);
    gcnw_dcache_flush(p, got);
    PROF_END(PROF_DISC);
    return got;
}

u32 w2c_env_gcn_host_disc_size(struct w2c_env *env)
{
    return (u32)gcnp_disc_size();
}

u32 w2c_env_gcn_host_retrace(struct w2c_env *env)
{
    u32 periods;

    gcn_gpu_retrace();
    periods = gcnp_retrace();
    sSpins = 0;
    sRetraces += periods;
    return periods;
}

u32 gcnw_retrace_count(void) { return sRetraces; }

u32 w2c_env_gcn_host_pad(struct w2c_env *env, u32 port, u32 status)
{
    GcnPad pad;
    uint8_t *p = guest_ptr(status, 12);

    memset(&pad, 0, sizeof(pad));
    if (!gcnp_pad((int)port, &pad) || !pad.connected) return 0;
    p[0] = (uint8_t)(pad.buttons >> 8);
    p[1] = (uint8_t)pad.buttons;
    p[2] = (uint8_t)pad.stick_x;
    p[3] = (uint8_t)pad.stick_y;
    p[4] = (uint8_t)pad.substick_x;
    p[5] = (uint8_t)pad.substick_y;
    p[6] = pad.trigger_l;
    p[7] = pad.trigger_r;
    p[8] = (pad.buttons & GCN_PAD_A) ? 0xFF : 0;
    p[9] = (pad.buttons & GCN_PAD_B) ? 0xFF : 0;
    p[10] = 0;
    return 1;
}

void w2c_env_gcn_host_overlay(struct w2c_env *env, u32 line, u32 text)
{
    char buf[64];
    u32 i, off = GCNW_OFFSET(text);

    (void)env;
    for (i = 0; i < sizeof(buf) - 1 && text && off + i < GCNW_MEM_BYTES && sMem[off + i]; i++) buf[i] = (char)sMem[off + i];
    buf[i] = 0;
    gcn_overlay_set((int)line, buf);
}

u32 w2c_env_gcn_host_thp_decode(struct w2c_env *env, u32 file, u32 y, u32 u, u32 v)
{
    /* what lies between each pointer and the end of guest memory bounds the decoder */
    u32 fo = GCNW_OFFSET(file), yo = GCNW_OFFSET(y), uo = GCNW_OFFSET(u), vo = GCNW_OFFSET(v), cap;

    if (fo >= GCNW_MEM_BYTES || yo >= GCNW_MEM_BYTES || uo >= GCNW_MEM_BYTES || vo >= GCNW_MEM_BYTES) return 25;
    cap = GCNW_MEM_BYTES - yo;
    if ((GCNW_MEM_BYTES - uo) * 4 < cap) cap = (GCNW_MEM_BYTES - uo) * 4;
    if ((GCNW_MEM_BYTES - vo) * 4 < cap) cap = (GCNW_MEM_BYTES - vo) * 4;
    (void)env;
    {
        PROF_BEGIN();
        size_t luma = 0;
        u32 r = (u32)gcn_thp_decode(sMem + fo, GCNW_MEM_BYTES - fo, sMem + yo, sMem + uo, sMem + vo, cap, &luma);
        /* the planes are textures: the SDK decoder wrote them through the locked cache's DMA
         * (only what this frame wrote: the bound above runs to the end of guest memory) */
        gcnw_dcache_flush(sMem + yo, luma);
        gcnw_dcache_flush(sMem + uo, luma / 4);
        gcnw_dcache_flush(sMem + vo, luma / 4);
        PROF_END(PROF_THP);
        return r;
    }
}

u32 w2c_env_gcn_host_card_size(struct w2c_env *env, u32 chan)
{
    return gcnp_card_size((int)chan);
}

/* the game's own cache maintenance (DCFlushRange / DCStoreRange / DCInvalidateRange / after
 * LCStoreData): real where the GPU reads main memory behind the CPU's caches. Invalidate is
 * done as a flush: the runtime's emulated DMA (disc, ARAM) copies with the CPU, so a real
 * invalidate would drop data it just wrote. */
void w2c_env_gcn_host_dcache(struct w2c_env *env, u32 op, u32 addr, u32 bytes)
{
    PROF_BEGIN();
    (void)env;
    (void)op;
    if (bytes > GCNW_MEM_BYTES) bytes = GCNW_MEM_BYTES;
    gcnw_dcache_flush(guest_ptr(addr, bytes), bytes);
    PROF_END(PROF_DCACHE);
}

u32 w2c_env_gcn_host_card_io(struct w2c_env *env, u32 chan, u32 buf, u32 offset, u32 size, u32 write)
{
    return gcnp_card_io((int)chan, guest_ptr(buf, size), offset, size, (int)write);
}

void w2c_env_gcn_host_rumble(struct w2c_env *env, u32 port, u32 on)
{
    gcnp_rumble((int)port, (int)on);
}

void w2c_env_gcn_host_audio(struct w2c_env *env, u32 pcm, u32 bytes, u32 rate)
{
    static int16_t buf[8192];
    const uint8_t *p = guest_ptr(pcm, bytes);
    u32 n = bytes / 2, i;
    PROF_BEGIN();

    if (n > 8192) n = 8192;
    for (i = 0; i < n; i++) buf[i] = (int16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
    gcnp_audio(buf, n / 2, rate);
    PROF_END(PROF_AUDIO);
}

void w2c_env_gcn_host_aram(struct w2c_env *env, u32 dir, u32 ram, u32 aram, u32 bytes)
{
    PROF_BEGIN();
    aram &= 0x00FFFFFF;
    if (aram + bytes > 0x01000000) bytes = 0x01000000 - aram;
    if (dir == 0)
    {
        memcpy(sAram + aram, guest_ptr(ram, bytes), bytes);
    }
    else
    {
        memcpy(guest_ptr(ram, bytes), sAram + aram, bytes);
        gcnw_dcache_flush(guest_ptr(ram, bytes), bytes); /* a DMA on the console */
    }
    PROF_END(PROF_ARAM);
}

uint8_t *gcnw_aram(void) { return sAram; }

u32 w2c_env_gcn_host_interrupts(struct w2c_env *env)
{
    u32 bits;
    static int trace = -1;
    PROF_BEGIN();
    bits = gcn_gpu_take_interrupts();
    PROF_END(PROF_IRQ);

    if (trace < 0) trace = getenv("GCN_TRACE_MMIO") != NULL;
    if (trace && bits) host_log("interrupts %08X", (unsigned)bits);
    return bits;
}

/* ---- script bridge (Runtime/guest/sdk/bridge.c, gcn_mod.h) -------------------------------
 * The game publishes its mods' variables and requests; the host queues requests the game
 * runs once per frame and collects the events mods emit. Host calls (gcnw_bridge_*) come
 * from the thread that drives the game while the game waits for a retrace. */
#define BRIDGE_MAX_VARS 256
#define BRIDGE_MAX_REQUESTS 128
#define BRIDGE_QUEUE 64
#define BRIDGE_RESULTS 256
#define BRIDGE_EVENTS 256

static GcnwBridgeVar sBridgeVars[BRIDGE_MAX_VARS];
static int sBridgeVarCount;
static GcnwBridgeRequest sBridgeRequests[BRIDGE_MAX_REQUESTS];
static int sBridgeRequestCount;
static struct
{
    int id;
    char name[64];
    int args[8];
    int nargs;
} sBridgeQueue[BRIDGE_QUEUE];
static int sBridgeQueueHead, sBridgeQueueCount, sBridgeNextId = 1;
static struct
{
    int id, result;
} sBridgeResults[BRIDGE_RESULTS];
static int sBridgeResultNext;
static GcnwBridgeEvent sBridgeEvents[BRIDGE_EVENTS];
static int sBridgeEventHead, sBridgeEventCount;

static void guest_string(u32 addr, char *out, size_t cap)
{
    size_t i;

    if (!cap) return;
    for (i = 0; addr && i + 1 < cap; i++)
    {
        char c = (char)*guest_ptr(addr + (u32)i, 1);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

void w2c_env_gcn_host_bridge_publish(struct w2c_env *env, u32 vars, u32 nvars, u32 reqs, u32 nreqs)
{
    u32 i;

    sBridgeVarCount = 0;
    for (i = 0; i < nvars && i < BRIDGE_MAX_VARS; i++)
    {
        GcnwBridgeVar *v = &sBridgeVars[sBridgeVarCount++];
        u32 e = vars + i * 24;
        guest_string(gcnw_read32(e), v->name, sizeof(v->name));
        v->addr = gcnw_read32(e + 4);
        v->type = (int)gcnw_read32(e + 8);
        v->count = (int)gcnw_read32(e + 12);
        v->stride = (int)gcnw_read32(e + 16);
        guest_string(gcnw_read32(e + 20), v->help, sizeof(v->help));
    }
    sBridgeRequestCount = 0;
    for (i = 0; i < nreqs && i < BRIDGE_MAX_REQUESTS; i++)
    {
        GcnwBridgeRequest *r = &sBridgeRequests[sBridgeRequestCount++];
        guest_string(gcnw_read32(reqs + i * 8), r->name, sizeof(r->name));
        guest_string(gcnw_read32(reqs + i * 8 + 4), r->help, sizeof(r->help));
    }
}

u32 w2c_env_gcn_host_bridge_poll(struct w2c_env *env, u32 name, u32 cap, u32 args, u32 max, u32 nargs)
{
    int i, n;
    size_t len;

    if (!sBridgeQueueCount) return 0;
    {
        int id = sBridgeQueue[sBridgeQueueHead].id;
        len = strlen(sBridgeQueue[sBridgeQueueHead].name);
        if (cap)
        {
            if (len >= cap) len = cap - 1;
            memcpy(guest_ptr(name, (u32)len + 1), sBridgeQueue[sBridgeQueueHead].name, len);
            *guest_ptr(name + (u32)len, 1) = 0;
        }
        n = sBridgeQueue[sBridgeQueueHead].nargs;
        if (n > (int)max) n = (int)max;
        for (i = 0; i < n; i++) gcnw_write32(args + (u32)i * 4, (u32)sBridgeQueue[sBridgeQueueHead].args[i]);
        gcnw_write32(nargs, (u32)n);
        sBridgeQueueHead = (sBridgeQueueHead + 1) % BRIDGE_QUEUE;
        sBridgeQueueCount--;
        return (u32)id;
    }
}

void w2c_env_gcn_host_bridge_done(struct w2c_env *env, u32 id, u32 result)
{
    sBridgeResults[sBridgeResultNext].id = (int)id;
    sBridgeResults[sBridgeResultNext].result = (int)result;
    sBridgeResultNext = (sBridgeResultNext + 1) % BRIDGE_RESULTS;
}

void w2c_env_gcn_host_bridge_emit(struct w2c_env *env, u32 name, u32 args, u32 nargs)
{
    GcnwBridgeEvent *e;
    u32 i;

    if (sBridgeEventCount == BRIDGE_EVENTS)
    {
        /* nobody reads them: drop the oldest */
        sBridgeEventHead = (sBridgeEventHead + 1) % BRIDGE_EVENTS;
        sBridgeEventCount--;
    }
    e = &sBridgeEvents[(sBridgeEventHead + sBridgeEventCount) % BRIDGE_EVENTS];
    sBridgeEventCount++;
    guest_string(name, e->name, sizeof(e->name));
    e->nargs = nargs > 8 ? 8 : (int)nargs;
    for (i = 0; i < (u32)e->nargs; i++) e->args[i] = (int)gcnw_read32(args + i * 4);
}

int gcnw_bridge_var_count(void) { return sBridgeVarCount; }
const GcnwBridgeVar *gcnw_bridge_var(int index)
{
    return index >= 0 && index < sBridgeVarCount ? &sBridgeVars[index] : NULL;
}
const GcnwBridgeVar *gcnw_bridge_find_var(const char *name)
{
    int i;

    for (i = 0; i < sBridgeVarCount; i++)
        if (!strcmp(sBridgeVars[i].name, name)) return &sBridgeVars[i];
    return NULL;
}
int gcnw_bridge_request_count(void) { return sBridgeRequestCount; }
const GcnwBridgeRequest *gcnw_bridge_request_info(int index)
{
    return index >= 0 && index < sBridgeRequestCount ? &sBridgeRequests[index] : NULL;
}

int gcnw_bridge_request(const char *name, const int *args, int nargs)
{
    int slot, i;

    if (!sModule || sBridgeQueueCount == BRIDGE_QUEUE) return 0;
    slot = (sBridgeQueueHead + sBridgeQueueCount) % BRIDGE_QUEUE;
    sBridgeQueue[slot].id = sBridgeNextId++;
    if (sBridgeNextId <= 0) sBridgeNextId = 1;
    snprintf(sBridgeQueue[slot].name, sizeof(sBridgeQueue[slot].name), "%s", name);
    sBridgeQueue[slot].nargs = nargs > 8 ? 8 : nargs;
    for (i = 0; i < sBridgeQueue[slot].nargs; i++) sBridgeQueue[slot].args[i] = args[i];
    sBridgeQueueCount++;
    return sBridgeQueue[slot].id;
}

int gcnw_bridge_result(int id, int *result)
{
    int i;

    for (i = 0; i < BRIDGE_RESULTS; i++)
    {
        if (sBridgeResults[i].id == id && id)
        {
            *result = sBridgeResults[i].result;
            sBridgeResults[i].id = 0; /* handed out once */
            return 1;
        }
    }
    return 0;
}

int gcnw_bridge_next_event(GcnwBridgeEvent *out)
{
    if (!sBridgeEventCount) return 0;
    *out = sBridgeEvents[sBridgeEventHead];
    sBridgeEventHead = (sBridgeEventHead + 1) % BRIDGE_EVENTS;
    sBridgeEventCount--;
    return 1;
}

static void bridge_reset(void)
{
    sBridgeVarCount = sBridgeRequestCount = 0;
    sBridgeQueueHead = sBridgeQueueCount = 0;
    sBridgeEventHead = sBridgeEventCount = 0;
    memset(sBridgeResults, 0, sizeof(sBridgeResults));
}

/* ---- game symbols (gcn_place.py's table, in the module descriptor) ------------------------ */
const GcnwSymbol *gcnw_find_symbol(const char *name)
{
    const GcnwSymbol *s;
    uint32_t lo = 0, hi;

    if (!sModule || !sModule->symbols) return NULL;
    hi = sModule->symbol_count;
    /* sorted by name */
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;
        int c;
        s = &sModule->symbols[mid];
        c = strcmp(s->name, name);
        if (!c) return s;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
