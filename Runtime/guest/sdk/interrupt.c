/*
 * Hardware interrupts the SDK's own code registers handlers for (the GX library's
 * pixel-engine token / finish and command-processor interrupts). The host raises them
 * as the emulated GPU processes the command stream; they are delivered at the next
 * scheduling point (gcn_dispatch_interrupts).
 */
#include <dolphin.h>
#include <dolphin/os.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

static __OSInterruptHandler sHandlers[32];
static OSInterruptMask sMasked = 0xFFFFFFFFu;
static OSContext sInterruptContext;
static OSContext *sCurrentContext;

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler)
{
    __OSInterruptHandler old = sHandlers[interrupt & 31];
    sHandlers[interrupt & 31] = handler;
    return old;
}

__OSInterruptHandler __OSGetInterruptHandler(__OSInterrupt interrupt)
{
    return sHandlers[interrupt & 31];
}

OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask)
{
    OSInterruptMask old = sMasked;
    sMasked |= mask;
    return old;
}

OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask)
{
    OSInterruptMask old = sMasked;
    sMasked &= ~mask;
    return old;
}

void OSClearContext(OSContext *context) {}
void OSSetCurrentContext(OSContext *context) { sCurrentContext = context; }
OSContext *OSGetCurrentContext(void) { return sCurrentContext ? sCurrentContext : &sInterruptContext; }

/* interrupts the SDK replacement raises itself (the DSP's, audio.c) */
static u32 sRaised;

void gcn_raise_interrupt(int interrupt) { sRaised |= 1u << (interrupt & 31); }
int gcn_interrupts_raised(void) { return sRaised != 0; }

/* Runs the handlers of the interrupts the host has pending; returns how many ran. */
int gcn_dispatch_interrupts(void)
{
    u32 pending = gcn_host_interrupts() | sRaised;

    sRaised = 0;
    int ran = 0, i;

    for (i = 0; i < 32 && pending; i++)
    {
        if (!(pending & (1u << i))) continue;
        pending &= ~(1u << i);
        if (sHandlers[i])
        {
            sHandlers[i]((__OSInterrupt)i, &sInterruptContext);
            ran++;
        }
    }
    return ran;
}

/* ---- the boot ROM's font (OSLoadFont): not provided ---------------------------------- */
u16 OSGetFontEncode(void) { return 0; }
u32 OSLoadFont(OSFontHeader *fontData, void *tmp) { return 0; }
char *OSGetFontTexel(const char *string, void *image, s32 pos, s32 stride, s32 *width)
{
    if (width) *width = 0;
    return (char *)(string + 1);
}
char *OSGetFontWidth(const char *string, s32 *width)
{
    if (width) *width = 0;
    return (char *)(string + 1);
}

u32 PPCMfhid2(void) { return 0; }
void PPCMthid2(u32 v) {}

/* Metrowerks runtime: float to unsigned (saturating like the hardware conversion) */
u32 __cvt_fp2unsigned(f64 x)
{
    if (x != x || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (u32)x;
}
