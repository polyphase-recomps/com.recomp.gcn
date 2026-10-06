/*
 * The GameCube GPU ("Flipper") as the runtime emulates it: the command stream the game's
 * GX library writes to the write-gather pipe (and display lists it calls), the
 * command-processor / pixel-engine registers, the embedded frame buffer and its copies.
 */
#ifndef GCN_GPU_H
#define GCN_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_EFB_W 640
#define GCN_EFB_H 528

void gcn_gpu_init(uint8_t *guest_mem);
/* Bytes written to the write-gather pipe (0xCC008000), in order. */
void gcn_gpu_fifo_write(uint32_t value, int bytes);
/* Passthrough: bytes of vertex data the draw in progress still expects; while nonzero, pipe
 * writes may go straight to the console's pipe (gcnw_ops.h) after taking theirs off here. */
extern uint32_t gcn_gpu_payload;
/* While the game builds a display list (the pipe detached from the GPU): where its next
 * bytes go in guest memory, and how far they may go there; NULL otherwise. */
extern uint8_t *gcn_gpu_dl_ptr, *gcn_gpu_dl_end;
/* Hardware registers of the command processor (0xCC000000), pixel engine (0xCC001000) and
 * processor interface FIFO (0xCC003000). */
uint32_t gcn_gpu_reg_read(uint32_t addr, int bytes);
void gcn_gpu_reg_write(uint32_t addr, uint32_t value, int bytes);
/* A vertical retrace happened (the GPU reports one more finished frame). */
void gcn_gpu_retrace(void);
/* Interrupts raised since the last call (bits = __OS_INTERRUPT_* numbers). */
uint32_t gcn_gpu_take_interrupts(void);
/* The last image copied to the external frame buffer (RGBA8, GCN_EFB_W wide). */
const uint32_t *gcn_gpu_frame(int *width, int *height);
/* Passthrough hosts (Wii): the GX RGB565 texture (4x4 tiles, 32-byte aligned) that the
 * game's copies to its external frame buffer go to; width and height multiples of 4. Never
 * set (the default), those copies are dropped: the picture stays in the embedded frame
 * buffer, where the host draws over it and copies it to the screen itself. */
void gcn_gpu_set_display(void *rgba8_tiled, int width, int height);
/* Copies to the external frame buffer so far (the game finished a picture). */
uint32_t gcn_gpu_display_copies(void);
/* Passthrough without a display texture: whether the game finished a picture (left in the
 * EFB) since the last call. */
int gcn_gpu_take_frame_done(void);
/* The color the game clears the frame buffer to with its copies (r, g, b, a). */
void gcn_gpu_clear_color(uint8_t rgba[4]);
/* Passthrough hosts: the host drew with the GPU since the game last did; give the game back
 * its state (vertex formats, arrays, transform and pixel registers it set). Call between the
 * host's drawing and the game's next command. No-op on software hosts. */
void gcn_gpu_resume(void);
/* Statistics of the frame being drawn (reset at each display copy). */
typedef struct GcnGpuStats
{
    uint32_t primitives, vertices, display_lists, efb_copies, unknown_commands;
    uint32_t fifo_bytes, bp_writes, xf_writes, cp_writes;
} GcnGpuStats;
const GcnGpuStats *gcn_gpu_last_frame_stats(void);
/* Microseconds spent passing the game's commands on (consoles) and write-gather pipe writes,
 * since the last call. */
void gcn_gpu_take_feed(uint32_t *us, uint32_t *calls);
const GcnGpuStats *gcn_gpu_current_stats(void); /* since the last display copy */

#ifdef __cplusplus
}
#endif

#endif /* GCN_GPU_H */
