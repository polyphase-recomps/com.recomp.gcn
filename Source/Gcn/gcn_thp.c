/*
 * THP movie frames, decoded on the host (the guest's THPVideoDecode calls in here): the
 * decoder runs as native code on the frame's bytes in guest memory, instead of as
 * translated guest code where every table and coefficient access is a byte-swapped load.
 *
 * A THP video frame is a baseline JPEG: 4:2:0 YCbCr, three components, Huffman coded,
 * without the 0xFF00 byte stuffing of ordinary JPEG (the SDK reads the entropy-coded data
 * as plain words). The output matches the SDK's: Y, U and V each in GX I8 texture layout
 * (8x4 texel tiles, row-major), Y as wide as the frame, U and V half as wide and high, so
 * the game can use the buffers as textures directly.
 */
#include <stdint.h>
#include <string.h>

#include "gcn_thp.h"

#define LOOKAHEAD 9

typedef struct
{
    uint8_t valid;
    uint8_t vals[256];
    int32_t maxcode[18]; /* largest code of each length, -1 = none */
    int32_t valptr[17];
    int32_t mincode[17];
    /* codes of up to LOOKAHEAD bits by their next LOOKAHEAD bits: length << 8 | value
     * (0: a longer code, decoded bit by bit) */
    uint16_t fast[1 << LOOKAHEAD];
    /* AC tables: a whole coefficient (code and its magnitude bits) within LOOKAHEAD bits:
     * value << 8 | run << 4 | bits used (0: not that short) */
    int16_t fast_ac[1 << LOOKAHEAD];
} Huff;

typedef struct
{
    const uint8_t *p, *end;
    uint32_t acc; /* bits not consumed yet, msb first */
    int bits;
} Bits;

static const uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static uint32_t be16(const uint8_t *p) { return (uint32_t)(p[0] << 8 | p[1]); }

static void huff_build(Huff *h, const uint8_t *counts, const uint8_t *vals, int nvals)
{
    int len, k = 0, i;
    int32_t code = 0;

    memcpy(h->vals, vals, (size_t)nvals);
    for (len = 1; len <= 16; len++)
    {
        int n = counts[len - 1];
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += n;
        k += n;
        h->maxcode[len] = n ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    memset(h->fast, 0, sizeof(h->fast));
    for (len = 1; len <= LOOKAHEAD; len++)
    {
        int32_t c;
        for (c = h->mincode[len]; h->maxcode[len] >= 0 && c <= h->maxcode[len]; c++)
        {
            /* every LOOKAHEAD-bit pattern that starts with this code */
            const int shift = LOOKAHEAD - len;
            int j;
            for (j = 0; j < (1 << shift); j++)
                h->fast[(c << shift) | j] = (uint16_t)(len << 8 | h->vals[h->valptr[len] + c - h->mincode[len]]);
        }
    }
    memset(h->fast_ac, 0, sizeof(h->fast_ac));
    for (i = 0; i < (1 << LOOKAHEAD); i++)
    {
        const int hit = h->fast[i], clen = hit >> 8, rs = hit & 0xFF, run = rs >> 4, size = rs & 15;
        int32_t v;
        if (!hit || !size || clen + size > LOOKAHEAD) continue;
        v = (i >> (LOOKAHEAD - clen - size)) & ((1 << size) - 1);
        if (v < (1 << (size - 1))) v -= (1 << size) - 1; /* extend() */
        if (v < -128 || v > 127) continue;
        h->fast_ac[i] = (int16_t)(v * 256 + (run << 4) + clen + size);
    }
    h->valid = 1;
}

static void fill(Bits *b)
{
    while (b->bits <= 24)
    {
        uint32_t byte = b->p < b->end ? *b->p++ : 0;
        b->acc |= byte << (24 - b->bits);
        b->bits += 8;
    }
}

static uint32_t get_bits(Bits *b, int n)
{
    uint32_t v;

    if (!n) return 0;
    fill(b);
    v = b->acc >> (32 - n);
    b->acc <<= n;
    b->bits -= n;
    return v;
}

static int decode(Bits *b, const Huff *h)
{
    int32_t code = 0;
    int len;
    uint32_t hit;

    fill(b);
    hit = h->fast[b->acc >> (32 - LOOKAHEAD)];
    if (hit)
    {
        b->acc <<= hit >> 8;
        b->bits -= (int)(hit >> 8);
        return (int)(hit & 0xFF);
    }
    for (len = 1; len <= 16; len++)
    {
        code = (code << 1) | (int32_t)(b->acc >> 31);
        b->acc <<= 1;
        b->bits--;
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len]) return h->vals[h->valptr[len] + code - h->mincode[len]];
    }
    return 0; /* corrupt data: carry on with a zero */
}

static int32_t extend(uint32_t v, int s)
{
    return s && v < (1u << (s - 1)) ? (int32_t)v - (int32_t)(1u << s) + 1 : (int32_t)v;
}

/* Integer inverse DCT, separable (the IJG "islow" factorisation): constants in 12-bit
 * fixed point, columns first keeping 2 extra bits, then rows. */
#define FIX(x) ((int)((x) * 4096.0f + 0.5f))
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                                                       \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                                                           \
    p2 = s2;                                                                                                          \
    p3 = s6;                                                                                                          \
    p1 = (p2 + p3) * FIX(0.5411961f);                                                                                 \
    t2 = p1 + p3 * FIX(-1.847759065f);                                                                                \
    t3 = p1 + p2 * FIX(0.765366865f);                                                                                 \
    p2 = s0;                                                                                                          \
    p3 = s4;                                                                                                          \
    t0 = (p2 + p3) * 4096;                                                                                            \
    t1 = (p2 - p3) * 4096;                                                                                            \
    x0 = t0 + t3;                                                                                                     \
    x3 = t0 - t3;                                                                                                     \
    x1 = t1 + t2;                                                                                                     \
    x2 = t1 - t2;                                                                                                     \
    t0 = s7;                                                                                                          \
    t1 = s5;                                                                                                          \
    t2 = s3;                                                                                                          \
    t3 = s1;                                                                                                          \
    p3 = t0 + t2;                                                                                                     \
    p4 = t1 + t3;                                                                                                     \
    p1 = t0 + t3;                                                                                                     \
    p2 = t1 + t2;                                                                                                     \
    p5 = (p3 + p4) * FIX(1.175875602f);                                                                               \
    t0 = t0 * FIX(0.298631336f);                                                                                      \
    t1 = t1 * FIX(2.053119869f);                                                                                      \
    t2 = t2 * FIX(3.072711026f);                                                                                      \
    t3 = t3 * FIX(1.501321110f);                                                                                      \
    p1 = p5 + p1 * FIX(-0.899976223f);                                                                                \
    p2 = p5 + p2 * FIX(-2.562915447f);                                                                                \
    p3 = p3 * FIX(-1.961570560f);                                                                                     \
    p4 = p4 * FIX(-0.390180644f);                                                                                     \
    t3 += p1 + p4;                                                                                                    \
    t2 += p2 + p3;                                                                                                    \
    t1 += p2 + p4;                                                                                                    \
    t0 += p1 + p3;

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void idct(const int *in, uint8_t out[64])
{
    int val[64], i, flat_rows = 1;

    for (i = 0; i < 8; i++)
    {
        const int *d = in + i;
        int *v = val + i;

        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56])
        {
            int dc = d[0] * 4; /* only the DC term: the column is flat */
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
            if (i && dc) flat_rows = 0;
        }
        else
        {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10;
            v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10;
            v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10;
            v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10;
            v[32] = (x3 - t0) >> 10;
            if (i) flat_rows = 0;
        }
    }
    if (flat_rows)
    {
        /* only the first column: each row is its first value, what the row pass gives */
        for (i = 0; i < 8; i++) memset(out + i * 8, clamp8((val[i * 8] * 4096 + 65536 + (128 << 17)) >> 17), 8);
        return;
    }
    for (i = 0; i < 8; i++)
    {
        const int *v = val + i * 8;
        uint8_t *o = out + i * 8;

        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        /* 1<<12 constants, 1<<2 kept from the columns, 1<<3 from the two sqrt(8) scales:
         * round, and add 128 for the level shift */
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = clamp8((x0 + t3) >> 17);
        o[7] = clamp8((x0 - t3) >> 17);
        o[1] = clamp8((x1 + t2) >> 17);
        o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17);
        o[5] = clamp8((x2 - t1) >> 17);
        o[3] = clamp8((x3 + t0) >> 17);
        o[4] = clamp8((x3 - t0) >> 17);
    }
}

/* one 8x8 block: entropy decode, dequantise, inverse DCT, store into an I8 tiled plane */
static void block(Bits *b, const Huff *dc, const Huff *ac, const uint16_t *q, int32_t *pred, uint8_t *plane,
                  uint32_t width, uint32_t bx, uint32_t by)
{
    int coef[64], k, s, y, any_ac = 0;
    uint8_t px[64];

    memset(coef, 0, sizeof(coef));
    s = decode(b, dc);
    *pred += extend(get_bits(b, s), s);
    coef[0] = *pred * q[0];
    for (k = 1; k < 64;)
    {
        int rs, r, fa;
        fill(b);
        fa = ac->fast_ac[b->acc >> (32 - LOOKAHEAD)];
        if (fa)
        {
            b->acc <<= fa & 15;
            b->bits -= fa & 15;
            k += (fa >> 4) & 15;
            if (k > 63) break;
            coef[kZigzag[k]] = (fa >> 8) * q[kZigzag[k]];
            any_ac = 1;
            k++;
            continue;
        }
        rs = decode(b, ac);
        r = rs >> 4;
        s = rs & 15;
        if (!s)
        {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) break;
        coef[kZigzag[k]] = extend(get_bits(b, s), s) * q[kZigzag[k]];
        any_ac = 1;
        k++;
    }
    if (!any_ac)
    {
        /* only the DC term (flat areas, common in video): what idct() gives for it */
        memset(px, clamp8((coef[0] * 16384 + 65536 + (128 << 17)) >> 17), sizeof(px));
    }
    else
    {
        idct(coef, px);
    }
    /* bx is a multiple of 8: each row of the block is one 8-texel row of a tile */
    for (y = 0; y < 8; y++)
    {
        uint32_t py = by + (uint32_t)y;
        memcpy(plane + ((py >> 2) * (width >> 3) + (bx >> 3)) * 32 + (py & 3) * 8, px + y * 8, 8);
    }
}

int gcn_thp_decode(const uint8_t *file, size_t file_cap, uint8_t *tile_y, uint8_t *tile_u, uint8_t *tile_v,
                   size_t plane_cap, size_t *luma_bytes)
{
    const uint8_t *c = file, *file_end = file + file_cap;
    Huff huff[4]; /* DC 0, AC 0, DC 1, AC 1 (index = id * 2 + class) */
    uint16_t quant[4][64];
    uint8_t qsel[3] = {0, 1, 1}, dcsel[3] = {0, 1, 1}, acsel[3] = {0, 1, 1};
    uint32_t w = 0, h = 0, restart = 0;
    int i;

    *luma_bytes = 0;
    memset(huff, 0, sizeof(huff));
    memset(quant, 0, sizeof(quant));
    for (;;)
    {
        uint8_t m;

        if (c + 4 > file_end) return 3;
        if (*c++ != 0xFF) return 3;
        while (*c == 0xFF && c < file_end) c++;
        m = *c++;
        if (m == 0xD8) continue; /* SOI */
        if (m == 0xC4)           /* DHT */
        {
            const uint8_t *end = c + be16(c);
            c += 2;
            while (c < end)
            {
                int tc = c[0] >> 4, th = c[0] & 15, n = 0;
                for (i = 0; i < 16; i++) n += c[1 + i];
                if (th < 2) huff_build(&huff[th * 2 + tc], c + 1, c + 17, n);
                c += 17 + n;
            }
            c = end;
        }
        else if (m == 0xDB) /* DQT */
        {
            const uint8_t *end = c + be16(c);
            c += 2;
            while (c < end)
            {
                int pq = c[0] >> 4, tq = c[0] & 3;
                c++;
                for (i = 0; i < 64; i++) quant[tq][kZigzag[i]] = pq ? (uint16_t)be16(c + i * 2) : c[i];
                c += pq ? 128 : 64;
            }
            c = end;
        }
        else if (m == 0xC0) /* SOF0 */
        {
            if (c[2] != 8 || c[7] != 3) return 12;
            h = be16(c + 3);
            w = be16(c + 5);
            for (i = 0; i < 3; i++) qsel[i] = c[8 + i * 3 + 2] & 3;
            c += be16(c);
        }
        else if (m == 0xDD) /* DRI */
        {
            restart = be16(c + 2);
            c += be16(c);
        }
        else if (m == 0xDA) /* SOS: the entropy-coded data follows */
        {
            if (c[2] != 3) return 12;
            for (i = 0; i < 3; i++)
            {
                dcsel[i] = (c[3 + i * 2 + 1] >> 4) & 1;
                acsel[i] = c[3 + i * 2 + 1] & 1;
            }
            c += be16(c);
            break;
        }
        else if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE)
        {
            c += be16(c);
        }
        else
        {
            return 11;
        }
    }
    if (!w || !h || (w & 15) || (h & 15) || (size_t)w * h > plane_cap) return 3;
    for (i = 0; i < 3; i++)
        if (!huff[dcsel[i] * 2].valid || !huff[acsel[i] * 2 + 1].valid) return 15;
    *luma_bytes = (size_t)w * h;

    {
        Bits b;
        int32_t pred[3] = {0, 0, 0};
        uint32_t mx, my, left = restart;

        b.p = c;
        b.end = file_end;
        b.acc = 0;
        b.bits = 0;
        for (my = 0; my < h; my += 16)
        {
            for (mx = 0; mx < w; mx += 16)
            {
                const Huff *dy = &huff[dcsel[0] * 2], *ay = &huff[acsel[0] * 2 + 1];
                block(&b, dy, ay, quant[qsel[0]], &pred[0], tile_y, w, mx, my);
                block(&b, dy, ay, quant[qsel[0]], &pred[0], tile_y, w, mx + 8, my);
                block(&b, dy, ay, quant[qsel[0]], &pred[0], tile_y, w, mx, my + 8);
                block(&b, dy, ay, quant[qsel[0]], &pred[0], tile_y, w, mx + 8, my + 8);
                block(&b, &huff[dcsel[1] * 2], &huff[acsel[1] * 2 + 1], quant[qsel[1]], &pred[1], tile_u, w / 2, mx / 2, my / 2);
                block(&b, &huff[dcsel[2] * 2], &huff[acsel[2] * 2 + 1], quant[qsel[2]], &pred[2], tile_v, w / 2, mx / 2, my / 2);
                if (restart && --left == 0)
                {
                    /* restart interval: the data resumes at the next byte, predictions reset */
                    left = restart;
                    b.p -= b.bits >> 3;
                    b.acc = 0;
                    b.bits = 0;
                    pred[0] = pred[1] = pred[2] = 0;
                }
            }
        }
    }
    return 0;
}
