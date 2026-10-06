/*
 * What guest code (the runtime's SDK replacement, game glue in Native/game, mods) can use
 * from the host. Guest code is compiled through the big-endian pipeline: pointers passed
 * here are guest addresses, and data behind them is in GameCube (big-endian) byte order.
 *
 * Every gcn_host_* function is a wasm import implemented by the host runtime
 * (com.recomp.gcn Source/ or the standalone runner); see Runtime/tools/gcn_imports.txt.
 */
#ifndef GCN_GUEST_H
#define GCN_GUEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char gcn_u8;
typedef unsigned short gcn_u16;
typedef unsigned int gcn_u32;
typedef unsigned long long gcn_u64;

/* ---- host imports -------------------------------------------------------------------- */
void gcn_host_log(const char *text);
void gcn_host_fatal(const char *text);              /* does not return */
gcn_u64 gcn_host_ticks(void);                       /* OS time base (40.5 MHz) */
gcn_u32 gcn_host_disc_read(void *dst, gcn_u32 offset, gcn_u32 size); /* bytes read */
gcn_u32 gcn_host_disc_size(void);
/* Ends the current frame: the host shows what the GPU drew and waits for the next
 * vertical retrace (paced at 60 Hz, or returns to the engine between frames). Returns
 * the retrace count. */
gcn_u32 gcn_host_retrace(void); /* returns the 1/60 s periods that passed (at least 1) */
/* Pad state of a controller port: buttons (PAD_BUTTON_* bits), sticks and triggers in
 * the PADStatus layout; returns 0 if nothing is connected. */
int gcn_host_pad(int port, void *pad_status);
void gcn_host_rumble(int port, int on);
/* Memory card images (slot 0 = A, 1 = B), kept by the host: size in bytes (0 = no card),
 * and reads/writes of raw bytes; returns the bytes moved. */
gcn_u32 gcn_host_card_size(gcn_u32 chan);
gcn_u32 gcn_host_card_io(gcn_u32 chan, void *buf, gcn_u32 offset, gcn_u32 size, gcn_u32 write);
/* THP movie frame: decoded by the host into the three I8-tiled planes; 0 or an SDK error code. */
int gcn_host_thp_decode(const void *file, void *tile_y, void *tile_u, void *tile_v);
/* On-screen text line (0..15) the host draws on every frame shown; "" clears it. */
void gcn_host_overlay(int line, const char *text);
/* Data cache maintenance over guest memory (op: 0 flush, 1 store, 2 invalidate): real on hosts
 * whose GPU reads memory behind the CPU caches (Wii), nothing elsewhere. */
void gcn_host_dcache(gcn_u32 op, const void *addr, gcn_u32 bytes);
/* Audio: `bytes` of 16-bit big-endian stereo PCM at `rate` Hz, played after what came before. */
void gcn_host_audio(const void *pcm, gcn_u32 bytes, gcn_u32 rate);
/* Auxiliary RAM (16 MB, kept by the host): dir 0 = main RAM -> ARAM, 1 = ARAM -> main RAM. */
void gcn_host_aram(gcn_u32 dir, void *ram, gcn_u32 aram_addr, gcn_u32 bytes);

/* Interrupts the emulated hardware raised since the last call (bit = __OS_INTERRUPT_*). */
gcn_u32 gcn_host_interrupts(void);

/* Execution contexts (guest threads): the host gives each its own native stack. */
int gcn_host_ctx_create(void *entry, void *arg, void *stack_top);
void gcn_host_ctx_switch(int ctx);
void gcn_host_ctx_destroy(int ctx);

/* Script bridge (port_bridge.h): requests and events between the game and Polyphase. */
void gcn_host_bridge_publish(const void *vars, int nvars, const void *requests, int nrequests);
int gcn_host_bridge_poll(char *name, int name_cap, int *args, int max_args, int *nargs);
void gcn_host_bridge_done(int id, int result);
void gcn_host_bridge_emit(const char *name, const int *args, int nargs);

/* ---- guest helpers (guest/os.c) ------------------------------------------------------ */
void gcn_logf(const char *fmt, ...);
#define GCN_TODO_ONCE(what)                                    \
    do {                                                       \
        static int gcn_todo_said;                              \
        if (!gcn_todo_said) {                                  \
            gcn_todo_said = 1;                                 \
            gcn_host_log("TODO: " what " is not implemented"); \
        }                                                      \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* GCN_GUEST_H */
