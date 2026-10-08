/*
 * Audio hardware: AI (DMA of mixed samples to the DAC), ARAM (auxiliary RAM, kept by the
 * host) and the DSP. The DSP's microcode is not run: its work is done in C (musyx_dsp.c,
 * the MusyX sound engine's microcode) when the game mails it a frame to mix.
 */
#include <dolphin.h>
#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/dsp.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

static int dsp_resume(void);

/* ---- AI --------------------------------------------------------------------------------- */
static AIDCallback sAiCallback;
static u32 sAiStart, sAiLength;
static BOOL sAiRunning;
static u32 sDspRate = 32000;
static u32 sAiFraction;

AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
    AIDCallback old = sAiCallback;
    sAiCallback = callback;
    return old;
}

void AIInitDMA(u32 start_addr, u32 length)
{
    sAiStart = start_addr;
    sAiLength = length;
}

BOOL AIGetDMAEnableFlag(void) { return sAiRunning; }
void AIStartDMA(void) { sAiRunning = TRUE; }
void AIStopDMA(void) { sAiRunning = FALSE; }
u32 AIGetDMABytesLeft(void) { return 0; }
u32 AIGetDMAStartAddr(void) { return sAiStart; }
u32 AIGetDMALength(void) { return sAiLength; }
BOOL AICheckInit(void) { return TRUE; }
void AIInit(u8 *stack) {}
void AIReset(void) {}
void AISetDSPSampleRate(u32 rate) { sDspRate = rate ? 48000 : 32000; }
u32 AIGetDSPSampleRate(void) { return sDspRate == 48000; }

/* ---- AI streaming: the drive's audio track (dvd.c), decoded and mixed by the AI --------- */
static u32 sAisState, sAisRate = 48000, sAisCount, sAisTrigger, sAisFrac;
static u8 sAisVolL, sAisVolR;
static AISCallback sAisCallback;
static s16 sAisPcm[28 * 2];
static int sAisPos = 28;
static s32 sAisHist[4], sAisPrev[2], sAisCur[2];

AISCallback AIRegisterStreamCallback(AISCallback callback)
{
    AISCallback old = sAisCallback;
    sAisCallback = callback;
    return old;
}

u32 AIGetStreamSampleCount(void) { return sAisCount; }
void AIResetStreamSampleCount(void) { sAisCount = 0; }
void AISetStreamTrigger(u32 trigger) { sAisTrigger = trigger; }
u32 AIGetStreamTrigger(void) { return sAisTrigger; }
void AISetStreamPlayState(u32 state) { sAisState = state; }
u32 AIGetStreamPlayState(void) { return sAisState; }
void AISetStreamSampleRate(u32 rate) { sAisRate = rate ? 48000 : 32000; }
u32 AIGetStreamSampleRate(void) { return sAisRate == 48000; }
void AISetStreamVolLeft(u8 vol) { sAisVolL = vol; }
u8 AIGetStreamVolLeft(void) { return sAisVolL; }
void AISetStreamVolRight(u8 vol) { sAisVolR = vol; }
u8 AIGetStreamVolRight(void) { return sAisVolR; }

/* the drive's ADPCM: per 32-byte block a predictor/shift byte for each side (and copies),
 * then 28 bytes of a left (low) and a right (high) nibble; fixed predictors */
static s32 adp_sample(s32 nib, u32 ps, s32 *hist)
{
    s32 h = 0, v;

    switch ((ps >> 4) & 3)
    {
    case 1: h = hist[0] * 0x3C; break;
    case 2: h = hist[0] * 0x73 - hist[1] * 0x34; break;
    case 3: h = hist[0] * 0x62 - hist[1] * 0x37; break;
    }
    h = (h + 0x20) >> 6;
    if (h > 0x1FFFFF) h = 0x1FFFFF;
    if (h < -0x200000) h = -0x200000;
    v = ((((s32)(nib << 28) >> 28) << 12 >> (ps & 15)) << 6) + h;
    hist[1] = hist[0];
    hist[0] = v;
    v >>= 6;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* next stream sample pair; 0 once the drive stopped */
static int ais_next(s32 *lr)
{
    if (sAisPos >= 28)
    {
        u8 blk[32];
        int i, r = gcn_dvd_stream_block(blk);

        if (!r) return 0;
        if (r == 2) sAisHist[0] = sAisHist[1] = sAisHist[2] = sAisHist[3] = 0;
        for (i = 0; i < 28; i++)
        {
            sAisPcm[i * 2] = (s16)adp_sample(blk[4 + i] & 15, blk[0], &sAisHist[0]);
            sAisPcm[i * 2 + 1] = (s16)adp_sample(blk[4 + i] >> 4, blk[1], &sAisHist[2]);
        }
        sAisPos = 0;
    }
    lr[0] = sAisPcm[sAisPos * 2];
    lr[1] = sAisPcm[sAisPos * 2 + 1];
    sAisPos++;
    if (++sAisCount == sAisTrigger && sAisCallback) sAisCallback(sAisCount);
    return 1;
}

/* a DMA buffer with the stream mixed in (resampled to the DMA rate) */
static const void *ais_mix(const s16 *dma, u32 bytes)
{
    static s16 out[8192];
    u32 i, n = bytes / 4 > 4096 ? 4096 : bytes / 4;
    u32 step = (sAisRate << 16) / sDspRate;

    for (i = 0; i < n; i++)
    {
        s32 c, v;
        for (sAisFrac += step; sAisFrac >= 0x10000; sAisFrac -= 0x10000)
        {
            sAisPrev[0] = sAisCur[0];
            sAisPrev[1] = sAisCur[1];
            if (!ais_next(sAisCur)) sAisCur[0] = sAisCur[1] = 0;
        }
        for (c = 0; c < 2; c++)
        {
            v = sAisPrev[c] + (((sAisCur[c] - sAisPrev[c]) * (s32)(sAisFrac >> 1)) >> 15);
            v = dma[i * 2 + c] + ((v * (c ? sAisVolR : sAisVolL)) >> 8);
            out[i * 2 + c] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    return out;
}

/* Per retrace: the DMA buffers the DAC would have played in 1/60 s, each handed to the
 * host and followed by the DMA interrupt (where the game queues the next buffer). */
void gcn_audio_retrace(void)
{
    int guard = 16;

    if (!sAiRunning || sAiLength == 0 || !sAiCallback) return;
    sAiFraction += sDspRate * 4; /* bytes of 16-bit stereo per second / 60, kept exact */
    while (sAiFraction >= sAiLength * 60 && guard--)
    {
        sAiFraction -= sAiLength * 60;
        if (sAisState)
            gcn_host_audio(ais_mix((const s16 *)sAiStart, sAiLength), sAiLength > 16384 ? 16384 : sAiLength, sDspRate);
        else gcn_host_audio((void *)sAiStart, sAiLength, sDspRate);
        sAiCallback();
        dsp_resume(); /* the DSP is done with the frame the callback gave it */
    }
}

/* ---- ARAM ------------------------------------------------------------------------------- */
#define ARAM_SIZE 0x01000000u
#define ARAM_USER_BASE 0x4000u
static u32 sAramTop = ARAM_USER_BASE;
static ARCallback sArCallback;

u32 ARInit(u32 *stack_index_addr, u32 num_entries) { return ARAM_USER_BASE; }
BOOL ARCheckInit(void) { return TRUE; }
void ARReset(void) {}
void ARSetSize(void) {}
u32 ARGetBaseAddress(void) { return ARAM_USER_BASE; }
u32 ARGetSize(void) { return ARAM_SIZE; }
u32 ARGetInternalSize(void) { return ARAM_SIZE; }
u32 ARGetDMAStatus(void) { return 0; }
void ARClear(u32 flag) {}

ARCallback ARRegisterDMACallback(ARCallback callback)
{
    ARCallback old = sArCallback;
    sArCallback = callback;
    return old;
}

/* The transfers happen at once; their completion callbacks are the DMA interrupt and run where
 * interrupts are delivered (gcn_dsp_poll: the next scheduling point, spin or retrace), never
 * inside the call that started the transfer. Games count on that: Metroid Prime's ARAM file
 * cache (CDvdFile::PingARAMTransfer) posts a transfer and then clears the "ARAM idle" flag its
 * callback sets, and chains the next chunk from the callbacks. */
static int sArDmaPending;

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    gcn_host_aram(type, (void *)mainmem_addr, aram_addr, length);
    if (sArCallback) sArDmaPending++;
}

u32 ARAlloc(u32 length)
{
    u32 p = sAramTop;
    sAramTop += length;
    return p;
}

u32 ARFree(u32 *length) { return sAramTop; }

void ARQInit(void) {}
void ARQSetChunkSize(u32 size) {}

#define ARQ_PENDING 1024
static struct
{
    ARQRequest *request;
    ARQCallback callback;
} sArqDone[ARQ_PENDING];
static int sArqHead, sArqCount;

/* The ARAM DMA interrupts: completions of ARStartDMA and ARQ transfers, in order. Transfers
 * the callbacks start complete in the same call. Returns how many callbacks ran. */
static int ar_poll(void)
{
    int ran = 0;

    while ((sArDmaPending > 0 || sArqCount > 0) && ran < 4096)
    {
        if (sArDmaPending > 0)
        {
            sArDmaPending--;
            if (sArCallback) sArCallback();
        }
        else
        {
            ARQRequest *r = sArqDone[sArqHead].request;
            ARQCallback cb = sArqDone[sArqHead].callback;
            sArqHead = (sArqHead + 1) % ARQ_PENDING;
            sArqCount--;
            cb((u32)r);
        }
        ran++;
    }
    return ran;
}

void ARQPostRequest(ARQRequest *request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback)
{
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) gcn_host_aram(type, (void *)source, dest, length);
    else gcn_host_aram(type, (void *)dest, source, length);
    if (!callback) return;
    /* the completion: at the next interrupt delivery (ar_poll, see ARStartDMA) */
    if (sArqCount == ARQ_PENDING) gcn_host_fatal("gcn: too many ARAM requests waiting for completion");
    sArqDone[(sArqHead + sArqCount) % ARQ_PENDING].request = request;
    sArqDone[(sArqHead + sArqCount) % ARQ_PENDING].callback = callback;
    sArqCount++;
}

/* ---- DSP --------------------------------------------------------------------------------- */
/* The task's program is not run; a frame the game mails is mixed at once (the mail is a
 * "0xBABE" header word and the address of a command list) and the task is resumed when
 * the CPU next takes interrupts. */
static DSPTaskInfo *sTasks;
static DSPTaskInfo *sInitPending, *sResumePending;
static u32 sMailHeader;

void DSPInit(void) {}
BOOL DSPCheckInit(void) { return TRUE; }
u32 DSPCheckMailToDSP(void) { return 0; }
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadCPUToDSPMbox(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }
void DSPAssertInt(void) {}
void DSPHalt(void) {}
void DSPReset(void) {}
u32 DSPGetDMAStatus(void) { return 0; }

/* GCN_DSP_MUSYX (default 1): the DSP program is MusyX's. A game whose sound is AX (its own
 * microcode, the same 0xBABE frame mails, other commands: F-Zero GX) sets it to 0 in its
 * gcn_game.json "defines": frames are acknowledged without being run (no sound, and no MusyX
 * reading of AX command lists, whose output addresses it would write to). */
#ifndef GCN_DSP_MUSYX
#define GCN_DSP_MUSYX 1
#endif

void DSPSendMailToDSP(u32 mail)
{
    if (!sMailHeader)
    {
        if ((mail >> 16) == 0xBABE) sMailHeader = mail;
        return;
    }
    sMailHeader = 0;
#if GCN_DSP_MUSYX
    gcn_musyx_dsp_frame((const u16 *)mail);
#endif
    sResumePending = sTasks;
}

DSPTaskInfo *DSPAddTask(DSPTaskInfo *task)
{
    task->next = sTasks;
    sTasks = task;
    task->state = 1;
    sInitPending = task; /* the DSP answers later, by interrupt */
    return task;
}

DSPTaskInfo *DSPCancelTask(DSPTaskInfo *task) { return task; }

static int dsp_resume(void)
{
    DSPTaskInfo *t = sResumePending;

    if (!t) return 0;
    sResumePending = 0;
    if (t->res_cb) t->res_cb(t);
    return 1;
}

int gcn_dsp_poll(void)
{
    DSPTaskInfo *t = sInitPending;
    const int ar = ar_poll(); /* the ARAM's DMA interrupts too */

    if (!t) return ar + dsp_resume();
    sInitPending = 0;
    if (t->init_cb) t->init_cb(t);
    return ar + 1 + dsp_resume();
}
