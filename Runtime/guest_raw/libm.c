/*
 * Math functions games expect from the C library, where the decomp's own library doesn't
 * provide them. They touch no memory, so they are compiled for wasm directly.
 */
#define WEAK __attribute__((weak))

static const double PI = 3.14159265358979323846;

/* sin and cos on [-pi/4, pi/4] (Taylor to x^13 / x^12: below float precision) */
static double ksin(double x)
{
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}

static double kcos(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
}

static double dsin(double x)
{
    double q = __builtin_floor(x / (PI / 2) + 0.5);
    double r = x - q * (PI / 2);
    long long n = (long long)q & 3;

    switch (n)
    {
    case 0: return ksin(r);
    case 1: return kcos(r);
    case 2: return -ksin(r);
    default: return -kcos(r);
    }
}

static double dcos(double x)
{
    return dsin(x + PI / 2);
}

WEAK double sin(double x) { return dsin(x); }
WEAK double cos(double x) { return dcos(x); }
WEAK double tan(double x) { return dsin(x) / dcos(x); }
WEAK float sinf(float x) { return (float)dsin(x); }
WEAK float cosf(float x) { return (float)dcos(x); }
WEAK float tanf(float x) { return (float)(dsin(x) / dcos(x)); }
WEAK double sqrt(double x) { return __builtin_sqrt(x); }
WEAK float sqrtf(float x) { return __builtin_sqrtf(x); }
WEAK double floor(double x) { return __builtin_floor(x); }
WEAK float floorf(float x) { return __builtin_floorf(x); }
WEAK double ceil(double x) { return __builtin_ceil(x); }
WEAK float ceilf(float x) { return __builtin_ceilf(x); }
WEAK double fabs(double x) { return __builtin_fabs(x); }
WEAK float fabsf(float x) { return __builtin_fabsf(x); }
