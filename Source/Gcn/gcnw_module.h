/*
 * A GameCube game translated by wasm2c, as the runtime sees it. Runtime/tools/
 * gcn_wasm_to_c.py writes one descriptor per game (<name>_guest_module.c) next to the
 * generated code; the addon build also gets <name>_guest_register.cpp, which registers it
 * at startup so GcnPlayer finds it by package id.
 */
#ifndef GCNW_MODULE_H
#define GCNW_MODULE_H

#include "wasm-rt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct w2c_env;

/* A global of the game at its address (gcn_place.py: the original DOL address when the
 * decomp's layout is known), for scripts and mods that address game variables by name. */
typedef struct GcnwSymbol
{
    const char *name;
    uint32_t addr;
    uint32_t size;
} GcnwSymbol;

typedef struct GcnwModule
{
    const char *name;      /* build name, e.g. "sfa" */
    const char *package;   /* game package id, e.g. "com.recomp.starfoxadventures" */
    const char *title;     /* "Star Fox Adventures" */
    const char *disc_name; /* file name of the disc image the game was built for */
    void (*instantiate)(struct w2c_env *env);
    void (*free)(void);
    void (*run)(void);                                 /* boot + main(); returns if main returns */
    void (*thread_entry)(uint32_t fn, uint32_t arg);   /* first call of a guest thread */
    wasm_rt_memory_t *(*memory)(void);
    uint32_t *(*stack_pointer)(void);                  /* the guest's shadow stack pointer */
    void (*spin)(void);                                /* the game spins: deliver interrupts */
    const GcnwSymbol *symbols;                         /* sorted by name; may be NULL */
    uint32_t symbol_count;
} GcnwModule;

/* registry (gcnw_backend.c) */
void gcnw_register_module(const GcnwModule *module);
/* by package id; NULL or "" gives the first registered game */
const GcnwModule *gcnw_find_module(const char *package);

#ifdef __cplusplus
}
#endif

#endif /* GCNW_MODULE_H */
