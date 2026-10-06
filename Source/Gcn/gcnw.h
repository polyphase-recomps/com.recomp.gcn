/*
 * Guest memory of a GameCube game built by com.recomp.gcn.
 *
 * The guest module is linked so that addresses have their GameCube meaning: main RAM at
 * 0x80000000 (24 MB, the module's data, stack and the game's arena), the locked cache at
 * 0xE0000000. GCNW_OFFSET folds them onto one buffer the way the console mirrors RAM:
 *
 *   0x00000000-0x01FFFFFF  main RAM (0x80xxxxxx cached, 0xC0xxxxxx uncached, 0x0xxxxxxx
 *                          physical: the low 25 bits), 24 MB used
 *   0x02000000-0x02003FFF  locked cache (0xE00xxxxx)
 *
 * Memory holds GameCube (big-endian) data: the guest code swaps on its own (tools/gcn_ir.py),
 * so the buffer is a plain wasm memory and host code reads it with the gcnw_be* helpers.
 * Hardware registers (0xCC000000...) never reach the buffer: the guest's volatile
 * accesses go to the host (__gcn_vload / __gcn_vstore, gcnw_backend.c).
 */
#ifndef GCNW_H
#define GCNW_H

#include <stdint.h>

#define GCNW_RAM_MASK 0x01FFFFFFu
#if defined(GEKKO)
/* Wii / GameCube hosts: exactly the console's 24 MB (memory is short there); the
 * locked cache window follows it. Addresses past 24 MB are bad on the console too. */
#define GCNW_LC_OFFSET 0x01800000u
/* The buffer sits at a fixed MEM2 address (gcnw_rt.c reserves it below libogc's MEM2 arena),
 * so the generated code reaches guest address a at GCNW_WRAP + a with 32-bit wrap-around and
 * no masking: 0x80xxxxxx -> 0x90xxxxxx (MEM2, cached), 0xC0xxxxxx -> 0xD0xxxxxx (the same
 * bytes uncached, as on the console); data BATs (gcnw_rt.c) map the other views:
 * physical 0x0xxxxxxx, the EFB 0xC8xxxxxx and the locked cache 0xE000xxxx. */
#define GCNW_GUEST_BASE 0x90000000u
#define GCNW_WRAP (GCNW_GUEST_BASE - 0x80000000u)
#elif defined(__3DS__)
/* 3DS: the console's 24 MB too (an application gets about 64 MB on the original model) */
#define GCNW_LC_OFFSET 0x01800000u
#else
#define GCNW_LC_OFFSET 0x02000000u
#endif
#define GCNW_LC_MASK 0x00003FFFu
/* 0xE... (locked cache) -> its window; everything else -> RAM */
#define GCNW_OFFSET(a)                                                                                     \
    ((((uint32_t)(a) >> 28) == 0xEu) ? (GCNW_LC_OFFSET | ((uint32_t)(a) & GCNW_LC_MASK)) \
                                     : ((uint32_t)(a) & GCNW_RAM_MASK))
/* room for an access of up to 16 bytes at the last address */
#define GCNW_MEM_BYTES (GCNW_LC_OFFSET + GCNW_LC_MASK + 1u + 16u)

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GCNW_BIG_ENDIAN 1
#else
#define GCNW_BIG_ENDIAN 0
#endif

#if defined(GEKKO) || defined(__3DS__)
#include <setjmp.h>
#define sigsetjmp(b, s) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)
#endif

#include <stddef.h>
/* Large zeroed buffers (guest RAM, ARAM): on Wii from MEM2, where the GPU can read them
 * (the game's textures and vertex arrays go to the real GPU there). gcnw_rt.c. */
void *gcnw_big_alloc(size_t bytes);
void gcnw_big_free(void *p);
/* The data cache over a host buffer the GPU reads or the CPU filled behind the game's
 * back (no-op where CPU and GPU share caches). */
void gcnw_dcache_flush(void *p, size_t bytes);

#endif /* GCNW_H */
