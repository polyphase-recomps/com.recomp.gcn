/*
 * Gekko instructions that Metrowerks C exposes as intrinsic functions (declared as
 * functions by the decomp's headers, so defined as functions here).
 */
#define WEAK __attribute__((weak))

WEAK int __cntlzw(unsigned int x)
{
    return x ? __builtin_clz(x) : 32;
}

WEAK double __fabs(double x)
{
    return __builtin_fabs(x);
}

WEAK float __fabsf(float x)
{
    return __builtin_fabsf(x);
}

/* frsqrte is an estimate (about 12 bits); games refine it with Newton steps, so the exact
 * value only makes results more precise */
WEAK double __frsqrte(double x)
{
    return 1.0 / __builtin_sqrt(x);
}

WEAK float __fres(float x)
{
    return 1.0f / x;
}

WEAK double __frsp(double x)
{
    return (float)x;
}

WEAK double __fnabs(double x)
{
    return -__builtin_fabs(x);
}

WEAK double __fsel(double a, double b, double c)
{
    return a >= 0.0 ? b : c;
}

WEAK float __fsels(float a, float b, float c)
{
    return a >= 0.0f ? b : c;
}
