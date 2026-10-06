/*
 * com.recomp.gcn override of the Dolphin SDK's OSFastCast.h: the paired-single quantised
 * loads and stores (psq_l / psq_st with the GQR settings OSInitFastCast makes) as C.
 * Quantised stores truncate towards zero and saturate to the target range, as on Gekko.
 */
#ifndef _DOLPHIN_OSFASTCAST
#define _DOLPHIN_OSFASTCAST

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_GQR_F32 0x0000
#define OS_GQR_U8 0x0004
#define OS_GQR_U16 0x0005
#define OS_GQR_S8 0x0006
#define OS_GQR_S16 0x0007

#define OS_FASTCAST_U8 2
#define OS_FASTCAST_U16 3
#define OS_FASTCAST_S8 4
#define OS_FASTCAST_S16 5

static inline void OSInitFastCast(void) {}

static inline s32 __OSf32toClamped(f32 in, s32 lo, s32 hi)
{
    if (in != in) return 0;
    if (in <= (f32)lo) return lo;
    if (in >= (f32)hi) return hi;
    return (s32)in;
}

static inline s16 __OSf32tos16(f32 inF) { return (s16)__OSf32toClamped(inF, -32768, 32767); }
static inline void OSf32tos16(f32 *f, s16 *out) { *out = __OSf32tos16(*f); }
static inline u8 __OSf32tou8(f32 inF) { return (u8)__OSf32toClamped(inF, 0, 255); }
static inline void OSf32tou8(f32 *f, u8 *out) { *out = __OSf32tou8(*f); }
static inline s8 __OSf32tos8(f32 inF) { return (s8)__OSf32toClamped(inF, -128, 127); }
static inline void OSf32tos8(f32 *f, s8 *out) { *out = __OSf32tos8(*f); }
static inline u16 __OSf32tou16(f32 inF) { return (u16)__OSf32toClamped(inF, 0, 65535); }
static inline void OSf32tou16(f32 *f, u16 *out) { *out = __OSf32tou16(*f); }

static inline float __OSs8tof32(const s8 *arg) { return (float)*arg; }
static inline void OSs8tof32(const s8 *in, float *out) { *out = __OSs8tof32(in); }
static inline float __OSs16tof32(const s16 *arg) { return (float)*arg; }
static inline void OSs16tof32(const s16 *in, float *out) { *out = __OSs16tof32(in); }
static inline float __OSu8tof32(const u8 *arg) { return (float)*arg; }
static inline void OSu8tof32(const u8 *in, float *out) { *out = __OSu8tof32(in); }
static inline float __OSu16tof32(const u16 *arg) { return (float)*arg; }
static inline void OSu16tof32(const u16 *in, float *out) { *out = __OSu16tof32(in); }

#ifdef __cplusplus
}
#endif

#endif /* _DOLPHIN_OSFASTCAST */
