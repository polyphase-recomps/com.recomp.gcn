/*
 * Odds and ends games link against: console output of the C library, Metrowerks' block
 * copy helpers, a THP audio fallback.
 */
#include <dolphin.h>
#include <stddef.h>

#include "gcn_guest.h"

#define WEAK __attribute__((weak))

/* MSL console: stdout / stderr lines end up in the host log */
static char sLine[256];
static int sLineLen;

static void put_chars(const unsigned char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (buf[i] == '\n' || sLineLen == (int)sizeof(sLine) - 1)
        {
            sLine[sLineLen] = 0;
            gcn_host_log(sLine);
            sLineLen = 0;
            if (buf[i] == '\n') continue;
        }
        sLine[sLineLen++] = (char)buf[i];
    }
}

WEAK int __write_console(u32 handle, unsigned char *buffer, size_t *count, void *idle_proc)
{
    put_chars(buffer, *count);
    return 0;
}

WEAK int __TRK_write_console(u32 handle, unsigned char *buffer, size_t *count, void *idle_proc)
{
    put_chars(buffer, *count);
    return 0;
}

WEAK int __close_console(u32 handle) { return 0; }
WEAK int __read_console(u32 handle, unsigned char *buffer, size_t *count, void *idle_proc)
{
    *count = 0;
    return 0;
}

WEAK void InitializeUART(void) {}
WEAK int WriteUARTN(const void *buf, u32 len)
{
    put_chars((const unsigned char *)buf, len);
    return 0;
}

/* Metrowerks runtime block copies (used by its memcpy / memmove) */
WEAK void __copy_longs_aligned(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
WEAK void __copy_longs_unaligned(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
WEAK void __copy_longs_rev_aligned(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
WEAK void __copy_longs_rev_unaligned(void *dst, const void *src, size_t n) { memmove(dst, src, n); }

/* THP video: guest/sdk/thp.c; audio frames are the SDK's own (THPAudio.c) when the game
 * links it */
WEAK s32 THPAudioDecode(s16 *buffer, u8 *audioFrame, s32 flag) { return 0; }

/* MSL's malloc asks the OS for memory through these; games use OSAlloc instead */
WEAK void *__sys_alloc(size_t size) { return 0; }
WEAK void __sys_free(void *ptr) {}
WEAK void (*__stdio_exit)(void) = 0;
