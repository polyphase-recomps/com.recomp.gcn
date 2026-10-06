/*
 * Force-included into every guest translation unit (game code, its libraries and the
 * runtime's guest side). Guest code is compiled by clang for a 32-bit big-endian target
 * and then retargeted (tools/gcn_ir.py), so this only has to bridge the Metrowerks dialect:
 * the decomp's headers already have non-__MWERKS__ fallbacks for most of it.
 */
#ifndef GCN_PRELUDE_H
#define GCN_PRELUDE_H

#define GCN_PORT 1
#ifndef PORT
#define PORT 1
#endif

/* Metrowerks storage specifiers: sections and alignment of code are meaningless here. */
#define __declspec(x)
/* Register hints: Metrowerks lets code take the address of a register variable. */
#define register

/* Gekko instructions MWCC exposes as intrinsics; the decomp declares some of them as
 * functions (math_api.h), so they are functions in guest/intrinsics.c as well. */
int __cntlzw(unsigned int);
double __fabs(double);
float __fabsf(float);
double __frsqrte(double);
float __fres(float);
double __fnabs(double);
double __fsel(double, double, double);
float __fsels(float, float, float);
#define __lhbrx(base, off) ((unsigned short)__builtin_bswap16(*(unsigned short *)((char *)(base) + (off))))
#define __lwbrx(base, off) ((unsigned int)__builtin_bswap32(*(unsigned int *)((char *)(base) + (off))))
#define __sthbrx(v, base, off) (*(unsigned short *)((char *)(base) + (off)) = __builtin_bswap16((unsigned short)(v)))
#define __stwbrx(v, base, off) (*(unsigned int *)((char *)(base) + (off)) = __builtin_bswap32((unsigned int)(v)))
#define __dcbz(base, off) __builtin_memset((char *)(base) + (off), 0, 32)
#define __dcbf(base, off) ((void)0)
#define __dcbst(base, off) ((void)0)
#define __dcbi(base, off) ((void)0)
#define __icbi(base, off) ((void)0)
#define __sync() ((void)0)
#define __isync() ((void)0)

#endif /* GCN_PRELUDE_H */
