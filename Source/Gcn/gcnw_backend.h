/*
 * The runtime's entry points for the program hosting a game (GcnPlayer or a standalone
 * runner): one game instance at a time.
 */
#ifndef GCNW_BACKEND_H
#define GCNW_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "gcnw_module.h"

#ifdef __cplusplus
extern "C" {
#endif

int gcnw_instantiate(const GcnwModule *module);
/* Runs the game on the calling coroutine; returns only if the game's main() returns. */
void gcnw_run(void);
/* How many times gcnw_run started a game (state kept across runs can tell a new one). */
uint32_t gcnw_run_count(void);
/* The coroutine the current guest thread runs on, replaced (returns the previous one): a
 * recompiled game's own threads (recomp_native.c, GS engine threads) run on coroutines of their
 * own on behalf of the guest thread that schedules them, so a switch back to that guest thread
 * must land in them. */
struct GcnpCoro *gcnw_ctx_swap_coro(struct GcnpCoro *coro);
void gcnw_free(void);

uint8_t *gcnw_memory(void);
uint8_t *gcnw_aram(void);
uint32_t gcnw_read32(uint32_t addr);
void gcnw_write32(uint32_t addr, uint32_t value);
uint32_t gcnw_retrace_count(void);
/* Time the game spent in host calls, by kind, per frame over `frames` (consoles; resets). */
void gcnw_take_import_profile(char *out, size_t cap, uint32_t frames);

/* A game global by name (NULL if the game has no symbol table or no such name). */
const GcnwSymbol *gcnw_find_symbol(const char *name);

/* ---- script bridge: what the game's mods published (gcn_mod.h) -------------------------
 * Call from the thread that drives the game, while the game waits for a retrace. */
enum
{
    GCNW_VAR_U8 = 1,
    GCNW_VAR_S8,
    GCNW_VAR_U16,
    GCNW_VAR_S16,
    GCNW_VAR_U32,
    GCNW_VAR_S32,
    GCNW_VAR_F32,
    GCNW_VAR_STR,
};

typedef struct GcnwBridgeVar
{
    char name[64];
    uint32_t addr;   /* element 0 */
    int type;        /* GCNW_VAR_* */
    int count;
    int stride;      /* bytes between elements */
    char help[128];
} GcnwBridgeVar;

typedef struct GcnwBridgeRequest
{
    char name[64];
    char help[128];
} GcnwBridgeRequest;

typedef struct GcnwBridgeEvent
{
    char name[64];
    int args[8];
    int nargs;
} GcnwBridgeEvent;

int gcnw_bridge_var_count(void);
const GcnwBridgeVar *gcnw_bridge_var(int index);
const GcnwBridgeVar *gcnw_bridge_find_var(const char *name);
int gcnw_bridge_request_count(void);
const GcnwBridgeRequest *gcnw_bridge_request_info(int index);
/* Queues a request the game runs at its next frame; returns its id (0: no game / full). */
int gcnw_bridge_request(const char *name, const int *args, int nargs);
/* 1 once request `id` ran, with its handler's result (each result is handed out once). */
int gcnw_bridge_result(int id, int *result);
/* Oldest event a mod emitted (1), or 0 when there is none. */
int gcnw_bridge_next_event(GcnwBridgeEvent *out);

#ifdef __cplusplus
}
#endif

#endif /* GCNW_BACKEND_H */
