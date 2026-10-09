/*
 * JAudio's DSP program (Pikmin; the "Zelda" family of microcodes), high level: what it does
 * for the CPU side (jaudio/dspinterface.c, dspproc.c, dspbuf.c) every sub-frame. audio.c
 * (GCN_DSP_JAUDIO) speaks the mailbox protocol and calls in here:
 *   setup table (0x81): the voice table (64 parameter blocks of 0x180 bytes in main RAM), the
 *     resampling filter (64 phases x 4 taps), the ADPCM coefficients, the 4 effect lines;
 *   sync frame (0x82): the frame's output buffers (two halves of 560 samples) and the mixer
 *     level (4096 = 1.0);
 *   release halt (a lone mail 0): one sub-frame, 80 samples, then the DSP interrupt.
 *
 * Per sub-frame, every enabled voice reads its sample (AFC 4-bit / 2-bit ADPCM, PCM8, PCM16
 * from ARAM, or PCM16 from a ring buffer in main RAM for streams), resamples it by its ratio
 * (0x1000 = 1.0) through the game's filter table, filters it (variable FIR) and adds it to up
 * to six buses with volume ramps (current -> target over the sub-frame). The CPU reads back
 * `done` (the sample ended, or a requested stop faded out), and for streams the position and
 * the samples left. Effect lines delay a bus by their circular buffer (one sub-frame per
 * slot), filter it and send it on. The front left / right buses are the output.
 *
 * The semantics come from what the CPU side writes and reads (the microcode was not
 * disassembled). Approximations: the biquad and distance filters, the Dolby fields and the
 * oscillators (synthesised waves) are not done; surround buses are folded into the front.
 */
#include <dolphin.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

#define JA_SUB 80     /* samples per sub-frame */
#define JA_VOICES 64
#define JA_FX 4

/* the voice parameter block (dspinterface.h DSPchannel_), big-endian in main RAM */
typedef struct JaMix
{
    u16 id, target, current, level;
} JaMix;

typedef struct JaVPB
{
    u16 enabled;          /* 00 */
    u16 done;             /* 02 */
    u16 ratio;            /* 04: 0x1000 = 1.0 */
    u16 _06;
    u16 reset;            /* 08: set by the CPU when the voice (re)starts */
    u16 endReached;       /* 0A */
    u16 useConstant;      /* 0C: paused */
    u16 keepCount;        /* 0E */
    JaMix mix[6];         /* 10 */
    u8 _40[0x10];
    u16 dolby[8];         /* 50 */
    u16 posFrac;          /* 60 */
    u16 _62;
    u16 afcDecoded;       /* 64 */
    s16 constant;         /* 66 */
    u32 position;         /* 68: streams: the ring position << 16 */
    u16 _6C, _6E;
    u32 aramAddr;         /* 70 */
    u32 remaining;        /* 74: streams: samples left to play */
    s16 resample[4];      /* 78 */
    u16 firHistory[20];   /* 80 */
    s16 biquadHistory[4]; /* A8 */
    u16 afcRemain[16];    /* B0 */
    s16 lowPassHistory[2];/* D0 */
    u8 _D4[0x2C];
    u16 format;           /* 100: 0-3 oscillators, 5 AFC 2-bit, 8 PCM8, 9 AFC 4-bit, 0x10 PCM16, 0x21 RAM PCM16 */
    u16 looping;          /* 102 */
    s16 loopYn1, loopYn2; /* 104 */
    s16 filterMode;       /* 108: 0x20 biquad, low 5 bits FIR length */
    u16 endRequested;     /* 10A */
    u32 age;              /* 10C */
    u32 loopStart;        /* 110: loop start sample (streams: the ring's address) */
    u32 end;              /* 114: end / loop end sample (streams: the ring's size << 16) */
    u32 base;             /* 118: the sample's address (ARAM; streams: main RAM) */
    u32 count;            /* 11C: samples */
    s16 fir[20];          /* 120 */
    s16 biquad[4];        /* 148 */
    s16 lowPass;          /* 150 */
    u8 _152[0x2E];
} JaVPB;

typedef char ja_vpb_size[sizeof(JaVPB) == 0x180 ? 1 : -1];

typedef struct JaFX
{
    u16 enabled, size; /* size: sub-frames of delay */
    u32 buffer;        /* main RAM, size * 80 samples */
    struct { u16 id; s16 volume; } dest[2];
    s16 coef[8];
} JaFX;

/* the decoder's own state per voice (the DSP keeps it in the parameter block's private part) */
typedef struct JaState
{
    u32 next;       /* index of the next sample to read */
    u32 frac;       /* 16.16 position between window[1] and window[2] */
    s16 window[4];  /* the last samples read, for the 4-tap resampler */
    s16 block[16];  /* decoded ADPCM block */
    s32 blockIndex; /* which block `block` holds, -1 none */
    s16 hist1, hist2;
    s16 fir[24];    /* FIR input history */
    u8 ended;
    u8 cache[64];   /* ARAM bytes */
    u32 cacheBase;
} JaState;

static JaVPB *sVoices;
static u32 sVoiceCount;
static const s16 *sResample; /* 64 phases x 4 taps */
static const s16 *sCoefs;    /* 16 pairs */
static JaFX *sFX;
static s16 *sOutR, *sOutL; /* the frame's halves (see ja_dsp_frame) */
static u32 sSubframes, sSubframe, sLevel = 0x1000;
static JaState sState[JA_VOICES];

/* buses, by the DSP memory address the CPU names them with */
static const u16 sBusIds[] = {0x0D00, 0x0D60, 0x0DC0, 0x0E20, 0x0E80, 0x0EE0, 0x0CA0, 0x0F40, 0x0FA0, 0x0B00, 0x09A0};
#define JA_BUSES (sizeof(sBusIds) / sizeof(sBusIds[0]))
static s32 sBus[JA_BUSES][JA_SUB];

static int bus_index(u16 id)
{
    int i;
    for (i = 0; i < (int)JA_BUSES; i++)
    {
        if (sBusIds[i] == id) return i;
    }
    return -1;
}

static inline s32 clamp16(s32 v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

void gcn_jaudio_dsp_setup(u32 count, u32 voices, u32 resample, u32 coefs, u32 fx)
{
    int i;
    sVoiceCount = count > JA_VOICES ? JA_VOICES : count;
    sVoices = (JaVPB *)voices;
    sResample = (const s16 *)resample;
    sCoefs = (const s16 *)coefs;
    sFX = (JaFX *)fx;
    for (i = 0; i < JA_VOICES; i++) sState[i].cacheBase = 0xFFFFFFFFu;
}

/* A frame: `start` and `end` are the halves of the CPU's output buffer. The CPU's AI mix
 * (aictrl.c MixExtraTrack) puts the second half first in each stereo pair: the console's AI
 * takes right first, the runtime left first, so the left bus goes to the second half. */
void gcn_jaudio_dsp_frame(u32 subframes, u32 level, u32 start, u32 end)
{
    int i;
    sSubframes = subframes;
    sSubframe = 0;
    sLevel = level;
    sOutR = (s16 *)start;
    sOutL = (s16 *)end;
    for (i = 0; i < JA_VOICES; i++) sState[i].cacheBase = 0xFFFFFFFFu; /* ARAM may have changed */
}

/* ---- sample sources ------------------------------------------------------------------------ */
static u32 aram_byte(JaState *s, u32 a)
{
    if (a - s->cacheBase >= sizeof(s->cache))
    {
        s->cacheBase = a & ~31u;
        gcn_host_aram(1, s->cache, s->cacheBase, sizeof(s->cache));
    }
    return s->cache[a - s->cacheBase];
}

static void afc_block(JaState *s, const JaVPB *v, s32 index)
{
    const int two = v->format == 5;
    const u32 bytes = two ? 5 : 9;
    const u32 addr = v->base + (u32)index * bytes;
    const u32 head = aram_byte(s, addr);
    const s32 scale = 1 << (head >> 4);
    const s32 c1 = sCoefs[(head & 15) * 2], c2 = sCoefs[(head & 15) * 2 + 1];
    s32 h1 = s->hist1, h2 = s->hist2;
    int i;

    for (i = 0; i < 16; i++)
    {
        s32 n, x;
        if (two)
        {
            n = (aram_byte(s, addr + 1 + i / 4) >> (6 - (i & 3) * 2)) & 3;
            n = n >= 2 ? n - 4 : n;
            x = (n * scale) << 13;
        }
        else
        {
            n = (aram_byte(s, addr + 1 + i / 2) >> ((i & 1) ? 0 : 4)) & 15;
            n = n >= 8 ? n - 16 : n;
            x = (n * scale) << 11;
        }
        x = clamp16((x + c1 * h1 + c2 * h2) >> 11);
        s->block[i] = (s16)x;
        h2 = h1;
        h1 = x;
    }
    s->hist1 = (s16)h1;
    s->hist2 = (s16)h2;
    s->blockIndex = index;
}

/* the next sample of the voice's source; 0 once it ended */
static s32 next_sample(JaState *s, JaVPB *v)
{
    s32 x = 0;
    u32 n;

    if (s->ended) return 0;
    if (v->format == 0x21)
    {
        const u32 ring = v->end >> 16;
        if (!v->remaining)
        {
            s->ended = 1;
            return 0;
        }
        if (ring && s->next >= ring) s->next = 0;
        x = ((const s16 *)v->loopStart)[s->next++];
        v->remaining--;
        return x;
    }
    n = s->next;
    if (n >= v->end)
    {
        if (v->looping && v->loopStart < v->end)
        {
            n = v->loopStart;
            s->hist1 = v->loopYn1;
            s->hist2 = v->loopYn2;
            s->blockIndex = -1;
            /* the loop block decodes from the loop's history */
            if (v->format == 9 || v->format == 5)
            {
                afc_block(s, v, (s32)(n / 16));
            }
        }
        else
        {
            s->ended = 1;
            return 0;
        }
    }
    switch (v->format)
    {
    case 9:
    case 5:
        if (s->blockIndex != (s32)(n / 16)) afc_block(s, v, (s32)(n / 16));
        x = s->block[n & 15];
        break;
    case 8:
        x = (s32)(s8)aram_byte(s, v->base + n) << 8;
        break;
    case 0x10:
        x = (s16)((aram_byte(s, v->base + n * 2) << 8) | aram_byte(s, v->base + n * 2 + 1));
        break;
    default:
        x = 0; /* oscillators: not done */
        break;
    }
    s->next = n + 1;
    return x;
}

static void voice_reset(JaState *s, JaVPB *v)
{
    int i;
    s->next = 0;
    s->frac = 0;
    s->blockIndex = -1;
    s->hist1 = s->hist2 = 0;
    s->ended = 0;
    s->cacheBase = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) s->window[i] = 0;
    for (i = 0; i < 24; i++) s->fir[i] = 0;
    if (v->format == 0x21) s->next = v->position >> 16;
    v->reset = 0;
    v->endReached = 0;
}

/* ---- one voice, one sub-frame -------------------------------------------------------------- */
static void voice_run(int index, JaVPB *v)
{
    JaState *s = &sState[index];
    s32 out[JA_SUB];
    const u32 step = (u32)v->ratio << 4; /* 16.16 */
    const int firLen = v->filterMode & 31;
    int i, m;

    if (v->reset) voice_reset(s, v);

    if (v->useConstant)
    {
        for (i = 0; i < JA_SUB; i++) out[i] = v->constant;
    }
    else
    {
        for (i = 0; i < JA_SUB; i++)
        {
            const s16 *t = sResample ? sResample + ((s->frac >> 10) & 63) * 4 : 0;
            s32 y;
            if (t)
            {
                y = (t[0] * s->window[0] + t[1] * s->window[1] + t[2] * s->window[2] + t[3] * s->window[3]) >> 15;
            }
            else
            {
                y = s->window[1] + (((s->window[2] - s->window[1]) * (s32)(s->frac >> 1)) >> 15);
            }
            out[i] = clamp16(y);
            s->frac += step;
            while (s->frac >= 0x10000u)
            {
                s->frac -= 0x10000u;
                s->window[0] = s->window[1];
                s->window[1] = s->window[2];
                s->window[2] = s->window[3];
                s->window[3] = (s16)next_sample(s, v);
            }
        }
        v->constant = (s16)out[JA_SUB - 1];
    }

    /* variable FIR (a single 0x7FFF tap is the identity) */
    if (firLen > 1 || (firLen == 1 && v->fir[0] != 0x7FFF))
    {
        const int len = firLen > 20 ? 20 : firLen;
        for (i = 0; i < JA_SUB; i++)
        {
            s32 acc = 0;
            int k;
            for (k = 23; k > 0; k--) s->fir[k] = s->fir[k - 1];
            s->fir[0] = (s16)out[i];
            for (k = 0; k < len; k++) acc += v->fir[k] * s->fir[k];
            out[i] = clamp16(acc >> 15);
        }
    }

    /* to the buses, the volume ramping to its target over the sub-frame; a requested stop
     * fades out and ends the voice */
    for (m = 0; m < 6; m++)
    {
        JaMix *x = &v->mix[m];
        const int b = x->id ? bus_index(x->id) : -1;
        const s32 from = (s16)x->current;
        const s32 to = v->endRequested ? 0 : (s16)x->target;
        if (b >= 0 && (from || to))
        {
            s32 *bus = sBus[b];
            for (i = 0; i < JA_SUB; i++)
            {
                const s32 vol = from + ((to - from) * (i + 1)) / JA_SUB;
                bus[i] += (out[i] * vol) >> 15;
            }
        }
        x->current = (u16)to;
    }

    if (v->format == 0x21) v->position = s->next << 16;
    if (s->ended) v->endReached = 1;
    if (v->endRequested || s->ended) v->done = 1;
}

/* ---- effect lines -------------------------------------------------------------------------- */
/* line i takes bus 2 + i (0x0DC0, 0x0E20, 0x0E80, 0x0EE0) into its circular buffer and sends
 * what went in `size` sub-frames ago, filtered, to its destinations */
static u32 sFxSlot[JA_FX];
static s16 sFxPrev[JA_FX][8];

static void fx_run(void)
{
    int f;
    if (!sFX) return;
    for (f = 0; f < JA_FX; f++)
    {
        const JaFX *fx = &sFX[f];
        s16 *ring = (s16 *)fx->buffer;
        s32 *in = sBus[2 + f];
        s16 delayed[JA_SUB];
        int i, d;

        if (!fx->enabled || !fx->buffer || !fx->size) continue;
        if (sFxSlot[f] >= fx->size) sFxSlot[f] = 0;
        ring += sFxSlot[f] * JA_SUB;
        for (i = 0; i < JA_SUB; i++)
        {
            delayed[i] = ring[i];
            ring[i] = (s16)clamp16(in[i]);
        }
        sFxSlot[f]++;
        for (i = 0; i < JA_SUB; i++)
        {
            s32 acc = 0;
            int k;
            for (k = 0; k < 8; k++)
            {
                const int at = i - 7 + k;
                acc += fx->coef[k] * (at >= 0 ? delayed[at] : sFxPrev[f][at + 8]);
            }
            in[i] = clamp16(acc >> 15); /* reuse: the filtered output */
        }
        for (i = 0; i < 8; i++) sFxPrev[f][i] = delayed[JA_SUB - 8 + i];
        for (d = 0; d < 2; d++)
        {
            const int b = fx->dest[d].id ? bus_index(fx->dest[d].id) : -1;
            const s32 vol = fx->dest[d].volume;
            if (b < 0 || b == 2 + f || !vol) continue;
            for (i = 0; i < JA_SUB; i++) sBus[b][i] += (in[i] * vol) >> 15;
        }
    }
}

/* One sub-frame (the CPU released the DSP): all voices, effects, output. */
void gcn_jaudio_dsp_subframe(void)
{
    int i, b;

    for (b = 0; b < (int)JA_BUSES; b++)
    {
        for (i = 0; i < JA_SUB; i++) sBus[b][i] = 0;
    }
    if (sVoices)
    {
        for (i = 0; i < (int)sVoiceCount; i++)
        {
            JaVPB *v = &sVoices[i];
            if (v->enabled && !v->done) voice_run(i, v);
        }
    }
    fx_run();
    if (sSubframe < sSubframes && sOutL && sOutR)
    {
        s16 *l = sOutL + sSubframe * JA_SUB, *r = sOutR + sSubframe * JA_SUB;
        for (i = 0; i < JA_SUB; i++)
        {
            /* front left / right; the effect return (0x09A0) and the rear / surround buses
             * (0x0CA0, 0x0F40, 0x0FA0, 0x0B00) on both sides */
            const s32 both = sBus[10][i] + ((sBus[6][i] + sBus[7][i] + sBus[8][i] + sBus[9][i]) >> 1);
            const s32 L = sBus[0][i] + both, R = sBus[1][i] + both;
            l[i] = (s16)clamp16((s32)(((s64)L * (s32)sLevel) >> 12));
            r[i] = (s16)clamp16((s32)(((s64)R * (s32)sLevel) >> 12));
        }
    }
    sSubframe++;
}
