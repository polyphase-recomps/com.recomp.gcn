/*
 * AX effects as MusyX links them (the CPU half of the aux buses): the standard reverb.
 * The decomp's callback is Gekko assembly (reverb_std_callback.c, HandleReverb2); this is
 * the same computation in C, and ReverbSTDCreate as the decomp's (allocating with
 * salMalloc, the sound engine's hook).
 */
#include <dolphin.h>
#include <dolphin/axfx.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

void *salMalloc(u32 size);

static const s32 sDelayLengths[4] = {1789, 1999, 433, 149}; /* combs, all-passes */

/* 10^x for the comb feedback (no libm here) */
static f32 pow10f(f32 x)
{
    f32 y = x * 2.30258509f, r, term, sum, scale = 1.0f;
    int k, i;

    k = (int)(y / 0.693147181f);
    if (y < 0.0f && (f32)k * 0.693147181f > y) k--;
    r = y - (f32)k * 0.693147181f;
    for (sum = term = 1.0f, i = 1; i < 10; i++)
    {
        term *= r / (f32)i;
        sum += term;
    }
    for (; k > 0; k--) scale *= 2.0f;
    for (; k < 0; k++) scale *= 0.5f;
    return sum * scale;
}

static void dl_set_delay(AXFX_REVSTD_DELAYLINE *dl, s32 lag)
{
    dl->outPoint = dl->inPoint - lag * 4;
    while (dl->outPoint < 0) dl->outPoint += dl->length;
}

static void dl_create(AXFX_REVSTD_DELAYLINE *dl, s32 len)
{
    s32 i;

    dl->length = len * 4;
    dl->inputs = salMalloc(len * 4);
    for (i = 0; i < len; i++) dl->inputs[i] = 0.0f;
    dl->lastOutput = 0.0f;
    dl_set_delay(dl, len >> 1);
    dl->inPoint = 0;
    dl->outPoint = 0;
}

int ReverbSTDCreate(AXFX_REVSTD_WORK *rv, f32 coloration, f32 time, f32 mix, f32 damping, f32 predelay)
{
    u8 i, k;
    u32 n;
    f32 timeFactor;

    if (coloration < 0.0f || coloration > 1.0f || time < 0.01f || time > 10.0f || mix < 0.0f || mix > 1.0f ||
        damping < 0.0f || damping > 1.0f || predelay < 0.0f || predelay > 0.1f)
        return 0;
    for (n = 0; n < sizeof(*rv); n++) ((u8 *)rv)[n] = 0;
    timeFactor = 32000.0f * time;
    for (k = 0; k < 3; k++)
    {
        for (i = 0; i < 2; i++)
        {
            dl_create(&rv->C[i + k * 2], sDelayLengths[i] + 2);
            dl_set_delay(&rv->C[i + k * 2], sDelayLengths[i]);
            rv->combCoef[i + k * 2] = pow10f((sDelayLengths[i] * -3) / timeFactor);
        }
        for (i = 0; i < 2; i++)
        {
            dl_create(&rv->AP[i + k * 2], sDelayLengths[i + 2] + 2);
            dl_set_delay(&rv->AP[i + k * 2], sDelayLengths[i + 2]);
        }
        rv->lpLastout[k] = 0.0f;
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping < 0.05f ? 0.05f : damping;
    rv->damping = 1.0f - (0.05f + 0.8f * rv->damping);
    if (predelay != 0.0f)
    {
        rv->preDelayTime = (s32)(32000.0f * predelay);
        for (i = 0; i < 3; i++)
        {
            rv->preDelayLine[i] = salMalloc(rv->preDelayTime * 4);
            for (n = 0; n < (u32)rv->preDelayTime; n++) rv->preDelayLine[i][n] = 0.0f;
            rv->preDelayPtr[i] = rv->preDelayLine[i];
        }
    }
    else
    {
        rv->preDelayTime = 0;
        for (i = 0; i < 3; i++) rv->preDelayPtr[i] = rv->preDelayLine[i] = 0;
    }
    return 1;
}

/* per channel (left, right, surround follow each other, 160 samples each): pre-delay, two
 * parallel combs, an all-pass, a one-pole low-pass, a second all-pass; the result is
 * 0.6 * (wet * mix + dry * (1 - mix)) */
static void handle_reverb(s32 *sptr, AXFX_REVSTD_WORK *rv)
{
    f32 ap = rv->allPassCoeff, damping = rv->damping;
    f32 wet = rv->level * 0.6f, dry = 0.6f - wet;
    int k, i;

    for (k = 0; k < 3; k++, sptr += 160)
    {
        AXFX_REVSTD_DELAYLINE *c0 = &rv->C[k * 2], *c1 = &rv->C[k * 2 + 1];
        AXFX_REVSTD_DELAYLINE *a0 = &rv->AP[k * 2], *a1 = &rv->AP[k * 2 + 1];
        f32 cc0 = rv->combCoef[k * 2], cc1 = rv->combCoef[k * 2 + 1];
        f32 lp = rv->lpLastout[k];
        f32 c0out = c0->lastOutput, c1out = c1->lastOutput, a0out = a0->lastOutput, a1out = a1->lastOutput;
        f32 *pre = rv->preDelayPtr[k], *preStart = rv->preDelayLine[k];
        f32 *preEnd = preStart + rv->preDelayTime - 1;
        s32 c0in = c0->inPoint, c0o = c0->outPoint, c1in = c1->inPoint, c1o = c1->outPoint;
        s32 a0in = a0->inPoint, a0o = a0->outPoint, a1in = a1->inPoint, a1o = a1->outPoint;

        for (i = 0; i < 160; i++)
        {
            f32 in = (f32)sptr[i], x = in, sum, t, v;

            if (rv->preDelayTime != 0)
            {
                x = *pre;
                *pre++ = in;
                if (pre == preEnd) pre = preStart;
            }
            /* combs */
            c0->inputs[c0in >> 2] = cc0 * c0out + x;
            c1->inputs[c1in >> 2] = cc1 * c1out + x;
            c0out = c0->inputs[c0o >> 2];
            c1out = c1->inputs[c1o >> 2];
            sum = c0out + c1out;
            if ((c0in += 4) == c0->length) c0in = 0;
            if ((c0o += 4) == c0->length) c0o = 0;
            if ((c1in += 4) == c1->length) c1in = 0;
            if ((c1o += 4) == c1->length) c1o = 0;
            /* all-pass */
            v = ap * a0out + sum;
            a0->inputs[a0in >> 2] = v;
            t = a0out - ap * v;
            a0out = a0->inputs[a0o >> 2];
            if ((a0in += 4) == a0->length) a0in = 0;
            if ((a0o += 4) == a0->length) a0o = 0;
            /* low-pass */
            lp = damping * lp + t * 0.3f;
            /* all-pass */
            v = ap * a1out + lp;
            a1->inputs[a1in >> 2] = v;
            t = a1out - ap * v;
            a1out = a1->inputs[a1o >> 2];
            if ((a1in += 4) == a1->length) a1in = 0;
            if ((a1o += 4) == a1->length) a1o = 0;
            sptr[i] = (s32)(wet * t + dry * in);
        }
        c0->inPoint = c0in;
        c0->outPoint = c0o;
        c1->inPoint = c1in;
        c1->outPoint = c1o;
        a0->inPoint = a0in;
        a0->outPoint = a0o;
        a1->inPoint = a1in;
        a1->outPoint = a1o;
        c0->lastOutput = c0out;
        c1->lastOutput = c1out;
        a0->lastOutput = a0out;
        a1->lastOutput = a1out;
        rv->lpLastout[k] = lp;
        rv->preDelayPtr[k] = pre;
    }
}

void ReverbSTDCallback(s32 *left, s32 *right, s32 *surround, AXFX_REVSTD_WORK *rv)
{
    if (!rv->C[0].inputs) return; /* not created */
    handle_reverb(left, rv);
}
