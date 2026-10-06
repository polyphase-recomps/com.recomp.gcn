/*
 * The MusyX DSP microcode (MusyX <= 2.0, "dspSlave"), high level: what the sound engine's
 * DSP program does with the command list salCtrlDsp mails it every audio frame (160
 * samples, 5 ms at 32 kHz). The CPU side (hw_dspctrl.c, salBuildCommandList) prepares
 * everything in main RAM: one parameter block (PB) per voice, chained, with its sample in
 * ARAM; per studio (mixer) a depop block (SPB) and mix buffers; the list says which of
 * them to process and where the final 16-bit stereo goes (an AI DMA buffer).
 *
 * Mixing: every studio has nine 32-bit buses of 160 samples (main L/R/S, aux A L/R/S, aux
 * B L/R/S). A voice is processed in five 32-sample steps; before each step, the parameter
 * changes the CPU queued for it (update list: PB word offset, value) are applied. The
 * sample is read through the ARAM "accelerator" (DSP ADPCM, PCM16 or PCM8, end/loop
 * addresses), resampled by the pitch ratio, scaled by the envelope and mixed into the
 * buses with per-sample volume ramps. Aux buses go to the CPU (reverb callback) and come
 * back two frames later; Dolby Pro Logic II studios use the aux B buses as rear L/R.
 *
 * The semantics come from what the CPU side writes and reads back (the microcode itself
 * was not disassembled). Known approximations: the polyphase filters of the DSP's
 * coefficient ROM are replaced by one cubic (Catmull-Rom) interpolator; surround and rear
 * channels are matrixed into the stereo output without the encoder's 90-degree phase shift.
 */
#include <dolphin.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

#define MX_FRAME 160 /* samples per audio frame */
#define MX_STEP 32   /* samples between parameter updates */
#define MX_BUSES 9

/* Voice parameter block (_PB), 94 big-endian words */
typedef struct MxPB
{
    u16 nextHi, nextLo, currHi, currLo;
    u16 srcSelect;  /* 0 polyphase, 1 linear, 2 none */
    u16 coefSelect; /* polyphase filter table */
    u16 mixerCtrl;  /* 1 aux A, 2 aux B, 4 surround, 8 volume ramps, 0x10 DPL2 rear */
    u16 state;      /* 0 stopped, else playing */
    u16 loopType;   /* 0: the loop restores the ADPCM history; 1 (streams): it does not */
    u16 mix[18];    /* volume, delta: L, R, AL, AR, BL, BR, BS, S, AS */
    u16 itdFlag, itdBufferHi, itdBufferLo, itdShiftL, itdShiftR, itdTargetL, itdTargetR;
    u16 updNum[5], updDataHi, updDataLo;
    u16 dpop[9]; /* last sample per bus: L, AL, BL, R, AR, BR, S, AS, BS */
    u16 envVolume, envDelta;
    u16 fir[3];
    u16 loopFlag, format, loopHi, loopLo, endHi, endLo, curHi, curLo;
    u16 coef[16], gain, predScale, yn1, yn2;
    u16 ratioHi, ratioLo, frac, last[4];
    u16 loopPredScale, loopYn1, loopYn2;
    u16 streamLoopCnt;
} MxPB;

#define MX_PB_WORDS (sizeof(MxPB) / 2)

/* Studio depop block (_SPB): per bus a 32-bit start value and a per-sample delta */
typedef struct MxSPB
{
    u16 hi, lo, delta;
} MxSPB;

/* Buses in the order of the SPB (and of the DSP's own memory): */
enum { BUS_L, BUS_R, BUS_S, BUS_AL, BUS_AR, BUS_AS, BUS_BL, BUS_BR, BUS_BS };

/* per bus: its volume in MxPB.mix, its slot in MxPB.dpop, the ITD side (0 L, 1 R, 2 none) */
static const u8 sBusMix[MX_BUSES] = {0, 2, 14, 4, 6, 16, 8, 10, 12};
static const u8 sBusDpop[MX_BUSES] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
static const u8 sBusSide[MX_BUSES] = {0, 1, 2, 0, 1, 2, 0, 1, 2};

static s32 sBus[MX_BUSES][MX_FRAME]; /* the studio being mixed */
static s32 sOut[5][MX_FRAME];        /* final mix: L, R, S, rear L, rear R */

/* DPL2 studios: where each one's rear buses were stored, by main buffer */
static struct { u32 main, rear; } sRear[8];
static u32 sStudioRear;

#define ADDR(p) (((u32)(p)[0] << 16) | (p)[1])

static inline s32 clamp16(s32 v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* ---- ARAM reads (the accelerator) -------------------------------------------------------- */
static u8 sAram[512];
static u32 sAramBase = 0xFFFFFFFFu;

static inline u32 aram_byte(u32 a)
{
    if (a - sAramBase >= sizeof(sAram))
    {
        sAramBase = a & ~31u;
        gcn_host_aram(1, sAram, sAramBase, sizeof(sAram));
    }
    return sAram[a - sAramBase];
}

typedef struct MxAccel
{
    u32 cur, end, loop; /* nibbles (ADPCM), samples (PCM16) or bytes (PCM8) */
    u32 format, ps;
    s32 yn1, yn2;
    s16 coef[16];
    MxPB *pb;
} MxAccel;

static s32 accel_sample(MxAccel *a)
{
    s32 v;

    if (a->format == 0x0A) /* PCM16 */
        v = (s16)((aram_byte(a->cur * 2) << 8) | aram_byte(a->cur * 2 + 1));
    else if (a->format == 0x19) /* PCM8 */
        v = (s8)aram_byte(a->cur) << 8;
    else /* ADPCM: 8-byte frames, a predictor/scale byte and 14 nibbles */
    {
        s32 nib;
        u32 c;

        if ((a->cur & 15) == 0)
        {
            a->ps = aram_byte(a->cur >> 1);
            a->cur += 2;
        }
        nib = aram_byte(a->cur >> 1);
        nib = (a->cur & 1) ? nib & 15 : nib >> 4;
        if (nib >= 8) nib -= 16;
        c = (a->ps >> 4) & 7;
        v = (((nib << (a->ps & 15)) << 11) + 1024 + a->coef[c * 2] * a->yn1 + a->coef[c * 2 + 1] * a->yn2) >> 11;
        v = clamp16(v);
        a->yn2 = a->yn1;
        a->yn1 = v;
    }
    /* the end address is the last sample: continue at the loop address (for one-shot
     * samples the CPU points it at a zeroed block and stops the voice itself) */
    if (a->cur == a->end)
    {
        MxPB *pb = a->pb;

        a->cur = a->loop;
        if (pb->loopFlag)
        {
            if (a->format == 0)
            {
                a->ps = pb->loopPredScale;
                if (pb->loopType == 0)
                {
                    a->yn1 = (s16)pb->loopYn1;
                    a->yn2 = (s16)pb->loopYn2;
                }
            }
            pb->streamLoopCnt++;
        }
    }
    else a->cur++;
    return v;
}

/* ---- resampling --------------------------------------------------------------------------- */
static s16 sCubic[128][4]; /* Catmull-Rom weights (1.15) for 128 phases */
static int sCubicReady;

static void cubic_init(void)
{
    int p;

    for (p = 0; p < 128; p++)
    {
        f32 t = p / 128.0f, t2 = t * t, t3 = t2 * t;
        sCubic[p][0] = (s16)(16384.0f * (-t3 + 2.0f * t2 - t));
        sCubic[p][1] = (s16)(16384.0f * (3.0f * t3 - 5.0f * t2 + 2.0f) - 1.0f);
        sCubic[p][2] = (s16)(16384.0f * (-3.0f * t3 + 4.0f * t2 + t));
        sCubic[p][3] = (s16)(16384.0f * (t3 - t2));
    }
    sCubicReady = 1;
}

/* ---- one voice ---------------------------------------------------------------------------- */
static void mix_voice(MxPB *pb)
{
    u16 *words = (u16 *)pb;
    const u16 *upd = (const u16 *)ADDR(&pb->updDataHi);
    MxAccel a;
    s32 h[4], frac, smp[MX_STEP], side[2][MX_STEP], last[MX_BUSES];
    int step, i, b, playing = 0;

    a.cur = ADDR(&pb->curHi);
    a.end = ADDR(&pb->endHi);
    a.loop = ADDR(&pb->loopHi);
    a.format = pb->format;
    a.ps = pb->predScale;
    a.yn1 = (s16)pb->yn1;
    a.yn2 = (s16)pb->yn2;
    for (i = 0; i < 16; i++) a.coef[i] = (s16)pb->coef[i];
    a.pb = pb;
    frac = pb->frac;
    for (i = 0; i < 4; i++) h[i] = (s16)pb->last[i];
    for (b = 0; b < MX_BUSES; b++) last[b] = 0;

    for (step = 0; step < MX_FRAME / MX_STEP; step++)
    {
        u32 ratio, ctrl, n;
        s32 env, envDelta;

        for (n = pb->updNum[step]; n; n--, upd += 2)
            if (upd[0] < MX_PB_WORDS) words[upd[0]] = upd[1];
        ratio = ADDR(&pb->ratioHi);
        ctrl = pb->mixerCtrl;
        playing = pb->state != 0;

        if (playing)
        {
            env = (s16)pb->envVolume;
            envDelta = (s16)pb->envDelta;
            for (i = 0; i < MX_STEP; i++)
            {
                s32 v;

                if (pb->srcSelect == 2)
                    v = accel_sample(&a);
                else
                {
                    frac += ratio;
                    for (n = (u32)frac >> 16 > 64 ? 64 : (u32)frac >> 16; n; n--)
                    {
                        h[0] = h[1];
                        h[1] = h[2];
                        h[2] = h[3];
                        h[3] = accel_sample(&a);
                    }
                    frac &= 0xFFFF;
                    if (pb->srcSelect == 1)
                        v = h[1] + (((h[2] - h[1]) * (frac >> 1)) >> 15);
                    else
                    {
                        const s16 *c = sCubic[frac >> 9];
                        v = clamp16((c[0] * h[0] + c[1] * h[1] + c[2] * h[2] + c[3] * h[3]) >> 15);
                    }
                }
                smp[i] = (v * env) >> 15;
                env += envDelta;
                if (env < 0) env = 0;
                if (env > 0x7FFF) env = 0x7FFF;
            }
            pb->envVolume = (u16)env;

            /* interaural time difference: each side delayed by its own shift (samples), which
             * glides to its target over the step */
            if (pb->itdFlag)
            {
                s16 *hist = (s16 *)ADDR(&pb->itdBufferHi);
                s32 all[MX_STEP * 2];
                s32 sl = pb->itdShiftL > MX_STEP ? MX_STEP : pb->itdShiftL;
                s32 sr = pb->itdShiftR > MX_STEP ? MX_STEP : pb->itdShiftR;
                s32 tl = pb->itdTargetL > MX_STEP ? MX_STEP : pb->itdTargetL;
                s32 tr = pb->itdTargetR > MX_STEP ? MX_STEP : pb->itdTargetR;

                for (i = 0; i < MX_STEP; i++)
                {
                    all[i] = hist[i];
                    all[MX_STEP + i] = smp[i];
                }
                for (i = 0; i < MX_STEP; i++)
                {
                    side[0][i] = all[MX_STEP + i - (sl + (tl - sl) * (i + 1) / MX_STEP)];
                    side[1][i] = all[MX_STEP + i - (sr + (tr - sr) * (i + 1) / MX_STEP)];
                    hist[i] = (s16)smp[i];
                }
                pb->itdShiftL = (u16)tl;
                pb->itdShiftR = (u16)tr;
            }
            else
                for (i = 0; i < MX_STEP; i++) side[0][i] = side[1][i] = smp[i];
        }

        /* buses: the same enables the CPU's depop bookkeeping (HandleDepopVoice) uses */
        for (b = 0; b < MX_BUSES; b++)
        {
            u16 *vp = &pb->mix[sBusMix[b]];
            u32 vol = vp[0];
            s32 delta = (s16)vp[1];
            const s32 *src = sBusSide[b] == 2 ? smp : side[sBusSide[b]];
            s32 *dst = sBus[b];
            int on;

            switch (b)
            {
            case BUS_S: on = ctrl & 4; break;
            case BUS_AL:
            case BUS_AR: on = ctrl & 1; break;
            case BUS_AS: on = (ctrl & 1) && (ctrl & 0x14); break;
            case BUS_BL:
            case BUS_BR: on = ctrl & 0x12; break;
            case BUS_BS: on = (ctrl & 0x12) && (ctrl & 4); break;
            default: on = 1; break;
            }
            if (playing && on)
            {
                for (i = 0; i < MX_STEP; i++)
                {
                    last[b] = (src[i] * (s32)vol) >> 15;
                    dst[step * MX_STEP + i] += last[b];
                    vol = (u16)(vol + delta);
                }
            }
            else
            {
                last[b] = 0;
                vol = (u16)(vol + delta * MX_STEP);
            }
            vp[0] = (u16)vol;
        }
    }

    pb->curHi = (u16)(a.cur >> 16);
    pb->curLo = (u16)a.cur;
    pb->predScale = (u16)a.ps;
    pb->yn1 = (u16)a.yn1;
    pb->yn2 = (u16)a.yn2;
    pb->frac = (u16)frac;
    for (i = 0; i < 4; i++) pb->last[i] = (u16)h[i];
    /* what the voice contributed last: the CPU fades it out if the voice is cut */
    for (b = 0; b < MX_BUSES; b++) pb->dpop[sBusDpop[b]] = (u16)clamp16(playing ? last[b] : 0);
}

/* ---- studios and the final mix ------------------------------------------------------------ */
static void studio_begin(const MxSPB *spb)
{
    int b, i;

    for (b = 0; b < MX_BUSES; b++)
    {
        s32 v = (s32)ADDR(&spb[b].hi), d = (s16)spb[b].delta;
        for (i = 0; i < MX_FRAME; i++, v += d) sBus[b][i] = v;
    }
    sStudioRear = 0;
}

/* L/R/S of another studio's previous frame, into main, aux A and aux B */
static void studio_input(const s32 *in, u32 vol, u32 volA, u32 volB)
{
    int c, i;

    for (c = 0; c < 3; c++)
        for (i = 0; i < MX_FRAME; i++)
        {
            s64 v = in[c * MX_FRAME + i];
            sBus[BUS_L + c][i] += (s32)((v * vol) >> 15);
            sBus[BUS_AL + c][i] += (s32)((v * volA) >> 15);
            sBus[BUS_BL + c][i] += (s32)((v * volB) >> 15);
        }
}

static void buses_store(int first, s32 *dst)
{
    int c, i;

    for (c = 0; c < 3; c++)
        for (i = 0; i < MX_FRAME; i++) dst[c * MX_FRAME + i] = sBus[first + c][i];
}

/* aux send: the buses go to the CPU's effect, whose output (two frames old) comes back */
static void studio_aux(int first, s32 *send, const s32 *ret)
{
    int c, i;

    buses_store(first, send);
    for (c = 0; c < 3; c++)
        for (i = 0; i < MX_FRAME; i++) sBus[BUS_L + c][i] += ret[c * MX_FRAME + i];
}

static void master_add(const s32 *main)
{
    int c, i, s;

    for (c = 0; c < 3; c++)
        for (i = 0; i < MX_FRAME; i++) sOut[c][i] += main[c * MX_FRAME + i];
    for (s = 0; s < 8; s++)
        if (sRear[s].main == (u32)main && sRear[s].rear)
        {
            const s32 *rear = (const s32 *)sRear[s].rear;
            for (i = 0; i < MX_FRAME; i++)
            {
                sOut[3][i] += rear[i];
                sOut[4][i] += rear[MX_FRAME + i];
            }
        }
}

/* Lt/Rt: surround in opposite phase on both sides, rear channels as the DPL2 encoder */
static void master_output(s16 *dest)
{
    int i;

    for (i = 0; i < MX_FRAME; i++)
    {
        s64 s = sOut[2][i], ls = sOut[3][i], rs = sOut[4][i];
        s64 l = sOut[0][i] - s - ((ls * 28567 + rs * 16053) >> 15);
        s64 r = sOut[1][i] + s + ((ls * 16053 + rs * 28567) >> 15);
        dest[i * 2] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        dest[i * 2 + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

/* remembers (or forgets, rear 0) where a studio's rear buses go with its main output */
static void rear_note(u32 main)
{
    int s, free = -1;

    for (s = 0; s < 8; s++)
    {
        if (sRear[s].main == main) break;
        if (!sRear[s].main && free < 0) free = s;
    }
    if (s == 8)
    {
        if (free < 0 || !sStudioRear) return;
        s = free;
    }
    sRear[s].main = main;
    sRear[s].rear = sStudioRear;
}

/* Runs one command list (salBuildCommandList): one audio frame. */
void gcn_musyx_dsp_frame(const u16 *cmd)
{
    int guard = 4096;

    if (!sCubicReady) cubic_init();
    sAramBase = 0xFFFFFFFFu; /* the CPU may have changed ARAM since */
    while (guard--)
    {
        switch (cmd[0])
        {
        case 0: /* studio: depop block */
            studio_begin((const MxSPB *)ADDR(cmd + 1));
            cmd += 3;
            break;
        case 1: /* input from another studio */
            studio_input((const s32 *)ADDR(cmd + 1), cmd[3], cmd[4], cmd[5]);
            cmd += 6;
            break;
        case 2: /* voices: a chain of parameter blocks */
        {
            MxPB *pb = (MxPB *)ADDR(cmd + 1);
            int n = 256;
            while (pb && n--)
            {
                mix_voice(pb);
                pb = (MxPB *)ADDR(&pb->nextHi);
            }
            cmd += 3;
            break;
        }
        case 3: /* end of the voices */
            cmd += 1;
            break;
        case 4: /* aux A: send buffer, return buffer */
            studio_aux(BUS_AL, (s32 *)ADDR(cmd + 1), (const s32 *)ADDR(cmd + 3));
            cmd += 5;
            break;
        case 5: /* aux B */
            studio_aux(BUS_BL, (s32 *)ADDR(cmd + 1), (const s32 *)ADDR(cmd + 3));
            cmd += 5;
            break;
        case 16: /* Pro Logic II studio: aux B are the rear channels, kept with the frame */
            sStudioRear = ADDR(cmd + 1);
            buses_store(BUS_BL, (s32 *)sStudioRear);
            cmd += 5;
            break;
        case 6: /* studio output (main L/R/S of this frame) */
            buses_store(BUS_L, (s32 *)ADDR(cmd + 1));
            rear_note(ADDR(cmd + 1));
            cmd += 3;
            break;
        case 17: /* final mix: start */
        {
            s32 *o = &sOut[0][0];
            int i;
            for (i = 0; i < 5 * MX_FRAME; i++) o[i] = 0;
            cmd += 3;
            break;
        }
        case 9: /* final mix: add a master studio */
            master_add((const s32 *)ADDR(cmd + 1));
            cmd += 3;
            break;
        case 14: /* final mix: to 16-bit stereo */
            master_output((s16 *)ADDR(cmd + 3));
            cmd += 5;
            break;
        case 13: /* continue in the next part of the list */
            cmd = (const u16 *)ADDR(cmd + 1);
            break;
        case 15: /* end */
            return;
        default:
        {
            static int said;
            if (!said++) gcn_logf("musyx dsp: unknown command %u", cmd[0]);
            return;
        }
        }
    }
}
