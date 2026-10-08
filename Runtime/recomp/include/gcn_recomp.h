/**
 * @file gcn_recomp.h
 * @brief What the GameCube recompiler's generated C compiles against: the Gekko register file,
 *        guest memory access, condition register / carry helpers, floating point and paired
 *        singles, and the runtime's hooks.
 *
 * Every recompiled function is `void f_ADDR(uint8_t* mem, gcnr_ctx* c)`: `mem` is the guest memory
 * buffer (big-endian, the same one com.recomp.gcn's runtime uses: GCNR_OFFSET == GCNW_OFFSET), `c`
 * the registers of the guest thread running it.
 *
 * Build options (C defines, no regeneration needed):
 *   GCNR_FMA=1       fmadd and friends fused, as Gekko does (0: multiply then add, like the decomp
 *                    build compiled with -ffp-contract=off; for frame-exact comparisons)
 *   GCNR_CHECKED=1   also log the first accesses that reached the hardware page through an
 *                    address the recompiler couldn't prove (pointers held in variables,
 *                    e.g. GX's __cpReg): diagnostics only, they are handled either way
 *   GCNR_WATCH=1     store watchpoint: GCNR_WATCH=<hex address> in the environment logs every
 *                    store to that word, with a native backtrace (slow; debugging)
 *   GCNR_LOOP_LIMIT  iterations of a busy-wait loop (no stores, no calls) between calls to
 *                    gcnr_loop_poll, which delivers pending events (the decomp build: 2048
 *                    repeated volatile loads)
 */
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GCNR_FMA
#define GCNR_FMA 1
#endif
#ifndef GCNR_CHECKED
#define GCNR_CHECKED 0
#endif
#ifndef GCNR_WATCH
#define GCNR_WATCH 0
#endif
#ifndef GCNR_LOOP_LIMIT
#define GCNR_LOOP_LIMIT 2048u
#endif

#if defined(__GNUC__) || defined(__clang__)
#define GCNR_LIKELY(x) __builtin_expect(!!(x), 1)
#define GCNR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define GCNR_BSWAP16(x) __builtin_bswap16(x)
#define GCNR_BSWAP32(x) __builtin_bswap32(x)
#define GCNR_BSWAP64(x) __builtin_bswap64(x)
#else
#include <stdlib.h>
#define GCNR_LIKELY(x) (x)
#define GCNR_UNLIKELY(x) (x)
#define GCNR_BSWAP16(x) _byteswap_ushort(x)
#define GCNR_BSWAP32(x) _byteswap_ulong(x)
#define GCNR_BSWAP64(x) _byteswap_uint64(x)
#endif

/* ---- registers --------------------------------------------------------------------------- */
typedef struct gcnr_fpr
{
    double ps0, ps1; /* paired single halves (doubles in double-precision ops) */
} gcnr_fpr;

typedef struct gcnr_ctx
{
    uint32_t r[32];
    gcnr_fpr f[32];
    uint32_t cr;  /* as the hardware register: CR0 in the top nibble */
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca, xer_bc; /* XER bits, and its byte count (lswx/stswx) */
    uint32_t fpscr;
    uint32_t msr;
    uint32_t loop;    /* backward branches since the last gcnr_loop_poll */
    uint32_t reserve; /* lwarx reservation */
} gcnr_ctx;

typedef void (*gcnr_func)(uint8_t* mem, gcnr_ctx* c);
typedef struct gcnr_named_func
{
    const char* name;
    gcnr_func fn;
} gcnr_named_func;

/* the graphics quantisation registers: one set for every guest thread (the runtime's threads
 * don't save them per thread, nor does the decomp build) */
extern uint32_t gcnr_gqr[8];

/* ---- the runtime's side (recomp_gcn.c) ---------------------------------------------------- */
gcnr_func gcnr_lookup(uint32_t addr);                /* indirect calls; fatal for a non-entry */
gcnr_func gcnr_lookup_from(uint32_t addr, const gcnr_ctx* c); /* same, reporting the caller's registers */
void gcnr_loop_poll(uint8_t* mem, gcnr_ctx* c);      /* a long-running loop: deliver events */
uint32_t gcnr_mmio_read(uint32_t addr, int bytes);   /* hardware registers */
void gcnr_mmio_write(uint32_t addr, uint32_t value, int bytes); /* (incl. the write-gather pipe) */
void gcnr_psq_st_mmio(gcnr_ctx* c, int fs, uint32_t addr, int w, int i); /* psq_st to the pipe */
void gcnr_checked_access(uint32_t addr, int write);  /* GCNR_CHECKED: an unproven access hit 0xCC */
uint32_t gcnr_mfspr(gcnr_ctx* c, uint32_t spr);
void gcnr_mtspr(gcnr_ctx* c, uint32_t spr, uint32_t value);
uint64_t gcnr_timebase(void);
void gcnr_syscall(uint8_t* mem, gcnr_ctx* c);
void gcnr_trap(gcnr_ctx* c, uint32_t addr);
void gcnr_unhandled(gcnr_ctx* c, uint32_t addr, const char* what); /* rfi, bad jump, ... */
void gcnr_trace_func(uint32_t addr, uint8_t* mem, const gcnr_ctx* c); /* GcnRecomp --trace */
void gcnr_sp_changed(uint32_t at, uint32_t callee, uint32_t before, uint32_t after); /* --trace */

#define GCNR_CALL_INDIRECT(mem, c, target) (gcnr_lookup_from((target), (c))((mem), (c)))
#define GCNR_LOOP(mem, c)                                          \
    do                                                             \
    {                                                              \
        if (GCNR_UNLIKELY(++(c)->loop >= GCNR_LOOP_LIMIT))         \
            gcnr_loop_poll((mem), (c));                            \
    } while (0)
/* every other backward branch: pending interrupts every GCNR_LOOP_LIMIT rounds (when enabled),
 * as the console takes them anywhere; no retrace, no thread switch (gcnr_loop_any) */
void gcnr_loop_any(uint8_t* mem, gcnr_ctx* c);
#define GCNR_LOOP_ANY(mem, c)                                      \
    do                                                             \
    {                                                              \
        if (GCNR_UNLIKELY(++(c)->loop >= GCNR_LOOP_LIMIT))         \
            gcnr_loop_any((mem), (c));                             \
    } while (0)

/* ---- guest memory -------------------------------------------------------------------------- */
/* the same folding as gcnw.h GCNW_OFFSET on PC: RAM mirrors onto one buffer, locked cache after it */
#define GCNR_RAM_MASK 0x01FFFFFFu
#define GCNR_LC_OFFSET 0x02000000u
#define GCNR_LC_MASK 0x00003FFFu
#define GCNR_OFFSET(a) \
    ((((uint32_t)(a) >> 28) == 0xEu) ? (GCNR_LC_OFFSET | ((uint32_t)(a) & GCNR_LC_MASK)) : ((uint32_t)(a) & GCNR_RAM_MASK))
#define GCNR_PTR(mem, a) ((mem) + GCNR_OFFSET(a))

/* Accesses whose address the recompiler couldn't prove may still reach the hardware registers
 * (GX keeps pointers to them in variables): those go to the MMIO handlers, as the decomp build's
 * volatile accesses do. Proven ones never come here (the generator calls gcnr_mmio_* directly). */
#define GCNR_IS_MMIO(a) GCNR_UNLIKELY(((uint32_t)(a) >> 24) == 0xCCu)
#if GCNR_WATCH
extern uint32_t gcnr_watch_addr;
void gcnr_watch_hit(uint32_t addr, uint64_t value, int bytes);
#define GCNR_WATCHED(a, v, n) \
    if (GCNR_UNLIKELY((((uint32_t)(a) & 0x01FFFFFCu) == gcnr_watch_addr))) gcnr_watch_hit((a), (v), (n))
#else
#define GCNR_WATCHED(a, v, n)
#endif
#if GCNR_CHECKED
#define GCNR_NOTE(a, w) gcnr_checked_access((a), (w))
#else
#define GCNR_NOTE(a, w) ((void)0)
#endif

static inline uint32_t gcnr_lw(const uint8_t* mem, uint32_t a)
{
    uint32_t v;
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 0); return gcnr_mmio_read(a, 4); }
    memcpy(&v, GCNR_PTR(mem, a), 4);
    return GCNR_BSWAP32(v);
}
static inline uint32_t gcnr_lhz(const uint8_t* mem, uint32_t a)
{
    uint16_t v;
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 0); return gcnr_mmio_read(a, 2) & 0xFFFFu; }
    memcpy(&v, GCNR_PTR(mem, a), 2);
    return GCNR_BSWAP16(v);
}
static inline uint32_t gcnr_lha(const uint8_t* mem, uint32_t a)
{
    return (uint32_t)(int32_t)(int16_t)gcnr_lhz(mem, a);
}
static inline uint32_t gcnr_lbz(const uint8_t* mem, uint32_t a)
{
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 0); return gcnr_mmio_read(a, 1) & 0xFFu; }
    return *GCNR_PTR(mem, a);
}
static inline uint64_t gcnr_ld(const uint8_t* mem, uint32_t a)
{
    uint64_t v;
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 0); return (uint64_t)gcnr_mmio_read(a, 4) << 32 | gcnr_mmio_read(a + 4, 4); }
    memcpy(&v, GCNR_PTR(mem, a), 8);
    return GCNR_BSWAP64(v);
}
static inline void gcnr_sw(uint8_t* mem, uint32_t a, uint32_t v)
{
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 1); gcnr_mmio_write(a, v, 4); return; }
    GCNR_WATCHED(a, v, 4);
    v = GCNR_BSWAP32(v);
    memcpy(GCNR_PTR(mem, a), &v, 4);
}
static inline void gcnr_sh(uint8_t* mem, uint32_t a, uint32_t v)
{
    uint16_t h = GCNR_BSWAP16((uint16_t)v);
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 1); gcnr_mmio_write(a, v & 0xFFFFu, 2); return; }
    GCNR_WATCHED(a, v, 2);
    memcpy(GCNR_PTR(mem, a), &h, 2);
}
static inline void gcnr_sb(uint8_t* mem, uint32_t a, uint32_t v)
{
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 1); gcnr_mmio_write(a, v & 0xFFu, 1); return; }
    GCNR_WATCHED(a, v, 1);
    *GCNR_PTR(mem, a) = (uint8_t)v;
}
static inline void gcnr_sd(uint8_t* mem, uint32_t a, uint64_t v)
{
    if (GCNR_IS_MMIO(a)) { GCNR_NOTE(a, 1); gcnr_mmio_write(a, (uint32_t)(v >> 32), 4); gcnr_mmio_write(a + 4, (uint32_t)v, 4); return; }
    GCNR_WATCHED(a, v, 8);
    GCNR_WATCHED(a + 4, v, 8);
    v = GCNR_BSWAP64(v);
    memcpy(GCNR_PTR(mem, a), &v, 8);
}
/* byte-reversed (little-endian) accesses: lwbrx & co */
static inline uint32_t gcnr_lwbr(const uint8_t* mem, uint32_t a) { return GCNR_BSWAP32(gcnr_lw(mem, a)); }
static inline uint32_t gcnr_lhbr(const uint8_t* mem, uint32_t a) { return GCNR_BSWAP16((uint16_t)gcnr_lhz(mem, a)); }
static inline void gcnr_swbr(uint8_t* mem, uint32_t a, uint32_t v) { gcnr_sw(mem, a, GCNR_BSWAP32(v)); }
static inline void gcnr_shbr(uint8_t* mem, uint32_t a, uint32_t v) { gcnr_sh(mem, a, GCNR_BSWAP16((uint16_t)v)); }

/* dcbz / dcbz_l: zero the 32-byte block */
static inline void gcnr_dcbz(uint8_t* mem, uint32_t a)
{
    memset(GCNR_PTR(mem, a & ~31u), 0, 32);
}

/* ---- bit casts --------------------------------------------------------------------------------- */
static inline float gcnr_f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline uint32_t gcnr_bits32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline double gcnr_f64(uint64_t bits) { double d; memcpy(&d, &bits, 8); return d; }
static inline uint64_t gcnr_bits64(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

/* ---- integer ------------------------------------------------------------------------------------- */
static inline uint32_t gcnr_rotl(uint32_t v, uint32_t n)
{
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}

/* x + y + ci, setting XER[CA] (ca) and XER[OV/SO] (ov): every add / subtract form */
static inline uint32_t gcnr_add(gcnr_ctx* c, uint32_t x, uint32_t y, uint32_t ci, int ca, int ov)
{
    const uint64_t s = (uint64_t)x + y + ci;
    const uint32_t r = (uint32_t)s;
    if (ca) c->xer_ca = (uint32_t)(s >> 32);
    if (ov)
    {
        const uint32_t o = ((x ^ r) & (y ^ r)) >> 31;
        c->xer_ov = o;
        c->xer_so |= o;
    }
    return r;
}

static inline uint32_t gcnr_mullw(gcnr_ctx* c, uint32_t a, uint32_t b, int ov)
{
    const int64_t p = (int64_t)(int32_t)a * (int32_t)b;
    if (ov)
    {
        const uint32_t o = p != (int64_t)(int32_t)p;
        c->xer_ov = o;
        c->xer_so |= o;
    }
    return (uint32_t)p;
}

/* divw / divwu never trap; results as the decomp build (gcnw_ops.h DIV_S / DIV_U) */
static inline uint32_t gcnr_divw(gcnr_ctx* c, uint32_t a, uint32_t b, int ov)
{
    const int32_t x = (int32_t)a, y = (int32_t)b;
    const int bad = y == 0 || (x == INT32_MIN && y == -1);
    if (ov)
    {
        c->xer_ov = (uint32_t)bad;
        c->xer_so |= (uint32_t)bad;
    }
    if (GCNR_UNLIKELY(y == 0)) return x < 0 ? 0xFFFFFFFFu : 0u;
    if (GCNR_UNLIKELY(bad)) return 0u;
    return (uint32_t)(x / y);
}
static inline uint32_t gcnr_divwu(gcnr_ctx* c, uint32_t a, uint32_t b, int ov)
{
    if (ov)
    {
        c->xer_ov = b == 0;
        c->xer_so |= b == 0;
    }
    return b == 0 ? 0u : a / b;
}

static inline uint32_t gcnr_cntlzw(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return v ? (uint32_t)__builtin_clz(v) : 32u;
#else
    uint32_t n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
#endif
}

/* sraw / srawi: CA set when a negative value lost 1 bits */
static inline uint32_t gcnr_sraw(gcnr_ctx* c, uint32_t s, uint32_t n)
{
    const int32_t v = (int32_t)s;
    if (n & 0x20)
    {
        c->xer_ca = v < 0;
        return v < 0 ? 0xFFFFFFFFu : 0u;
    }
    n &= 31;
    c->xer_ca = v < 0 && n && (s & ((1u << n) - 1u)) != 0;
    return (uint32_t)(v >> n);
}

/* ---- condition register --------------------------------------------------------------------- */
#define GCNR_CRBIT(c, bi) (((c)->cr >> (31 - (bi))) & 1u)
static inline void gcnr_setcrbit(gcnr_ctx* c, int bit, uint32_t v)
{
    const uint32_t m = 0x80000000u >> bit;
    c->cr = v ? (c->cr | m) : (c->cr & ~m);
}
static inline void gcnr_setcrf(gcnr_ctx* c, int field, uint32_t nibble)
{
    const int sh = 28 - 4 * field;
    c->cr = (c->cr & ~(0xFu << sh)) | ((nibble & 0xFu) << sh);
}
static inline void gcnr_cmp(gcnr_ctx* c, int field, int32_t a, int32_t b)
{
    gcnr_setcrf(c, field, (a < b ? 8u : a > b ? 4u : 2u) | c->xer_so);
}
static inline void gcnr_cmpl(gcnr_ctx* c, int field, uint32_t a, uint32_t b)
{
    gcnr_setcrf(c, field, (a < b ? 8u : a > b ? 4u : 2u) | c->xer_so);
}
#define GCNR_CR0(c, v) gcnr_cmp((c), 0, (int32_t)(v), 0)
static inline uint32_t gcnr_xer(const gcnr_ctx* c)
{
    return (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29) | (c->xer_bc & 0x7F);
}
static inline void gcnr_setxer(gcnr_ctx* c, uint32_t v)
{
    c->xer_so = v >> 31;
    c->xer_ov = (v >> 30) & 1;
    c->xer_ca = (v >> 29) & 1;
    c->xer_bc = v & 0x7F;
}

/* ---- floating point ----------------------------------------------------------------------------- */
#define GCNR_SINGLE(x) ((double)(float)(x))
#if GCNR_FMA
#define GCNR_MADD(a, c, b) fma((a), (c), (b))
#define GCNR_MSUB(a, c, b) fma((a), (c), -(b))
#else
#define GCNR_MADD(a, c, b) ((a) * (c) + (b))
#define GCNR_MSUB(a, c, b) ((a) * (c) - (b))
#endif

/* fcmpu / fcmpo / ps_cmp: the CR field and FPSCR[FPCC] */
static inline void gcnr_fcmp(gcnr_ctx* c, int field, double a, double b)
{
    const uint32_t v = (a != a || b != b) ? 1u : a < b ? 8u : a > b ? 4u : 2u;
    gcnr_setcrf(c, field, v);
    c->fpscr = (c->fpscr & ~0xF000u) | (v << 12);
}
#define GCNR_CR1(c) gcnr_setcrf((c), 1, (c)->fpscr >> 28)

static inline double gcnr_fsel(double a, double c, double b)
{
    return a >= 0.0 ? c : b; /* NaN selects b */
}

/* fres / frsqrte and their paired forms: exact results (the decomp build's intrinsics too) */
static inline double gcnr_fres(double b) { return GCNR_SINGLE(1.0 / b); }
static inline double gcnr_frsqrte(double b) { return 1.0 / sqrt(b); }

/* fctiw / fctiwz: saturating, the integer in the low word of the register's bits */
static inline double gcnr_fctiw(double b, int truncate)
{
    int32_t v;
    if (b != b) v = INT32_MIN;
    else if (b >= 2147483648.0) v = INT32_MAX;
    else if (b < -2147483648.0) v = INT32_MIN;
    else v = (int32_t)(truncate ? b : nearbyint(b));
    return gcnr_f64(0xFFF8000000000000ull | (uint32_t)v);
}

/* ---- paired singles: quantised loads and stores (GQRs) -------------------------------------- */
static inline double gcnr_dequant(const uint8_t* mem, uint32_t a, uint32_t type, uint32_t scale)
{
    const float s = ldexpf(1.0f, -(int)(((int32_t)(scale << 26)) >> 26));
    switch (type)
    {
    case 4: return (double)((float)gcnr_lbz(mem, a) * s);
    case 5: return (double)((float)gcnr_lhz(mem, a) * s);
    case 6: return (double)((float)(int8_t)gcnr_lbz(mem, a) * s);
    case 7: return (double)((float)(int16_t)gcnr_lhz(mem, a) * s);
    default: return (double)gcnr_f32(gcnr_lw(mem, a));
    }
}
static inline uint32_t gcnr_qsize(uint32_t type)
{
    return type == 4 || type == 6 ? 1u : type == 5 || type == 7 ? 2u : 4u;
}
static inline void gcnr_quant(uint8_t* mem, uint32_t a, double v, uint32_t type, uint32_t scale)
{
    const float x = (float)v * ldexpf(1.0f, ((int32_t)(scale << 26)) >> 26);
    switch (type)
    {
    case 4: gcnr_sb(mem, a, x <= 0.0f ? 0u : x >= 255.0f ? 255u : (uint32_t)x); return;
    case 5: gcnr_sh(mem, a, x <= 0.0f ? 0u : x >= 65535.0f ? 65535u : (uint32_t)x); return;
    case 6: gcnr_sb(mem, a, (uint32_t)(int32_t)(x <= -128.0f ? -128 : x >= 127.0f ? 127 : (int32_t)x)); return;
    case 7: gcnr_sh(mem, a, (uint32_t)(int32_t)(x <= -32768.0f ? -32768 : x >= 32767.0f ? 32767 : (int32_t)x)); return;
    default: gcnr_sw(mem, a, gcnr_bits32((float)v)); return;
    }
}
/* psq_l: ps0 (and ps1 unless W) from memory with GQR[i]'s load type / scale */
static inline void gcnr_psq_l(uint8_t* mem, gcnr_ctx* c, int fd, uint32_t a, int w, int i)
{
    const uint32_t g = gcnr_gqr[i];
    const uint32_t type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
    c->f[fd].ps0 = gcnr_dequant(mem, a, type, scale);
    c->f[fd].ps1 = w ? 1.0 : gcnr_dequant(mem, a + gcnr_qsize(type), type, scale);
}
static inline void gcnr_psq_st(uint8_t* mem, gcnr_ctx* c, int fs, uint32_t a, int w, int i)
{
    const uint32_t g = gcnr_gqr[i];
    const uint32_t type = g & 7, scale = (g >> 8) & 0x3F;
    gcnr_quant(mem, a, c->f[fs].ps0, type, scale);
    if (!w) gcnr_quant(mem, a + gcnr_qsize(type), c->f[fs].ps1, type, scale);
}

/* ---- string loads / stores (lswi / stswi / lswx / stswx) ------------------------------------------ */
static inline void gcnr_lsw(uint8_t* mem, gcnr_ctx* c, int rd, uint32_t a, uint32_t n)
{
    int r = rd - 1;
    for (uint32_t k = 0; k < n; k++)
    {
        if ((k & 3) == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= gcnr_lbz(mem, a + k) << (24 - 8 * (k & 3));
    }
}
static inline void gcnr_stsw(uint8_t* mem, gcnr_ctx* c, int rs, uint32_t a, uint32_t n)
{
    int r = rs - 1;
    for (uint32_t k = 0; k < n; k++)
    {
        if ((k & 3) == 0) r = (r + 1) & 31;
        gcnr_sb(mem, a + k, c->r[r] >> (24 - 8 * (k & 3)));
    }
}

#ifdef __cplusplus
}
#endif
