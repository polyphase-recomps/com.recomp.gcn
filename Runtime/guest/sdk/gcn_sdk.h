/*
 * Internal interface of the runtime's SDK replacement (guest/sdk/*.c).
 */
#ifndef GCN_SDK_H
#define GCN_SDK_H

#include <dolphin/types.h>

#define GCN_FST_RESERVE 0x00080000u /* top of RAM kept for the disc's file system table */

/* thread.c */
void gcn_threads_init(void);
/* Waits for the next event (vertical retrace) when nothing can run, and delivers it. */
void gcn_idle(void);
/* Delivers pending events (vertical retrace callbacks, finished disc reads) now. */
void gcn_poll_events(void);
void gcn_reschedule(void);

/* os.c: interrupts enabled (OSDisableInterrupts / OSRestoreInterrupts); handlers run between
 * enter (interrupts off, returns the old state) and leave (the old state back), as the
 * exception and its rfi */
int gcn_os_interrupts_enabled(void);
int gcn_os_interrupt_enter(void);
void gcn_os_interrupt_leave(int old);
/* thread.c: the game runs a loop (recompiled code, any loop): when interrupts are enabled,
 * deliver what is pending (disc reads, ARAM DMA, DSP, GPU), as the hardware would take them
 * there; no retrace, no thread switch */
void gcn_interrupt_point(void);
void gcn_idle_point(void);    /* thread.c: interrupts re-enabled; the idle thread lets time pass */

/* interrupt.c: runs the handlers of pending hardware interrupts; returns how many */
int gcn_dispatch_interrupts(void);

/* vi.c: called at every retrace by the event loop */
u32 gcn_vi_retrace(void);
/* dvd.c */
void gcn_dvd_init(void);
void gcn_dvd_poll(void);
int gcn_dvd_poll_count(void); /* same, returns how many requests completed */
int gcn_card_poll(void);      /* card.c: callbacks of finished asynchronous calls */
int gcn_dvd_stream_block(u8 *block); /* streamed audio: next 32-byte block, 0 if none */
void gcn_bridge_frame(void *pads); /* bridge.c: once per game frame, PADStatus[4] */
/* audio.c: delivers DSP task callbacks; returns how many ran */
int gcn_dsp_poll(void);
/* audio.c: once per retrace */
void gcn_audio_retrace(void);
/* musyx_dsp.c: the MusyX DSP program's work for one command list (one audio frame) */
void gcn_musyx_dsp_frame(const u16 *cmd);

#endif /* GCN_SDK_H */
