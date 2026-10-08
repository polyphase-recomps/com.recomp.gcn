/*
 * Mods for games on the com.recomp.gcn runtime.
 *
 * A mod is a C file in the game package's Native/mods/ (compiled with the game, so it
 * sees the decomp's headers and globals). It defines
 *
 *     void mod_<file name>_init(void)
 *
 * which runs once before the game's main(), and uses the calls below to hook frames and
 * to talk to Polyphase scripts (Lua table `Gcn`). The recomp builds compile the same mods
 * into their HLE module, with GCN_RECOMP defined to 1 (Docs/Modding.md):
 *
 *   - variables: named views of game memory scripts read and write by name
 *     (Gcn.Read("lives"), Gcn.Write("lives", 9)); every global of the decomp is reachable
 *     by its own name anyway, published variables add short names, help and arrays.
 *   - requests: named functions scripts call (Gcn.Request("warp", 12)); they run in the
 *     game's main thread at the start of a frame, where calling game code is safe.
 *   - events: the mod tells scripts something happened (Gcn.Events()).
 *   - frame hooks: run every game frame (start of the frame, before the game reads the
 *     controllers), e.g. to change input or patch memory continuously.
 */
#ifndef GCN_MOD_H
#define GCN_MOD_H

#ifdef __cplusplus
extern "C" {
#endif

/* variable types */
enum
{
    GCN_VAR_U8 = 1,
    GCN_VAR_S8,
    GCN_VAR_U16,
    GCN_VAR_S16,
    GCN_VAR_U32,
    GCN_VAR_S32,
    GCN_VAR_F32,
    GCN_VAR_STR, /* char array of `stride` bytes per element */
};

/* request handlers return what Gcn.Result hands to the script */
typedef int (*GcnModRequestFn)(const int *args, int nargs);
typedef void (*GcnModFrameFn)(void);

/* count: elements (1 for a scalar); stride: bytes between elements, 0 = the type's size */
void gcn_mod_variable(const char *name, void *addr, int type, int count, int stride, const char *help);
void gcn_mod_request(const char *name, GcnModRequestFn fn, const char *help);
void gcn_mod_on_frame(GcnModFrameFn fn);
void gcn_mod_emit(const char *name, const int *args, int nargs);
void gcn_mod_log(const char *fmt, ...);

/* Controller 0 as the game will see it this frame (PAD_BUTTON_* bits): frame hooks may
 * read and change it. */
unsigned short gcn_mod_pad_buttons(void);
void gcn_mod_set_pad_buttons(unsigned short buttons);
/* Controller 0's main stick this frame, -128..127 each (up and right positive). */
void gcn_mod_pad_stick(int *x, int *y);
void gcn_mod_set_pad_stick(int x, int y);
/* ... its C-stick (the host's right stick), the same way */
void gcn_mod_pad_substick(int *x, int *y);
void gcn_mod_set_pad_substick(int x, int y);
/* ... its analog L and R triggers, 0..255 */
void gcn_mod_pad_triggers(int *l, int *r);
void gcn_mod_set_pad_triggers(int l, int r);
/* On-screen text: line 0..15 up from the bottom left of the picture, shown on every frame until it
 * changes; NULL or "" clears it. Digits, letters (lower case shows as upper case) and
 * . , : - + = _ / % ( ) ! ? */
void gcn_mod_overlay(int line, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* GCN_MOD_H */
