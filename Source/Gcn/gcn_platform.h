/*
 * What the GameCube runtime needs from the program it runs in: the Polyphase addon
 * (GcnPlayer) or a standalone runner (Runtime/host). Plain C, so console and handheld
 * hosts can implement it as well.
 */
#ifndef GCN_PLATFORM_H
#define GCN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GameCube controller state, host byte order. Buttons use the PAD_BUTTON_* bits. */
typedef struct GcnPad
{
    uint16_t buttons;
    int8_t stick_x, stick_y, substick_x, substick_y; /* -128..127 */
    uint8_t trigger_l, trigger_r;                     /* 0..255 */
    uint8_t connected;
} GcnPad;

#define GCN_PAD_LEFT 0x0001
#define GCN_PAD_RIGHT 0x0002
#define GCN_PAD_DOWN 0x0004
#define GCN_PAD_UP 0x0008
#define GCN_PAD_Z 0x0010
#define GCN_PAD_R 0x0020
#define GCN_PAD_L 0x0040
#define GCN_PAD_A 0x0100
#define GCN_PAD_B 0x0200
#define GCN_PAD_X 0x0400
#define GCN_PAD_Y 0x0800
#define GCN_PAD_START 0x1000

void gcnp_log(const char *text);
void gcnp_crashed(void);                      /* does not return */
uint64_t gcnp_time_us(void);                  /* monotonic */
uint32_t gcnp_disc_read(void *dst, uint64_t offset, uint32_t size);
uint64_t gcnp_disc_size(void);
/* End of a frame: show gcn_gpu_frame() (RGBA8) and wait for the next 1/60 s, or give
 * control back to the engine until its next tick. Returns how many 1/60 s periods passed
 * since the last call (at least 1): a game slower than 60 Hz then sees the retraces it
 * missed, as on the console, so its clock and its audio keep real time. */
uint32_t gcnp_retrace(void);
#if defined(GEKKO)
/* Consoles (GPU passthrough): the game finished a picture, which is in the GPU's frame
 * buffer; give control to the engine, which draws over it and shows it. */
void gcnp_frame_done(void);
#endif
int gcnp_pad(int port, GcnPad *pad);
void gcnp_rumble(int port, int on);
/* Memory card images: size in bytes (0 = no card in that slot), raw reads and writes
 * (the guest keeps the card's layout). An absent image file is created zero-filled. */
uint32_t gcnp_card_size(int chan);
uint32_t gcnp_card_io(int chan, void *buf, uint32_t offset, uint32_t size, int write);
/* Interleaved stereo, host byte order. */
void gcnp_audio(const int16_t *samples, uint32_t frames, uint32_t rate);

/* Coroutines: one per guest thread. */
typedef struct GcnpCoro GcnpCoro;
GcnpCoro *gcnp_coro_current(void);
GcnpCoro *gcnp_coro_create(void (*fn)(void *), void *arg, size_t stack_bytes);
void gcnp_coro_switch(GcnpCoro *to);
void gcnp_coro_destroy(GcnpCoro *coro);

#ifdef __cplusplus
}
#endif

#endif /* GCN_PLATFORM_H */
