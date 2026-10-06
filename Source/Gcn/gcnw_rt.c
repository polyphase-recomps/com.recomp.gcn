/*
 * wasm2c runtime for the guest module (replaces wabt's wasm-rt-impl.c): one fixed
 * memory buffer laid out as described in gcnw.h whatever size the module declares,
 * function tables, traps reported through the platform host, and the exception
 * state used by setjmp/longjmp (wasm exception handling). Plain C, no threads, no
 * signals, so it builds with any host compiler (MSVC/clang, devkitPPC...).
 * The host provides host_log() and host_crashed().
 */
#include <stdlib.h>
#include <string.h>

#include "wasm-rt.h"
#include "wasm-rt-exceptions.h"

#include "gcnw.h"

void host_log(const char *fmt, ...);
void host_crashed(void); /* does not return */

static bool sInitialized;

void wasm_rt_init(void)
{
    sInitialized = true;
}

bool wasm_rt_is_initialized(void)
{
    return sInitialized;
}

void wasm_rt_free(void)
{
    sInitialized = false;
}

void wasm_rt_init_thread(void) {}
void wasm_rt_free_thread(void) {}

const char *wasm_rt_strerror(wasm_rt_trap_t trap)
{
    switch (trap)
    {
    case WASM_RT_TRAP_NONE: return "no error";
    case WASM_RT_TRAP_OOB: return "out-of-bounds access";
    case WASM_RT_TRAP_INT_OVERFLOW: return "integer overflow";
    case WASM_RT_TRAP_DIV_BY_ZERO: return "integer divide by zero";
    case WASM_RT_TRAP_INVALID_CONVERSION: return "invalid conversion";
    case WASM_RT_TRAP_UNREACHABLE: return "unreachable executed";
    case WASM_RT_TRAP_CALL_INDIRECT: return "invalid call_indirect";
    case WASM_RT_TRAP_UNCAUGHT_EXCEPTION: return "uncaught exception";
    case WASM_RT_TRAP_UNALIGNED: return "unaligned atomic access";
    case WASM_RT_TRAP_NULL_REF: return "null reference";
    default: return "trap";
    }
}

void gcnw_report_trap(const char *what); /* gcnw_backend.c */

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t trap)
{
    gcnw_report_trap(wasm_rt_strerror(trap));
    abort();
}

void gcnw_bad_indirect_call(uint32_t index)
{
    host_log("call through an empty function table slot (%u)", (unsigned)index);
    wasm_rt_trap(WASM_RT_TRAP_CALL_INDIRECT);
}

/* ---- memory --------------------------------------------------------------------- */
#if defined(GEKKO)
#include <ogc/cache.h>
#include <ogc/system.h>

/* MEM2 below libogc's arena is the guest's: its RAM at GCNW_GUEST_BASE (gcnw.h), the locked
 * cache window after it, then the big blocks (ARAM). Defining the arena's start here moves
 * it (rvl.ld only PROVIDEs it). */
#define GCNW_RESERVED_BLOCKS 0x91820000u
#define GCNW_RESERVED_END 0x92820000u
#define GCNW_REG_SCRATCH 0x92820000u /* 128 KB, see map_guest_views */
__asm__(".globl __Arena2Lo\n\t.set __Arena2Lo, 0x92840000\n");

/* The guest's other views of its memory (gcnw.h), in data BATs libogc leaves free:
 *   DBAT6  0x10000000, 32 MB: the guest's physical addresses (cached)
 *   DBAT7  0xF0000000, 128 KB: the locked cache window (guest 0xE0000000 + GCNW_WRAP)
 *   DBAT2  0xDC000000, 128 KB: hardware registers the decomp reaches through pointers it
 *          does not declare volatile (0xCC00xxxx + GCNW_WRAP), onto scratch memory - such
 *          accesses never reached the hardware (only volatile ones go to the host), and
 *          now must not fault either
 *   DBAT3  0xD8000000, 16 MB: the CPU's view of the embedded frame buffer (0xC8xxxxxx +
 *          GCNW_WRAP), the real one (physical 0x08000000, uncached): the game peeks at the
 *          picture it draws with the console's GPU
 * DBAT5 (libogc: MEM2 uncached, a 256 MB block) shrinks to MEM2's 64 MB so DBAT2/3 don't
 * overlap it. */
static void map_guest_views(void)
{
    const uint32_t lcPhys = (GCNW_GUEST_BASE + GCNW_LC_OFFSET) & 0x3FFFFFFFu;
    const uint32_t regPhys = GCNW_REG_SCRATCH & 0x3FFFFFFFu;
    __asm__ volatile("sync\n\t"
                     "mtspr 573,%0\n\t"
                     "mtspr 572,%1\n\t"
                     "mtspr 575,%2\n\t"
                     "mtspr 574,%3\n\t"
                     "mtspr 570,%4\n\t"
                     "mtspr 541,%5\n\t"
                     "mtspr 540,%6\n\t"
                     "mtspr 543,%7\n\t"
                     "mtspr 542,%8\n\t"
                     "isync"
                     :
                     : "r"(0x10000002u), "r"(0x100003FFu), "r"(lcPhys | 2u), "r"(0xF0000003u),
                       "r"(0xD00007FFu), "r"(regPhys | 2u), "r"(0xDC000003u), "r"(0x0800002Au),
                       "r"(0xD80001FFu)
                     : "memory");
}

/* big blocks, kept for the next game start (the sizes don't change): from the reserved
 * region, else carved from the top of the MEM2 arena */
static struct
{
    void *p;
    size_t bytes;
    int used;
} sBig[4];
static uint32_t sReservedNext = GCNW_RESERVED_BLOCKS;

void *gcnw_big_alloc(size_t bytes)
{
    int i;
    void *p = NULL;

    bytes = (bytes + 31) & ~(size_t)31;
    for (i = 0; i < 4 && !p; i++)
    {
        if (sBig[i].p && !sBig[i].used && sBig[i].bytes >= bytes)
        {
            sBig[i].used = 1;
            p = sBig[i].p;
        }
    }
    for (i = 0; i < 4 && !p; i++)
    {
        if (!sBig[i].p)
        {
            uint32_t at;
            if (GCNW_RESERVED_END - sReservedNext >= bytes)
            {
                at = sReservedNext;
                sReservedNext += (uint32_t)bytes;
            }
            else
            {
                uint32_t hi = (uint32_t)(uintptr_t)SYS_GetArena2Hi(), lo = (uint32_t)(uintptr_t)SYS_GetArena2Lo();
                at = (hi - (uint32_t)bytes) & ~31u;
                if (hi < bytes || at < lo) return NULL;
                SYS_SetArena2Hi((void *)(uintptr_t)at);
            }
            sBig[i].p = (void *)(uintptr_t)at;
            sBig[i].bytes = bytes;
            sBig[i].used = 1;
            p = sBig[i].p;
        }
    }
    if (p)
    {
        memset(p, 0, bytes);
        DCFlushRange(p, (u32)bytes);
    }
    return p;
}

void gcnw_big_free(void *p)
{
    int i;
    for (i = 0; i < 4; i++)
        if (sBig[i].p == p) sBig[i].used = 0;
}

void gcnw_dcache_flush(void *p, size_t bytes)
{
    if (p && bytes) DCFlushRange(p, (u32)bytes);
}
#else
void *gcnw_big_alloc(size_t bytes) { return calloc(1, bytes); }
void gcnw_big_free(void *p) { free(p); }
void gcnw_dcache_flush(void *p, size_t bytes)
{
    (void)p;
    (void)bytes;
}
#endif

void wasm_rt_allocate_memory(wasm_rt_memory_t *mem, uint64_t initial_pages, uint64_t max_pages, bool is64,
                             uint32_t page_size)
{
#if defined(GEKKO)
    mem->data = (uint8_t *)(uintptr_t)GCNW_GUEST_BASE;
    memset(mem->data, 0, GCNW_MEM_BYTES);
    DCFlushRange(mem->data, GCNW_MEM_BYTES);
    map_guest_views();
#else
    mem->data = (uint8_t *)gcnw_big_alloc(GCNW_MEM_BYTES);
#endif
    if (mem->data == NULL)
    {
        host_log("cannot allocate guest memory (%u bytes)", (unsigned)GCNW_MEM_BYTES);
        host_crashed();
    }
    mem->data_end = mem->data + GCNW_MEM_BYTES;
    mem->page_size = page_size;
    mem->pages = initial_pages;
    mem->max_pages = max_pages;
    mem->size = initial_pages * page_size;
    mem->is64 = is64;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *mem, uint64_t delta)
{
    /* the layout is fixed (gcnw.h): the module is linked with all its memory */
    return delta == 0 ? mem->pages : (uint64_t)-1;
}

void wasm_rt_free_memory(wasm_rt_memory_t *mem)
{
#if !defined(GEKKO)
    gcnw_big_free(mem->data);
#endif
    mem->data = mem->data_end = NULL;
}

/* ---- tables ----------------------------------------------------------------------- */
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t *table, uint32_t elements, uint32_t max_elements)
{
    table->data = (wasm_rt_funcref_t *)calloc(elements ? elements : 1, sizeof(wasm_rt_funcref_t));
    table->size = elements;
    table->max_size = max_elements;
}

void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t *table)
{
    free(table->data);
    table->data = NULL;
}

uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t *table, uint32_t delta, wasm_rt_funcref_t init)
{
    return (uint32_t)-1;
}

void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t *table, uint32_t elements, uint32_t max_elements)
{
    table->data = (wasm_rt_externref_t *)calloc(elements ? elements : 1, sizeof(wasm_rt_externref_t));
    table->size = elements;
    table->max_size = max_elements;
}

void wasm_rt_free_externref_table(wasm_rt_externref_table_t *table)
{
    free(table->data);
    table->data = NULL;
}

uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t *table, uint32_t delta, wasm_rt_externref_t init)
{
    return (uint32_t)-1;
}

/* ---- exceptions (setjmp/longjmp of the guest) ---------------------------------------
 * The game runs on one thread, so the state is plain statics. */
#define MAX_EXCEPTION_SIZE 256

static wasm_rt_tag_t sExceptionTag;
static uint8_t sException[MAX_EXCEPTION_SIZE];
static uint32_t sExceptionSize;
static wasm_rt_jmp_buf *sUnwindTarget;

void wasm_rt_load_exception(const wasm_rt_tag_t tag, uint32_t size, const void *values)
{
    if (size > MAX_EXCEPTION_SIZE)
    {
        wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
    }
    sExceptionTag = tag;
    sExceptionSize = size;
    if (size)
    {
        memcpy(sException, values, size);
    }
}

WASM_RT_NO_RETURN void wasm_rt_throw(void)
{
    if (sUnwindTarget == NULL)
    {
        wasm_rt_trap(WASM_RT_TRAP_UNCAUGHT_EXCEPTION);
    }
    WASM_RT_LONGJMP(*sUnwindTarget, WASM_RT_TRAP_UNCAUGHT_EXCEPTION);
}

/* forget a previous run's state (the host may have unwound it with longjmp) */
void gcnw_rt_reset(void)
{
    sUnwindTarget = NULL;
    sExceptionTag = NULL;
    sExceptionSize = 0;
}

WASM_RT_UNWIND_TARGET *wasm_rt_get_unwind_target(void)
{
    return sUnwindTarget;
}

void wasm_rt_set_unwind_target(WASM_RT_UNWIND_TARGET *target)
{
    sUnwindTarget = target;
}

wasm_rt_tag_t wasm_rt_exception_tag(void)
{
    return sExceptionTag;
}

uint32_t wasm_rt_exception_size(void)
{
    return sExceptionSize;
}

void *wasm_rt_exception(void)
{
    return sException;
}
