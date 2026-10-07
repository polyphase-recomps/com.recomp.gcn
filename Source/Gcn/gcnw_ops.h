/*
 * Included by Runtime/tools/gcn_wasm_to_c.py into the wasm2c -impl.h after its arithmetic
 * helpers: Gekko behaviour instead of wasm traps.
 */

/* divw / divwu never trap: x / 0 is -1 for a negative x and 0 otherwise (unsigned: 0);
 * INT_MIN / -1 is 0. Remainders are what the compiler's divide-multiply-subtract gives. */
#undef DIV_S
#define DIV_S(ut, min, x, y)                                     \
  ((UNLIKELY((y) == 0))                  ? (ut)((x) < 0 ? -1 : 0) \
   : (UNLIKELY((x) == min && (y) == -1)) ? (ut)0                 \
                                         : (ut)((x) / (y)))
#undef REM_S
#define REM_S(ut, min, x, y)                                 \
  ((UNLIKELY((y) == 0))                  ? (ut)((x) < 0 ? (x) + (y) * 0 + 0 : (x)) \
   : (UNLIKELY((x) == min && (y) == -1)) ? (ut)(x)           \
                                         : (ut)((x) % (y)))
#undef DIV_U
#define DIV_U(x, y) ((UNLIKELY((y) == 0)) ? ((x) - (x)) : ((x) / (y)))
#undef REM_U
#define REM_U(x, y) ((UNLIKELY((y) == 0)) ? (x) : ((x) % (y)))

/* Byte swaps (the guest's big-endian memory; gcn_ir.late_bswaps makes them env imports):
 * the host's own instruction, inline. On a big-endian host it cancels against the swap the
 * little-endian load/store helpers do (gcnw_mem_ops.h): a plain lwz / stw. Out-of-line
 * definitions (gcnw_backend.c) are kept for any use that isn't a call. */
#if defined(__GNUC__) || defined(__clang__)
#define GCNW_BSWAP16(x) __builtin_bswap16(x)
#define GCNW_BSWAP32(x) __builtin_bswap32(x)
#define GCNW_BSWAP64(x) __builtin_bswap64(x)
#else
#include <stdlib.h>
#define GCNW_BSWAP16(x) _byteswap_ushort(x)
#define GCNW_BSWAP32(x) _byteswap_ulong(x)
#define GCNW_BSWAP64(x) _byteswap_uint64(x)
#endif
#define w2c_env_0x5F_gcn_bswap16(env, x) ((u32)GCNW_BSWAP16((uint16_t)(x)))
#define w2c_env_0x5F_gcn_bswap32(env, x) ((u32)GCNW_BSWAP32((uint32_t)(x)))
#define w2c_env_0x5F_gcn_bswap64(env, x) ((u64)GCNW_BSWAP64((uint64_t)(x)))

/* The GX write-gather pipe (gcn_ir.wgpipe_store). Consoles with the GPU passed through: a
 * draw's vertex data goes straight into the console's own pipe, a few instructions inline;
 * everything else (commands: parsed, tracked, translated) through the host
 * (gcnw_backend.c). Elsewhere the import functions do all of it. */
#if defined(GEKKO)
extern uint32_t gcn_gpu_payload;
extern uint8_t *gcn_gpu_dl_ptr, *gcn_gpu_dl_end; /* building a display list (gcn_gpu.c) */
void gcnw_wgpipe_slow(uint32_t v, int bytes);
#define GCNW_WGPIPE(bits, n, v)                                                    \
  do {                                                                             \
    const uint32_t wv_ = (uint32_t)(v);                                            \
    if (LIKELY(gcn_gpu_payload >= (n))) {                                          \
      *(volatile uint##bits##_t*)0xCC008000 = (uint##bits##_t)wv_;                 \
      gcn_gpu_payload -= (n);                                                      \
    } else if (gcn_gpu_dl_ptr && gcn_gpu_dl_end - gcn_gpu_dl_ptr >= (n)) {         \
      const uint##bits##_t dv_ = (uint##bits##_t)wv_; /* big-endian host */        \
      __builtin_memcpy(gcn_gpu_dl_ptr, &dv_, (n));                                 \
      gcn_gpu_dl_ptr += (n);                                                       \
    } else {                                                                       \
      gcnw_wgpipe_slow(wv_ & (uint32_t)(((uint64_t)1 << (bits)) - 1), (n));        \
    }                                                                              \
  } while (0)
#define w2c_env_0x5F_gcn_wgpipe8(env, v) GCNW_WGPIPE(8, 1, v)
#define w2c_env_0x5F_gcn_wgpipe16(env, v) GCNW_WGPIPE(16, 2, v)
#define w2c_env_0x5F_gcn_wgpipe32(env, v) GCNW_WGPIPE(32, 4, v)
#endif

/* Indirect calls: decomps call through function pointers whose type differs from the
 * function's (K&R pointers, tables of mixed handlers), which PowerPC tolerates (arguments
 * in registers, the callee ignores extras); only check that the slot holds a function. */
void gcnw_bad_indirect_call(uint32_t index);
#undef CHECK_CALL_INDIRECT
#define CHECK_CALL_INDIRECT(table, ft, x) \
  (LIKELY((x) < table.size && table.data[x].func) || (gcnw_bad_indirect_call(x), 0))

/* The recomp build (Runtime/recomp): the runtime's SDK code is a wasm2c module, the game is
 * recompiled PowerPC, so a function pointer the game handed over (a thread entry, a callback)
 * is a PowerPC code address, not a table slot. Such calls run the recompiled function at that
 * address (Runtime/recomp/recomp_gcn.c gcnw_guest_callback). gcn_hle_build.py writes the
 * instance argument of indirect calls as GCNW_TABLE_INSTANCE so it never indexes the table
 * with an address. Decomp builds don't define GCNW_RECOMP and are unchanged. */
#ifdef GCNW_RECOMP
void *gcnw_guest_callback(uint32_t address);
#define GCNW_TABLE_INSTANCE(table, x) (LIKELY((x) < (table).size) ? (table).data[x].module_instance : (void *)0)
#undef CALL_INDIRECT
#define CALL_INDIRECT(table, t, ft, x, ...)                                              \
  ((LIKELY((x) < table.size && table.data[x].func)) ? ((t)table.data[x].func)(__VA_ARGS__) \
                                                     : ((t)gcnw_guest_callback(x))(__VA_ARGS__))
#endif
