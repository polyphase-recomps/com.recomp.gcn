/*
 * Included by Runtime/tools/gcn_wasm_to_c.py into the wasm2c -impl.h, just before the load and
 * store helpers are instantiated: memory accesses follow gcnw.h (masked addresses,
 * little-endian buffer, no bounds checks - the mask keeps every access inside it).
 */
#include "gcnw.h"

#undef MEM_ADDR
#undef MEM_ADDR_MEMOP
#if defined(GEKKO)
/* consoles: the buffer's fixed address and the CPU's memory map do the folding (gcnw.h) */
#define MEM_ADDR(mem, addr, n) ((uint8_t *)(uintptr_t)(GCNW_WRAP + (u32)(addr)))
#define MEM_ADDR_MEMOP(mem, addr, n) MEM_ADDR(mem, addr, n)
#else
#define MEM_ADDR(mem, addr, n) (&(mem)->data[GCNW_OFFSET((u32)(addr))])
/* loads and stores: through the function's copy of the base (the memory never grows), so a
 * store doesn't make the compiler fetch mem->data again for the next access */
#define MEM_ADDR_MEMOP(mem, addr, n) (&wasm_rt_local_memory_base[GCNW_OFFSET((u32)(addr))])
#endif
#undef RANGE_CHECK
#define RANGE_CHECK(mem, offset, len)

#ifdef GCNW_WATCH_WRITES
/* debugging build (-DGCNW_WATCH_WRITES): the host hears of every store into
 * [gcnw_watch_lo, gcnw_watch_hi) */
extern uint32_t gcnw_watch_lo, gcnw_watch_hi;
void gcnw_watch_hit(uint32_t addr, uint32_t size, uint64_t value);
#define GCNW_WATCH(addr, n, bits)                                       \
  do {                                                                  \
    uint32_t a_ = (uint32_t)(addr) & 0x01FFFFFFu;                       \
    if (a_ < gcnw_watch_hi && a_ + (n) > gcnw_watch_lo)                 \
      gcnw_watch_hit(a_, (uint32_t)(n), (bits));                        \
  } while (0)
#undef RANGE_CHECK
#define RANGE_CHECK(mem, offset, len) GCNW_WATCH(offset, (uint32_t)(len), 0)
#if !GCNW_BIG_ENDIAN
#undef DEFINE_STORE
#define DEFINE_STORE(name, t1, t2)                                     \
  static inline void name##_unchecked(                                 \
      uint8_t* const wasm_rt_local_memory_base, wasm_rt_memory_t* mem, \
      u64 addr, t2 value) {                                            \
    t1 wrapped = (t1)value;                                            \
    uint64_t bits_ = 0;                                                \
    wasm_rt_memcpy(&bits_, &wrapped, sizeof(t1));                      \
    GCNW_WATCH(addr, sizeof(t1), bits_);                               \
    wasm_rt_memcpy(MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), &wrapped,    \
                   sizeof(t1));                                        \
  }                                                                    \
  DEF_MEM_CHECKS1(name, _, t1, , void, t2)
#endif
#endif
#undef MEMCHECK_DEFAULT32
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t)
#undef MEMCHECK_GENERAL
#define MEMCHECK_GENERAL(mem, a, t)
#undef LOAD_DATA
#define LOAD_DATA(m, o, i, s) wasm_rt_memcpy(MEM_ADDR(&(m), o, s), i, s)

#if GCNW_BIG_ENDIAN
static inline f32 gcnw_swap_f32(f32 v)
{
    u32 u;
    wasm_rt_memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    wasm_rt_memcpy(&v, &u, 4);
    return v;
}

static inline f64 gcnw_swap_f64(f64 v)
{
    u64 u;
    wasm_rt_memcpy(&u, &v, 8);
    u = __builtin_bswap64(u);
    wasm_rt_memcpy(&v, &u, 8);
    return v;
}

/* little-endian <-> host for every type wasm2c loads and stores */
#define GCNW_LE(v)                                    \
    _Generic((v),                                     \
        u8: (v), s8: (v),                             \
        u16: (u16)__builtin_bswap16((u16)(v)),        \
        s16: (s16)__builtin_bswap16((u16)(v)),        \
        u32: (u32)__builtin_bswap32((u32)(v)),        \
        s32: (s32)__builtin_bswap32((u32)(v)),        \
        u64: (u64)__builtin_bswap64((u64)(v)),        \
        s64: (s64)__builtin_bswap64((u64)(v)),        \
        f32: gcnw_swap_f32((f32)(v)),                 \
        f64: gcnw_swap_f64((f64)(v)))

#undef DEFINE_LOAD
#define DEFINE_LOAD(name, t1, t2, t3, force_read)                             \
  static inline t3 name##_unchecked(uint8_t* const wasm_rt_local_memory_base, \
                                    wasm_rt_memory_t* mem, u64 addr) {        \
    t1 result;                                                                \
    wasm_rt_memcpy(&result, MEM_ADDR_MEMOP(mem, addr, sizeof(t1)),            \
                   sizeof(t1));                                               \
    result = GCNW_LE(result);                                                 \
    return (t3)(t2)result;                                                    \
  }                                                                           \
  DEF_MEM_CHECKS0(name, _, t1, return, t3)

#undef DEFINE_STORE
#define DEFINE_STORE(name, t1, t2)                                     \
  static inline void name##_unchecked(                                 \
      uint8_t* const wasm_rt_local_memory_base, wasm_rt_memory_t* mem, \
      u64 addr, t2 value) {                                            \
    t1 wrapped = (t1)value;                                            \
    wrapped = GCNW_LE(wrapped);                                        \
    wasm_rt_memcpy(MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), &wrapped,    \
                   sizeof(t1));                                        \
  }                                                                    \
  DEF_MEM_CHECKS1(name, _, t1, , void, t2)
#endif
