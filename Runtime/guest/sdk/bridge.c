/*
 * Guest side of the mod API (gcn_mod.h) and of the script bridge.
 *
 * Mods register variables, requests and frame hooks from their init functions; the
 * tables are handed to the host (gcn_host_bridge_publish) every time they change. Once
 * per game frame - when the game reads its controllers (PADRead) - queued script
 * requests run and the frame hooks are called, in the game's main thread.
 */
#include <dolphin.h>
#include <stdarg.h>
#include <string.h>

#include "gcn_guest.h"
#include "gcn_mod.h"
#include "gcn_sdk.h"

#define MAX_VARS 256
#define MAX_REQUESTS 128
#define MAX_HOOKS 32

/* read by the host from guest memory: keep in step with gcnw_backend.c */
typedef struct
{
    const char *name;
    void *addr;
    s32 type;
    s32 count;
    s32 stride;
    const char *help;
} BridgeVar;

typedef struct
{
    const char *name;
    const char *help;
} BridgeRequest;

static BridgeVar sVars[MAX_VARS];
static int sVarCount;
static BridgeRequest sRequests[MAX_REQUESTS];
static GcnModRequestFn sHandlers[MAX_REQUESTS];
static int sRequestCount;
static GcnModFrameFn sHooks[MAX_HOOKS];
static int sHookCount;
static PADStatus *sPads; /* the statuses PADRead fills, while hooks run */

static void publish(void)
{
    gcn_host_bridge_publish(sVars, sVarCount, sRequests, sRequestCount);
}

static int type_size(int type)
{
    switch (type)
    {
    case GCN_VAR_U8: case GCN_VAR_S8: case GCN_VAR_STR: return 1;
    case GCN_VAR_U16: case GCN_VAR_S16: return 2;
    default: return 4;
    }
}

void gcn_mod_variable(const char *name, void *addr, int type, int count, int stride, const char *help)
{
    BridgeVar *v;

    if (sVarCount >= MAX_VARS)
    {
        gcn_logf("mods: too many variables, %s left out", name);
        return;
    }
    v = &sVars[sVarCount++];
    v->name = name;
    v->addr = addr;
    v->type = type;
    v->count = count > 0 ? count : 1;
    v->stride = stride > 0 ? stride : type_size(type);
    v->help = help ? help : "";
    publish();
}

void gcn_mod_request(const char *name, GcnModRequestFn fn, const char *help)
{
    if (sRequestCount >= MAX_REQUESTS)
    {
        gcn_logf("mods: too many requests, %s left out", name);
        return;
    }
    sRequests[sRequestCount].name = name;
    sRequests[sRequestCount].help = help ? help : "";
    sHandlers[sRequestCount] = fn;
    sRequestCount++;
    publish();
}

void gcn_mod_on_frame(GcnModFrameFn fn)
{
    if (sHookCount < MAX_HOOKS) sHooks[sHookCount++] = fn;
}

void gcn_mod_emit(const char *name, const int *args, int nargs)
{
    gcn_host_bridge_emit(name, args, nargs);
}

void gcn_mod_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    gcn_logf("mod: %s", buf);
}

unsigned short gcn_mod_pad_buttons(void)
{
    return sPads ? sPads[0].button : 0;
}

void gcn_mod_set_pad_buttons(unsigned short buttons)
{
    if (sPads) sPads[0].button = buttons;
}

void gcn_mod_pad_stick(int *x, int *y)
{
    if (x) *x = sPads ? sPads[0].stickX : 0;
    if (y) *y = sPads ? sPads[0].stickY : 0;
}

void gcn_mod_set_pad_stick(int x, int y)
{
    if (!sPads) return;
    sPads[0].stickX = (s8)(x < -128 ? -128 : x > 127 ? 127 : x);
    sPads[0].stickY = (s8)(y < -128 ? -128 : y > 127 ? 127 : y);
}

void gcn_mod_overlay(int line, const char *text)
{
    gcn_host_overlay(line, text ? text : "");
}

/* PADRead: requests from scripts, then the frame hooks */
void gcn_bridge_frame(void *pad_statuses)
{
    PADStatus *pads = (PADStatus *)pad_statuses;
    char name[64];
    int args[8], nargs, id, i;

    sPads = pads;
    while ((id = gcn_host_bridge_poll(name, sizeof(name), args, 8, &nargs)) > 0)
    {
        int result = -0x7FFFFFFF; /* unknown request */
        for (i = 0; i < sRequestCount; i++)
        {
            if (!strcmp(sRequests[i].name, name))
            {
                result = sHandlers[i](args, nargs);
                break;
            }
        }
        gcn_host_bridge_done(id, result);
    }
    for (i = 0; i < sHookCount; i++) sHooks[i]();
    sPads = 0;
}
