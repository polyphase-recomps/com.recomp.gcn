/*
 * The SDK's matrix and vector library: the paired-single (PS*) versions as plain C, and
 * the C_ functions the decomp's mtx sources don't build (they are paired-single assembly).
 */
#include <dolphin.h>
#include <dolphin/mtx.h>

#define WEAK __attribute__((weak))

static float fsqrt(float x) { return __builtin_sqrtf(x); }
float sinf(float);
float cosf(float);
float tanf(float);

WEAK void PSMTXIdentity(Mtx m)
{
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) m[i][j] = i == j ? 1.0f : 0.0f;
}

WEAK void PSMTXCopy(const Mtx src, Mtx dst)
{
    int i, j;
    if (src == dst) return;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) dst[i][j] = src[i][j];
}

WEAK void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
{
    Mtx t;
    int i, j;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 4; j++)
        {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
        }
    }
    PSMTXCopy(t, ab);
}

WEAK void PSMTXConcatArray(const Mtx a, const Mtx *srcBase, Mtx *dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) PSMTXConcat(a, srcBase[i], dstBase[i]);
}

WEAK void PSMTXTranspose(const Mtx src, Mtx xPose)
{
    Mtx t;
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) t[i][j] = src[j][i];
    t[0][3] = t[1][3] = t[2][3] = 0.0f;
    PSMTXCopy(t, xPose);
}

WEAK u32 PSMTXInverse(const Mtx src, Mtx inv)
{
    Mtx m;
    f32 det;

    det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] + src[0][2] * src[1][0] * src[2][1] -
          src[2][0] * src[1][1] * src[0][2] - src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    if (det == 0.0f) return 0;
    det = 1.0f / det;
    m[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * det;
    m[0][1] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * det;
    m[0][2] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * det;
    m[1][0] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * det;
    m[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * det;
    m[1][2] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * det;
    m[2][0] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * det;
    m[2][1] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * det;
    m[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * det;
    m[0][3] = -m[0][0] * src[0][3] - m[0][1] * src[1][3] - m[0][2] * src[2][3];
    m[1][3] = -m[1][0] * src[0][3] - m[1][1] * src[1][3] - m[1][2] * src[2][3];
    m[2][3] = -m[2][0] * src[0][3] - m[2][1] * src[1][3] - m[2][2] * src[2][3];
    PSMTXCopy(m, inv);
    return 1;
}

WEAK u32 PSMTXInvXpose(const Mtx src, Mtx invX)
{
    Mtx inv;
    if (!PSMTXInverse(src, inv)) return 0;
    PSMTXTranspose(inv, invX);
    return 1;
}

WEAK void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    PSMTXIdentity(m);
    switch (axis)
    {
    case 'x': case 'X':
        m[1][1] = cosA; m[1][2] = -sinA; m[2][1] = sinA; m[2][2] = cosA;
        break;
    case 'y': case 'Y':
        m[0][0] = cosA; m[0][2] = sinA; m[2][0] = -sinA; m[2][2] = cosA;
        break;
    case 'z': case 'Z':
        m[0][0] = cosA; m[0][1] = -sinA; m[1][0] = sinA; m[1][1] = cosA;
        break;
    }
}

WEAK void PSMTXRotRad(Mtx m, char axis, f32 rad)
{
    PSMTXRotTrig(m, axis, sinf(rad), cosf(rad));
}

WEAK void PSMTXRotAxisRad(Mtx m, const Vec *axis, f32 rad)
{
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    f32 x = axis->x, y = axis->y, z = axis->z;
    f32 len = fsqrt(x * x + y * y + z * z);

    if (len != 0.0f)
    {
        x /= len; y /= len; z /= len;
    }
    m[0][0] = t * x * x + c;     m[0][1] = t * x * y - s * z; m[0][2] = t * x * z + s * y; m[0][3] = 0.0f;
    m[1][0] = t * x * y + s * z; m[1][1] = t * y * y + c;     m[1][2] = t * y * z - s * x; m[1][3] = 0.0f;
    m[2][0] = t * x * z - s * y; m[2][1] = t * y * z + s * x; m[2][2] = t * z * z + c;     m[2][3] = 0.0f;
}

WEAK void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    PSMTXIdentity(m);
    m[0][3] = xT; m[1][3] = yT; m[2][3] = zT;
}

WEAK void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT)
{
    PSMTXCopy(src, dst);
    dst[0][3] += xT; dst[1][3] += yT; dst[2][3] += zT;
}

WEAK void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    PSMTXIdentity(m);
    m[0][0] = xS; m[1][1] = yS; m[2][2] = zS;
}

WEAK void PSMTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS)
{
    int j;
    for (j = 0; j < 4; j++)
    {
        dst[0][j] = src[0][j] * xS;
        dst[1][j] = src[1][j] * yS;
        dst[2][j] = src[2][j] * zS;
    }
}

WEAK void PSMTXQuat(Mtx m, const Quaternion *q)
{
    f32 s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    f32 xs = q->x * s, ys = q->y * s, zs = q->z * s;
    f32 wx = q->w * xs, wy = q->w * ys, wz = q->w * zs;
    f32 xx = q->x * xs, xy = q->x * ys, xz = q->x * zs;
    f32 yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;

    m[0][0] = 1.0f - (yy + zz); m[0][1] = xy - wz; m[0][2] = xz + wy; m[0][3] = 0.0f;
    m[1][0] = xy + wz; m[1][1] = 1.0f - (xx + zz); m[1][2] = yz - wx; m[1][3] = 0.0f;
    m[2][0] = xz - wy; m[2][1] = yz + wx; m[2][2] = 1.0f - (xx + yy); m[2][3] = 0.0f;
}

WEAK void PSMTXReorder(const Mtx src, ROMtx dest)
{
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) dest[j][i] = src[i][j];
}

WEAK void PSMTXMultVec(const Mtx m, const Vec *src, Vec *dst)
{
    Vec t;
    t.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z + m[0][3];
    t.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z + m[1][3];
    t.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z + m[2][3];
    *dst = t;
}

WEAK void PSMTXMultVecSR(const Mtx m, const Vec *src, Vec *dst)
{
    Vec t;
    t.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z;
    t.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z;
    t.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z;
    *dst = t;
}

WEAK void PSMTXMultVecArray(const Mtx m, const Vec *srcBase, Vec *dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) PSMTXMultVec(m, &srcBase[i], &dstBase[i]);
}

WEAK void PSMTXMultVecArraySR(const Mtx m, const Vec *srcBase, Vec *dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) PSMTXMultVecSR(m, &srcBase[i], &dstBase[i]);
}

WEAK void PSVECAdd(const Vec *a, const Vec *b, Vec *ab) { ab->x = a->x + b->x; ab->y = a->y + b->y; ab->z = a->z + b->z; }
WEAK void PSVECSubtract(const Vec *a, const Vec *b, Vec *a_b) { a_b->x = a->x - b->x; a_b->y = a->y - b->y; a_b->z = a->z - b->z; }
WEAK void PSVECScale(const Vec *src, Vec *dst, f32 scale) { dst->x = src->x * scale; dst->y = src->y * scale; dst->z = src->z * scale; }
WEAK f32 PSVECDotProduct(const Vec *a, const Vec *b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
WEAK f32 PSVECSquareMag(const Vec *v) { return v->x * v->x + v->y * v->y + v->z * v->z; }
WEAK f32 PSVECMag(const Vec *v) { return fsqrt(PSVECSquareMag(v)); }

WEAK void PSVECNormalize(const Vec *src, Vec *unit)
{
    f32 m = PSVECSquareMag(src);
    m = m > 0.0f ? 1.0f / fsqrt(m) : 0.0f;
    unit->x = src->x * m; unit->y = src->y * m; unit->z = src->z * m;
}

WEAK void PSVECCrossProduct(const Vec *a, const Vec *b, Vec *axb)
{
    Vec t;
    t.x = a->y * b->z - a->z * b->y;
    t.y = a->z * b->x - a->x * b->z;
    t.z = a->x * b->y - a->y * b->x;
    *axb = t;
}

WEAK f32 PSVECSquareDistance(const Vec *a, const Vec *b)
{
    f32 x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
    return x * x + y * y + z * z;
}

WEAK f32 PSVECDistance(const Vec *a, const Vec *b) { return fsqrt(PSVECSquareDistance(a, b)); }

WEAK void C_VECReflect(const Vec *src, const Vec *normal, Vec *dst)
{
    Vec s, n;
    f32 cosA;

    s.x = -src->x; s.y = -src->y; s.z = -src->z;
    PSVECNormalize(&s, &s);
    PSVECNormalize(normal, &n);
    cosA = PSVECDotProduct(&s, &n);
    dst->x = 2.0f * n.x * cosA - s.x;
    dst->y = 2.0f * n.y * cosA - s.y;
    dst->z = 2.0f * n.z * cosA - s.z;
    PSVECNormalize(dst, dst);
}

WEAK void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 angle = fovY * 0.5f * 3.14159265f / 180.0f;
    f32 cot = 1.0f / tanf(angle);

    m[0][0] = (cot / aspect) * scaleS; m[0][1] = 0.0f; m[0][2] = -transS; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = cot * scaleT; m[1][2] = -transT; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = -1.0f; m[2][3] = 0.0f;
}

WEAK void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 tmp;

    tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp * scaleS; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = (-(r + l) * tmp) * scaleS + transS;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f; m[1][1] = 2.0f * tmp * scaleT; m[1][2] = 0.0f; m[1][3] = (-(t + b) * tmp) * scaleT + transT;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 0.0f; m[2][3] = 1.0f;
}
