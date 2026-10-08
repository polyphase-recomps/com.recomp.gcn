/*
 * Video interface (retrace timing, frame buffer selection) and controllers.
 * The host owns the actual display: gcn_host_retrace() shows the frame the GPU
 * emulation copied out last and paces to 60 Hz.
 */
#include <dolphin.h>
#include <dolphin/vi.h>
#include <dolphin/pad.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

static VIRetraceCallback sPreRetrace, sPostRetrace;
static PADSamplingCallback sPadSampling; /* PADSetSamplingCallback: the SI poll's callback */
static volatile u32 sRetraceCount;
static OSThreadQueue sRetraceQueue;
static void *sNextFb, *sCurrentFb;
static BOOL sBlack = TRUE;

void VIInit(void) { OSInitThreadQueue(&sRetraceQueue); }
void VIConfigure(const GXRenderModeObj *rm) {}
void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height) {}
void VIFlush(void) { sCurrentFb = sNextFb; }
void VISetNextFrameBuffer(void *fb) { sNextFb = fb; }
void *VIGetNextFrameBuffer(void) { return sNextFb; }
void *VIGetCurrentFrameBuffer(void) { return sCurrentFb; }
void VISetBlack(BOOL black) { sBlack = black; }
u32 VIGetTvFormat(void) { return VI_NTSC; }
u32 VIGetDTVStatus(void) { return 0; }
u32 VIGetCurrentLine(void) { return 0; }
u32 VIGetNextField(void) { return sRetraceCount & 1; }
u32 VIGetRetraceCount(void) { return sRetraceCount; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback old = sPreRetrace;
    sPreRetrace = callback;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback old = sPostRetrace;
    sPostRetrace = callback;
    return old;
}

/* Vertical retrace: the host shows the frame and waits, then the interrupt runs, once
 * for every 1/60 s that passed (a frame that took longer misses retraces, as on the
 * console: the game's clock and the audio DMA keep real time). */
u32 gcn_vi_retrace(void)
{
    u32 periods = gcn_host_retrace(), i;

    if (periods < 1) periods = 1;
    if (periods > 4) periods = 4;
    for (i = 0; i < periods; i++)
    {
        sRetraceCount++;
        if (sPreRetrace) sPreRetrace(sRetraceCount);
        gcn_audio_retrace();
        if (sPostRetrace) sPostRetrace(sRetraceCount);
        /* the controllers are sampled once a frame (the SI polls at the default rate); games
         * that read them in the sampling callback (Pokemon Colosseum) see input only there */
        if (sPadSampling) sPadSampling();
    }
    OSWakeupThread(&sRetraceQueue);
    return periods;
}

void VIWaitForRetrace(void)
{
    u32 start = sRetraceCount;

    while (sRetraceCount == start)
    {
        OSSleepThread(&sRetraceQueue);
    }
}

/* ---- controllers ------------------------------------------------------------------------ */
static u32 sMotor[4];

BOOL PADInit(void) { return TRUE; }
int PADReset(u32 mask) { return TRUE; }
BOOL PADRecalibrate(u32 mask) { return TRUE; }
BOOL PADSync(void) { return TRUE; }
void PADSetSpec(u32 spec) {}
u32 PADGetSpec(void) { return 5; }
void PADSetAnalogMode(u32 mode) {}
void PADSetSamplingRate(u32 msec) {}
BOOL PADIsBarrel(s32 chan) { return FALSE; }
BOOL __PADDisableRecalibration(BOOL disable) { return FALSE; }

int PADGetType(s32 chan, u32 *type)
{
    *type = 0x09000000; /* standard controller */
    return TRUE;
}

u32 PADRead(PADStatus *status)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        memset(&status[i], 0, sizeof(PADStatus));
        if (!gcn_host_pad(i, &status[i])) status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    /* once per game frame: script requests and mod frame hooks (bridge.c) */
    gcn_bridge_frame(status);
    return 0;
}

void PADControlMotor(s32 chan, u32 command)
{
    if (chan < 0 || chan > 3) return;
    sMotor[chan] = command;
    gcn_host_rumble(chan, command == 1);
}

void PADControlAllMotors(const u32 *commandArray)
{
    int i;

    for (i = 0; i < 4; i++) PADControlMotor(i, commandArray[i]);
}

PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback callback)
{
    PADSamplingCallback old = sPadSampling;
    sPadSampling = callback;
    return old;
}

/* ---- serial interface: only what pads use --------------------------------------------- */
BOOL SIProbe(s32 chan) { return chan == 0; }
u32 SIGetType(s32 chan) { return chan == 0 ? 0x09000000 : 0x00000008; }
