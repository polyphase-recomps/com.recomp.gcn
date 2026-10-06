/*
 * THP movie frames (THPInit / THPVideoDecode): the SDK's decoder is paired-single assembly
 * writing through the locked cache; here the host decodes the frame (Source/Gcn/gcn_thp.c,
 * native code on the frame's bytes) through gcn_host_thp_decode.
 *
 * A THP video frame is a baseline JPEG: 4:2:0 YCbCr, three components, Huffman coded,
 * without the 0xFF00 byte stuffing of ordinary JPEG (the SDK reads the entropy-coded data
 * as plain words). The output matches the SDK's: Y, U and V each in GX I8 texture layout
 * (8x4 texel tiles, row-major), Y as wide as the frame, U and V half as wide and high, so
 * the game can use the buffers as textures directly.
 */
#include <dolphin.h>
#include <dolphin/thp.h>
#include <string.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

static BOOL sInit;

BOOL THPInit(void)
{
    sInit = TRUE;
    return TRUE;
}

s32 THPVideoDecode(void *file, void *tileY, void *tileU, void *tileV, void *work)
{
    (void)work;
    if (!file) return 25;
    if (!tileY || !tileU || !tileV) return 27;
    if (!sInit) return 29;
    return gcn_host_thp_decode(file, tileY, tileU, tileV);
}
