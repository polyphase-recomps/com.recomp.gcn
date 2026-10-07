/*
 * Standalone runner for a GameCube game built with com.recomp.gcn: the same runtime the
 * Polyphase addon uses, without the engine. Mostly a test harness:
 *
 *   gcn_runner --disc game.iso [--frames N] [--dump DIR] [--every N] [--dump-from F]
 *              [--realtime] [--fixed-clock] [--log FILE] [--wav FILE]
 *
 * Frames are written as PPM (DIR/frame_NNNNN.ppm). Without --realtime the game runs as
 * fast as it can (vertical retraces do not wait).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#endif

#include "gcn_platform.h"
#include "gcn_disc.h"
#include "gcn_gpu.h"
#include "gcnw_backend.h"
#include "gcnw_module.h"
#include "gcnw.h"

extern const GcnwModule GCN_GUEST_MODULE;

static void backtrace_log(void);
static GcnDisc *sDisc;
static FILE *sLog;
static int sFrames = 600, sEvery = 60, sDumpFrom = 0, sRealtime, sStackAt = -1, sWatchEvery = 60;
/* --fixed-clock: the guest's clock is the retrace count (1/60 s each), so two builds of a game
 * (decomp, recomp) see the same time and frame-pace the same way: comparable frame dumps */
static int sFixedClock;

/* --script "F:BUTTONS[:DURATION],...": hold the pad buttons (hex, GCN_PAD_*) from retrace F
 * for DURATION retraces (default 6). Without a script START is pressed now and then. */
#define MAX_SCRIPT 256
static struct { int frame, duration; unsigned buttons; } sScript[MAX_SCRIPT];
static int sScriptCount = -1;

static void script_parse(const char *text)
{
    char *end;

    sScriptCount = 0;
    while (*text && sScriptCount < MAX_SCRIPT)
    {
        sScript[sScriptCount].frame = (int)strtol(text, &end, 10);
        if (*end != ':') break;
        sScript[sScriptCount].buttons = (unsigned)strtoul(end + 1, &end, 16);
        sScript[sScriptCount].duration = *end == ':' ? (int)strtol(end + 1, &end, 10) : 6;
        sScriptCount++;
        text = end;
        while (*text == ',' || *text == ' ') text++;
    }
}
static const char *sDumpDir;
static int sFrame;

void gcnp_log(const char *text)
{
    fprintf(stderr, "%s\n", text);
    if (sLog)
    {
        fprintf(sLog, "%s\n", text);
        fflush(sLog);
    }
}

#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* Native call stack: wasm2c functions are named w2c_<game>_<guest function>. */
static void backtrace_log(void)
{
    void *frames[48];
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    HANDLE proc = GetCurrentProcess();
    USHORT n, i;

    SymInitialize(proc, NULL, TRUE);
    n = CaptureStackBackTrace(1, 48, frames, NULL);
    for (i = 0; i < n; i++)
    {
        DWORD64 disp = 0;
        char line[400];

        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &disp, sym))
        {
            IMAGEHLP_LINE64 src;
            DWORD col = 0;
            int len = snprintf(line, sizeof(line), "  #%d %s+0x%llx", i, sym->Name, (unsigned long long)disp);
            memset(&src, 0, sizeof(src));
            src.SizeOfStruct = sizeof(src);
            /* the return address is past the call: look at the byte before it */
            if (i > 0 && SymGetLineFromAddr64(proc, (DWORD64)(uintptr_t)frames[i] - 1, &col, &src))
            {
                const char *file = strrchr(src.FileName, '\\');
                snprintf(line + len, sizeof(line) - len, " (%s:%lu)", file ? file + 1 : src.FileName, src.LineNumber);
            }
        }
        else
            snprintf(line, sizeof(line), "  #%d %p", i, frames[i]);
        gcnp_log(line);
    }
}
/* Hang watchdog: if no retrace happens for a few seconds, print where the game thread is. */
static HANDLE sGameThread;
static volatile int sWatchFrame;

static DWORD WINAPI watchdog(void *p)
{
    int last = -1, still = 0;

    for (;;)
    {
        Sleep(1000);
        if (sFrame != last)
        {
            last = sFrame;
            still = 0;
            continue;
        }
        if (++still == 5)
        {
            CONTEXT ctx;
            STACKFRAME64 sf;
            HANDLE proc = GetCurrentProcess();
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            int i;

            gcnp_log("gcn_runner: HANG (no retrace for 5 s), game thread:");
            SuspendThread(sGameThread);
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            GetThreadContext(sGameThread, &ctx);
            SymInitialize(proc, NULL, TRUE);
            memset(&sf, 0, sizeof(sf));
            sf.AddrPC.Offset = ctx.Rip;
            sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = ctx.Rbp;
            sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = ctx.Rsp;
            sf.AddrStack.Mode = AddrModeFlat;
            for (i = 0; i < 40; i++)
            {
                DWORD64 disp = 0;
                char line[400];

                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, sGameThread, &sf, &ctx, NULL, SymFunctionTableAccess64,
                                 SymGetModuleBase64, NULL))
                    break;
                memset(buf, 0, sizeof(buf));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym))
                    snprintf(line, sizeof(line), "  #%d %s+0x%llx", i, sym->Name, (unsigned long long)disp);
                else
                    snprintf(line, sizeof(line), "  #%d %llx", i, (unsigned long long)sf.AddrPC.Offset);
                gcnp_log(line);
            }
            if (sLog) fflush(sLog);
            ExitProcess(4);
        }
    }
    return 0;
}

/* GCN_PROFILE=1: sampling profiler, the game thread's instruction pointer every ~1 ms,
 * folded into functions at exit (top 40). With GCN_RASTER_THREADS=1 drawing is included. */
#define PROF_MAX 8192
static DWORD64 sProfFunc[PROF_MAX];
static unsigned sProfHits[PROF_MAX];
static DWORD64 *sProfRaw;
static unsigned sProfTotal;
static volatile int sProfOn;

static DWORD WINAPI profiler(void *p)
{
    (void)p;
    sProfRaw = (DWORD64 *)malloc(sizeof(DWORD64) << 22);
    while (sProfOn && sProfRaw)
    {
        CONTEXT ctx;
        Sleep(1);
        if (SuspendThread(sGameThread) == (DWORD)-1) continue;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        /* raw addresses only: no locks may be taken while the game thread is suspended */
        if (GetThreadContext(sGameThread, &ctx) && sProfTotal < (1u << 22)) sProfRaw[sProfTotal++] = ctx.Rip;
        ResumeThread(sGameThread);
    }
    return 0;
}

static void profile_report(void)
{
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    unsigned n, total = sProfTotal;
    int i, k;

    sProfOn = 0;
    if (!total) return;
    Sleep(20);
    SymInitialize(proc, NULL, TRUE);
    for (n = 0; n < total; n++)
    {
        DWORD64 disp = 0, key = sProfRaw[n];
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(proc, key, &disp, sym)) key = sym->Address;
        for (i = 0; i < PROF_MAX && sProfFunc[i] && sProfFunc[i] != key; i++) {}
        if (i < PROF_MAX) { sProfFunc[i] = key; sProfHits[i]++; }
    }
    for (k = 0; k < 40; k++)
    {
        int best = -1;
        DWORD64 disp = 0;
        for (i = 0; i < PROF_MAX && sProfFunc[i]; i++)
            if (sProfHits[i] && (best < 0 || sProfHits[i] > sProfHits[best])) best = i;
        if (best < 0) break;
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        fprintf(stderr, "prof %5.1f%% %s\n", 100.0 * sProfHits[best] / total,
                SymFromAddr(proc, sProfFunc[best], &disp, sym) ? sym->Name : "?");
        sProfHits[best] = 0;
    }
    if (getenv("GCN_PROFILE_ADDRS"))
    {
        /* hottest instruction addresses, relative to the exe (llvm-symbolizer --inlining) */
        static DWORD64 addr[PROF_MAX];
        static unsigned hits[PROF_MAX];
        DWORD64 base = (DWORD64)(uintptr_t)GetModuleHandleA(NULL);
        for (n = 0; n < total; n++)
        {
            for (i = 0; i < PROF_MAX && addr[i] && addr[i] != sProfRaw[n]; i++) {}
            if (i < PROF_MAX) { addr[i] = sProfRaw[n]; hits[i]++; }
        }
        for (k = 0; k < 30; k++)
        {
            int best = -1;
            for (i = 0; i < PROF_MAX && addr[i]; i++)
                if (hits[i] && (best < 0 || hits[i] > hits[best])) best = i;
            if (best < 0) break;
            fprintf(stderr, "prof-addr %5.1f%% 0x%llx\n", 100.0 * hits[best] / total,
                    (unsigned long long)(addr[best] - base + 0x140000000ull));
            hits[best] = 0;
        }
    }
}

static void watchdog_start(void)
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sGameThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (getenv("GCN_PROFILE"))
    {
        sProfOn = 1;
        CreateThread(NULL, 0, profiler, NULL, 0, NULL);
        atexit(profile_report);
    }
}
#else
static void backtrace_log(void) {}
static void watchdog_start(void) {}
#endif

/* --dump-ram FILE: main RAM (24 MB, as the game sees it) when the run ends or crashes */
static const char *sRamDump;

static void ram_dump(void)
{
    FILE *f;

    if (!sRamDump || !(f = fopen(sRamDump, "wb"))) return;
    fwrite(gcnw_memory(), 1, 0x01800000, f);
    fclose(f);
}

void gcnp_crashed(void)
{
    backtrace_log();
    ram_dump();
    gcnp_log("gcn_runner: stopped");
    if (sLog) fflush(sLog);
    exit(3);
}

uint64_t gcnp_time_us(void)
{
    if (sFixedClock) return (uint64_t)sFrame * 16667u;
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart * 1000000.0 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
#endif
}

uint32_t gcnp_disc_read(void *dst, uint64_t offset, uint32_t size) { return gcn_disc_read(sDisc, dst, offset, size); }

uint64_t gcnp_disc_size(void) { return gcn_disc_size(sDisc); }

/* memory card A: <saves>/card_a.raw (2 MB, created on first use); no card in slot B */
#define CARD_BYTES (2u << 20)
static const char *sSaves = "saves";
static FILE *sCard;

static FILE *card_file(int chan)
{
    char path[1024];

    if (chan != 0) return NULL;
    if (sCard) return sCard;
#ifdef _WIN32
    CreateDirectoryA(sSaves, NULL);
#else
    mkdir(sSaves, 0755);
#endif
    snprintf(path, sizeof(path), "%s/card_a.raw", sSaves);
    sCard = fopen(path, "r+b");
    if (!sCard)
    {
        static char zero[4096];
        uint32_t i;
        sCard = fopen(path, "w+b");
        if (!sCard) return NULL;
        for (i = 0; i < CARD_BYTES; i += sizeof(zero)) fwrite(zero, 1, sizeof(zero), sCard);
        fflush(sCard);
    }
    return sCard;
}

uint32_t gcnp_card_size(int chan) { return card_file(chan) ? CARD_BYTES : 0; }

uint32_t gcnp_card_io(int chan, void *buf, uint32_t offset, uint32_t size, int write)
{
    FILE *f = card_file(chan);
    size_t n;

    if (!f || offset > CARD_BYTES || size > CARD_BYTES - offset) return 0;
    fseek(f, (long)offset, SEEK_SET);
    n = write ? fwrite(buf, 1, size, f) : fread(buf, 1, size, f);
    if (write) fflush(f);
    return (uint32_t)n;
}

/* --watch: guest variables (by name, from the wasm-ld map) printed with the frame stats */
#define MAX_WATCH 16
static struct
{
    char name[96];
    uint32_t addr, size;
} sWatch[MAX_WATCH];
static int sWatchCount;

static void watch_setup(const char *map, const char *list)
{
    char line[1024], names[1024];
    FILE *f = fopen(map, "r");
    char *tok;

    if (!f)
    {
        gcnp_log("gcn_runner: cannot open the link map (--map)");
        return;
    }
    snprintf(names, sizeof(names), "%s", list);
    for (tok = strtok(names, ","); tok && sWatchCount < MAX_WATCH; tok = strtok(NULL, ","))
    {
        snprintf(sWatch[sWatchCount].name, sizeof(sWatch[0].name), "%s", tok);
        /* a raw address (0x...) watches the word there */
        sWatch[sWatchCount].addr = !strncmp(tok, "0x", 2) ? (uint32_t)strtoul(tok, NULL, 16) : 0;
        sWatch[sWatchCount++].size = 4;
    }
    while (fgets(line, sizeof(line), f))
    {
        unsigned addr, off, size;
        char sym[512];
        int i;

        if (sscanf(line, "%x %x %x %511s", &addr, &off, &size, sym) != 4) continue;
        for (i = 0; i < sWatchCount; i++)
        {
            if (!sWatch[i].addr && !strcmp(sym, sWatch[i].name))
            {
                sWatch[i].addr = addr;
                sWatch[i].size = size;
            }
        }
    }
    fclose(f);
}

static void watch_print(void)
{
    char msg[1024];
    int i, n = 0;

    if (!sWatchCount) return;
    n += snprintf(msg + n, sizeof(msg) - n, "  watch:");
    for (i = 0; i < sWatchCount; i++)
    {
        uint32_t v = 0;
        const uint8_t *m = gcnw_memory() + (sWatch[i].addr & GCNW_RAM_MASK);
        if (!sWatch[i].addr)
        {
            n += snprintf(msg + n, sizeof(msg) - n, " %s=?", sWatch[i].name);
            continue;
        }
        if (sWatch[i].size == 1) v = m[0];
        else if (sWatch[i].size == 2) v = (uint32_t)((m[0] << 8) | m[1]);
        else v = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3];
        n += snprintf(msg + n, sizeof(msg) - n, " %s=%X", sWatch[i].name, v);
    }
    gcnp_log(msg);
}

static void dump_frame(void)
{
    char path[512];
    int w, h, x, y;
    const uint32_t *px = gcn_gpu_frame(&w, &h);
    FILE *f;

    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", sDumpDir, sFrame);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            uint32_t c = px[y * GCN_EFB_W + x];
            uint8_t rgb[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

uint32_t gcnp_retrace(void)
{
    static uint64_t next;
    uint32_t periods = 1;
    const GcnGpuStats *st;

    sFrame++;
    if (sFrame == sStackAt)
    {
        gcnp_log("gcn_runner: call stack at this retrace:");
        backtrace_log();
    }
    if (sDumpDir && sFrame >= sDumpFrom && sFrame % sEvery == 0) dump_frame();
    if (sWatchEvery > 0 && sFrame % sWatchEvery == 0 && sWatchEvery != 60) watch_print();
    if (sFrame % 60 == 0)
    {
        char msg[160];
        st = gcn_gpu_last_frame_stats();
        snprintf(msg, sizeof(msg), "gcn_runner: frame %d: %u primitives, %u vertices, %u display lists, %u unknown",
                 sFrame, st->primitives, st->vertices, st->display_lists, st->unknown_commands);
        gcnp_log(msg);
        st = gcn_gpu_current_stats();
        snprintf(msg, sizeof(msg), "  pending: fifo %u bytes, bp %u, xf %u, cp %u, prims %u, copies %u", st->fifo_bytes,
                 st->bp_writes, st->xf_writes, st->cp_writes, st->primitives, st->efb_copies);
        gcnp_log(msg);
        watch_print();
    }
    if (sFrame >= sFrames)
    {
        gcnp_log("gcn_runner: frame limit reached");
        ram_dump();
        if (sLog) fflush(sLog);
        exit(0);
    }
    if (sRealtime)
    {
        /* real time: retraces the game was too slow for count as passed (headless runs
         * keep one per frame, so they are repeatable) */
        uint64_t now = gcnp_time_us();
        if (next == 0) next = now;
        if (now > next + 16667)
        {
            periods = (uint32_t)((now - next) / 16667) + 1;
            if (periods > 4) periods = 4;
        }
        next += 16667ull * periods;
        if (now > next + 100000) next = now;
        while (gcnp_time_us() < next)
        {
#ifdef _WIN32
            Sleep(1);
#endif
        }
    }
    return periods;
}

int gcnp_pad(int port, GcnPad *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->connected = port == 0;
    if (port == 0 && sScriptCount >= 0)
    {
        int i;
        for (i = 0; i < sScriptCount; i++)
            if (sFrame >= sScript[i].frame && sFrame < sScript[i].frame + sScript[i].duration) pad->buttons |= sScript[i].buttons;
    }
    /* no script: press START now and then to get through menus */
    else if (port == 0 && sFrame > 200 && (sFrame % 120) < 6) pad->buttons = GCN_PAD_START;
    return pad->connected;
}

void gcnp_rumble(int port, int on) {}

/* --wav FILE: what the game plays, as 16-bit stereo (the sizes in the header are written at exit) */
static FILE *sWav;
static uint32_t sWavRate, sWavBytes;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(void)
{
    uint8_t h[44];

    memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0", 24);
    put_le32(h + 4, sWavBytes + 36);
    put_le32(h + 24, sWavRate);
    put_le32(h + 28, sWavRate * 4);
    put_le32(h + 32, 4 | (16 << 16)); /* block align 4, 16 bits per sample */
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, sWavBytes);
    fseek(sWav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), sWav);
    fseek(sWav, 0, SEEK_END);
}

static void wav_close(void)
{
    if (!sWav) return;
    wav_header();
    fclose(sWav);
    sWav = NULL;
}

void gcnp_audio(const int16_t *samples, uint32_t frames, uint32_t rate)
{
    if (!sWav) return;
    if (!sWavRate)
    {
        sWavRate = rate;
        wav_header();
        atexit(wav_close);
    }
    /* host order to little-endian */
    {
        uint8_t buf[8192 * 2];
        uint32_t i, n = frames * 2 > 8192 ? 8192 : frames * 2;
        for (i = 0; i < n; i++)
        {
            buf[i * 2] = (uint8_t)samples[i];
            buf[i * 2 + 1] = (uint8_t)((uint16_t)samples[i] >> 8);
        }
        fwrite(buf, 1, n * 2, sWav);
        sWavBytes += n * 2;
    }
}

/* ---- coroutines ---------------------------------------------------------------------------- */
#ifdef _WIN32
struct GcnpCoro
{
    void *fiber;
    void (*fn)(void *);
    void *arg;
};

static GcnpCoro sMainCoro;
static GcnpCoro *sCurrent;

GcnpCoro *gcnp_coro_current(void)
{
    if (!sCurrent)
    {
        sMainCoro.fiber = ConvertThreadToFiber(NULL);
        sCurrent = &sMainCoro;
    }
    return sCurrent;
}

static void WINAPI fiber_main(void *p)
{
    GcnpCoro *c = (GcnpCoro *)p;
    c->fn(c->arg);
}

GcnpCoro *gcnp_coro_create(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    GcnpCoro *c = (GcnpCoro *)calloc(1, sizeof(GcnpCoro));
    c->fn = fn;
    c->arg = arg;
    c->fiber = CreateFiber(stack_bytes, fiber_main, c);
    return c;
}

void gcnp_coro_switch(GcnpCoro *to)
{
    sCurrent = to;
    SwitchToFiber(to->fiber);
}

void gcnp_coro_destroy(GcnpCoro *coro)
{
    DeleteFiber(coro->fiber);
    free(coro);
}
#else
struct GcnpCoro
{
    ucontext_t ctx;
    void *stack;
    void (*fn)(void *);
    void *arg;
};

static GcnpCoro sMainCoro;
static GcnpCoro *sCurrent;

GcnpCoro *gcnp_coro_current(void)
{
    if (!sCurrent) sCurrent = &sMainCoro;
    return sCurrent;
}

static void coro_main(unsigned int hi, unsigned int lo)
{
    GcnpCoro *c = (GcnpCoro *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    c->fn(c->arg);
}

GcnpCoro *gcnp_coro_create(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    GcnpCoro *c = (GcnpCoro *)calloc(1, sizeof(GcnpCoro));
    uintptr_t p = (uintptr_t)c;

    c->fn = fn;
    c->arg = arg;
    c->stack = malloc(stack_bytes);
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = stack_bytes;
    c->ctx.uc_link = NULL;
    makecontext(&c->ctx, (void (*)(void))coro_main, 2, (unsigned int)(p >> 32), (unsigned int)p);
    return c;
}

void gcnp_coro_switch(GcnpCoro *to)
{
    GcnpCoro *from = gcnp_coro_current();
    sCurrent = to;
    swapcontext(&from->ctx, &to->ctx);
}

void gcnp_coro_destroy(GcnpCoro *coro)
{
    free(coro->stack);
    free(coro);
}
#endif

#ifdef GCNW_WATCH_WRITES
/* store watchpoint (debugging build): GCN_WATCH_WRITE=lo:hi[:value] stops at the first store
 * into [lo, hi) (of that 32-bit value, in either byte order) with a backtrace */
uint32_t gcnw_watch_lo, gcnw_watch_hi;
static uint32_t sWatchValue;
static int sWatchHasValue;

void gcnw_watch_hit(uint32_t addr, uint32_t size, uint64_t value)
{
    char msg[160];
    uint32_t v = (uint32_t)value;

    if (sWatchHasValue && sWatchValue == 1)
    {
        /* value "1": only misaligned RAM pointers (bad object pointers) */
        uint32_t be = __builtin_bswap32(v);
        if (size != 4 || be < 0x80000000u || be >= 0x81800000u || !(be & 3)) return;
    }
    else if (sWatchHasValue && v != sWatchValue && v != __builtin_bswap32(sWatchValue)) return;
    {
        /* GCN_WATCH_SKIP=n: report the (n+1)th matching store */
        static int skip = -1;
        if (skip < 0) skip = getenv("GCN_WATCH_SKIP") ? atoi(getenv("GCN_WATCH_SKIP")) : 0;
        if (skip-- > 0) return;
    }
    snprintf(msg, sizeof(msg), "gcn_runner: store of %u bytes at %08X (value %08X), frame %d:", size, addr | 0x80000000u,
             v, sFrame);
    gcnp_log(msg);
    backtrace_log();
    exit(4);
}

static void watch_writes_setup(void)
{
    const char *e = getenv("GCN_WATCH_WRITE");
    char *end;

    if (!e) return;
    gcnw_watch_lo = (uint32_t)strtoul(e, &end, 16) & 0x01FFFFFFu;
    gcnw_watch_hi = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) & 0x01FFFFFFu : gcnw_watch_lo + 4;
    if (*end == ':')
    {
        sWatchValue = (uint32_t)strtoul(end + 1, NULL, 16);
        sWatchHasValue = 1;
    }
}
#else
static void watch_writes_setup(void) {}
#endif

int main(int argc, char **argv)
{
    const char *disc = NULL, *map = NULL, *watch = NULL;
    int i;

    watch_writes_setup();
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) sFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) sDumpDir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) sEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) sDumpFrom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--realtime")) sRealtime = 1;
        else if (!strcmp(argv[i], "--fixed-clock")) sFixedClock = 1;
        else if (!strcmp(argv[i], "--stack-at") && i + 1 < argc) sStackAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) sSaves = argv[++i];
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc) sRamDump = argv[++i];
        else if (!strcmp(argv[i], "--watch-every") && i + 1 < argc) sWatchEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_parse(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) sLog = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) sWav = fopen(argv[++i], "wb");
        else
        {
            fprintf(stderr, "usage: gcn_runner --disc game.iso [--frames N] [--dump DIR] [--every N] "
                            "[--dump-from F] [--realtime] [--fixed-clock] [--log FILE] [--wav FILE]\n");
            return 2;
        }
    }
    if (!disc || !(sDisc = gcn_disc_open(disc)))
    {
        fprintf(stderr, "gcn_runner: cannot open the disc image or unpacked disc folder (--disc)\n");
        return 2;
    }
    if (!gcnw_instantiate(&GCN_GUEST_MODULE))
    {
        fprintf(stderr, "gcn_runner: cannot instantiate the game\n");
        return 2;
    }
    if (map && watch) watch_setup(map, watch);
    gcnp_coro_current();
    watchdog_start();
    gcnw_run();
    gcnp_log("gcn_runner: main() returned");
    return 0;
}
