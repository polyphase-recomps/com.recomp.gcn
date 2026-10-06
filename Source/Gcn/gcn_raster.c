/*
 * GameCube GPU software rasteriser: XF (transform, lighting, texture coordinate
 * generation), setup and rasterisation, texture decoding and sampling, the TEV and the
 * pixel engine, writing the embedded frame buffer. Register meanings follow the
 * hardware's (BP / XF / CP) as the game's GX library programs them.
 *
 * Vertices are transformed as the game issues them; the triangles are queued with a
 * snapshot of the state they need (PrimState) and drawn in parallel at the next flush
 * (EFB copy, texture change), each thread owning interleaved bands of rows.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcn_raster.h"
#include "gcn_gpu.h"
#include "gcnw.h"

static GcnGpuRegs R;

#define BP(r) (R.bp[(r)])
#define XF(a) (R.xf[(a)])
#define BITS(v, lo, n) (((v) >> (lo)) & ((1u << (n)) - 1u))

static float xff(uint32_t addr)
{
    float f;
    uint32_t v = R.xf[addr];
    memcpy(&f, &v, 4);
    return f;
}

void gcn_raster_init(const GcnGpuRegs *regs) { R = *regs; }

/* ---- textures -------------------------------------------------------------------------- */
typedef struct
{
    uint32_t addr, fmt, w, h, tlut, tlutfmt, key;
    uint32_t *px; /* RGBA8 (r in the low byte), w x h */
    uint64_t hash;  /* of the source texels (and palette) the decode came from */
    uint32_t epoch; /* sEpoch when the hash was last checked */
    int used;
    int next;       /* next entry in the same lookup bucket, -1 = none */
} TexEntry;

#define TEX_CACHE 512
static TexEntry sTex[TEX_CACHE];
static int sTexCount;
/* Texture memory may have changed (the game invalidated its texture cache, loaded a
 * palette, or a frame ended): every entry checks its source again on its next use and
 * decodes anew only if the texels changed. */
static uint32_t sEpoch = 1;

void gcn_raster_invalidate_textures(void)
{
    sEpoch++;
}

#define TEX_BUCKETS 1024
static int sTexBucket[TEX_BUCKETS]; /* first entry + 1, 0 = empty */

static uint32_t tex_key(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h)
{
    return ((addr >> 5) * 2654435761u ^ (fmt << 24) ^ (w << 12) ^ h) & (TEX_BUCKETS - 1);
}

void gcn_raster_flush(void);

static int sTexGeneration; /* counts free_textures: pointers taken before are gone */

/* texel buffers replaced while queued triangles may still sample them: freed once the
 * queue is drawn (gcn_raster_flush) */
static uint32_t **sRetired;
static int sRetiredCount, sRetiredCap;

static void retire(uint32_t *px)
{
    if (sRetiredCount == sRetiredCap)
    {
        sRetiredCap = sRetiredCap ? sRetiredCap * 2 : 64;
        sRetired = (uint32_t **)realloc(sRetired, sizeof(*sRetired) * (size_t)sRetiredCap);
    }
    sRetired[sRetiredCount++] = px;
}

static void free_retired(void)
{
    int i;
    for (i = 0; i < sRetiredCount; i++) free(sRetired[i]);
    sRetiredCount = 0;
}

static void free_textures(void)
{
    int i;
    gcn_raster_flush();
    sTexGeneration++;
    for (i = 0; i < sTexCount; i++) free(sTex[i].px);
    sTexCount = 0;
    memset(sTexBucket, 0, sizeof(sTexBucket));
}

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
static int c5(int v) { return (v << 3) | (v >> 2); }
static int c6(int v) { return (v << 2) | (v >> 4); }
static int c4(int v) { return (v << 4) | v; }
static int c3(int v) { return (v << 5) | (v << 2) | (v >> 1); }

static uint32_t from565(uint16_t v) { return rgba(c5(v >> 11), c6((v >> 5) & 63), c5(v & 31), 255); }

static uint32_t from5a3(uint16_t v)
{
    if (v & 0x8000) return rgba(c5((v >> 10) & 31), c5((v >> 5) & 31), c5(v & 31), 255);
    return rgba(c4((v >> 8) & 15), c4((v >> 4) & 15), c4(v & 15), c3((v >> 12) & 7));
}

static uint32_t tlut_color(uint32_t tlut, uint32_t fmt, uint32_t index)
{
    const uint8_t *p = R.tmem + ((tlut + index * 2) & 0xFFFFF);
    uint16_t v = (uint16_t)((p[0] << 8) | p[1]);

    switch (fmt)
    {
    case 0: return rgba(p[1], p[1], p[1], p[0]); /* IA8: intensity low byte, alpha high */
    case 1: return from565(v);
    default: return from5a3(v);
    }
}

static const uint8_t *guest(uint32_t addr) { return R.mem + GCNW_OFFSET(addr); }

static void decode(TexEntry *t)
{
    uint32_t w = t->w, h = t->h, x, y, bx, by;
    const uint8_t *src = guest(t->addr);
    uint32_t *out = t->px;

#define PUT(xx, yy, c) do { if ((xx) < w && (yy) < h) out[(yy) * w + (xx)] = (c); } while (0)
    switch (t->fmt)
    {
    case 0: /* I4: 8x8 tiles */
        for (by = 0; by < h; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x += 2, src++)
                    {
                        int a = c4(*src >> 4), b = c4(*src & 15);
                        PUT(bx + x, by + y, rgba(a, a, a, a));
                        PUT(bx + x + 1, by + y, rgba(b, b, b, b));
                    }
        break;
    case 1: /* I8: 8x4 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 8; x++, src++) PUT(bx + x, by + y, rgba(*src, *src, *src, *src));
        break;
    case 2: /* IA4: 8x4 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 8; x++, src++)
                    {
                        int i = c4(*src & 15);
                        PUT(bx + x, by + y, rgba(i, i, i, c4(*src >> 4)));
                    }
        break;
    case 3: /* IA8: 4x4 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++, src += 2) PUT(bx + x, by + y, rgba(src[1], src[1], src[1], src[0]));
        break;
    case 4: /* RGB565 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++, src += 2) PUT(bx + x, by + y, from565((uint16_t)((src[0] << 8) | src[1])));
        break;
    case 5: /* RGB5A3 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++, src += 2) PUT(bx + x, by + y, from5a3((uint16_t)((src[0] << 8) | src[1])));
        break;
    case 6: /* RGBA8: 4x4 tiles of AR then GB */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4, src += 64)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++)
                    {
                        int i = (y * 4 + x) * 2;
                        PUT(bx + x, by + y, rgba(src[i + 1], src[32 + i], src[32 + i + 1], src[i]));
                    }
        break;
    case 8: /* C4: 8x8 */
        for (by = 0; by < h; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x += 2, src++)
                    {
                        PUT(bx + x, by + y, tlut_color(t->tlut, t->tlutfmt, *src >> 4));
                        PUT(bx + x + 1, by + y, tlut_color(t->tlut, t->tlutfmt, *src & 15));
                    }
        break;
    case 9: /* C8: 8x4 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 8; x++, src++) PUT(bx + x, by + y, tlut_color(t->tlut, t->tlutfmt, *src));
        break;
    case 10: /* C14X2: 4x4 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++, src += 2)
                        PUT(bx + x, by + y, tlut_color(t->tlut, t->tlutfmt, ((src[0] << 8) | src[1]) & 0x3FFF));
        break;
    case 14: /* CMPR: 8x8 tiles of four 4x4 DXT1 blocks */
        for (by = 0; by < h; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; y += 4)
                    for (x = 0; x < 8; x += 4, src += 8)
                    {
                        uint16_t c0 = (uint16_t)((src[0] << 8) | src[1]), c1 = (uint16_t)((src[2] << 8) | src[3]);
                        uint32_t pal[4], a = from565(c0), b = from565(c1);
                        int i, j, k;

                        pal[0] = a;
                        pal[1] = b;
                        for (k = 0; k < 3; k++)
                        {
                            int ca = (a >> (k * 8)) & 255, cb = (b >> (k * 8)) & 255;
                            if (c0 > c1)
                            {
                                ((uint8_t *)&pal[2])[k] = (uint8_t)((2 * ca + cb) / 3);
                                ((uint8_t *)&pal[3])[k] = (uint8_t)((ca + 2 * cb) / 3);
                            }
                            else
                            {
                                ((uint8_t *)&pal[2])[k] = (uint8_t)((ca + cb) / 2);
                                ((uint8_t *)&pal[3])[k] = 0;
                            }
                        }
                        ((uint8_t *)&pal[2])[3] = 255;
                        ((uint8_t *)&pal[3])[3] = (uint8_t)(c0 > c1 ? 255 : 0);
                        for (j = 0; j < 4; j++)
                        {
                            uint8_t row = src[4 + j];
                            for (i = 0; i < 4; i++) PUT(bx + x + i, by + y + j, pal[(row >> (6 - i * 2)) & 3]);
                        }
                    }
        break;
    default:
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) PUT(x, y, rgba(255, 0, 255, 255));
        break;
    }
#undef PUT
}

/* bytes of a texture in guest memory (whole tiles) */
static uint32_t tex_bytes(uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t tw = 4, th = 4, bpp = 16;

    switch (fmt)
    {
    case 0: case 8: case 14: tw = 8; th = 8; bpp = 4; break; /* I4, C4, CMPR */
    case 1: case 2: case 9: tw = 8; th = 4; bpp = 8; break;  /* I8, IA4, C8 */
    case 6: bpp = 32; break;                                 /* RGBA8 */
    default: break;                                          /* 16 bits, 4x4 */
    }
    return ((w + tw - 1) / tw * tw) * ((h + th - 1) / th * th) * bpp / 8;
}

static uint64_t hash_bytes(uint64_t h, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    uint64_t v;

    for (i = 0; i + 8 <= n; i += 8)
    {
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t tex_hash(const TexEntry *t)
{
    uint64_t h = hash_bytes(0xCBF29CE484222325ull, guest(t->addr), tex_bytes(t->fmt, t->w, t->h));

    if (t->fmt >= 8 && t->fmt <= 10)
    {
        uint32_t entries = t->fmt == 8 ? 16 : t->fmt == 9 ? 256 : 16384;
        uint32_t off = t->tlut & 0xFFFFF, n = entries * 2;
        if (off + n > 0x100000) n = 0x100000 - off;
        h = hash_bytes(h, R.tmem + off, n);
    }
    return h;
}

static TexEntry *texture(int map)
{
    static const uint8_t img0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
    static const uint8_t img3[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
    static const uint8_t tlutr[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
    uint32_t i0 = BP(img0[map]), addr = (BP(img3[map]) & 0xFFFFFF) << 5;
    uint32_t w = BITS(i0, 0, 10) + 1, h = BITS(i0, 10, 10) + 1, fmt = BITS(i0, 20, 4);
    uint32_t tl = BP(tlutr[map]), tlut = (BITS(tl, 0, 10) << 9), tlutfmt = BITS(tl, 10, 2);
    uint32_t key = tex_key(addr, fmt, w, h);
    int i;

    for (i = sTexBucket[key] - 1; i >= 0; i = sTex[i].next)
    {
        TexEntry *t = &sTex[i];
        if (t->addr == addr && t->fmt == fmt && t->w == w && t->h == h &&
            (fmt < 8 || fmt > 10 || (t->tlut == tlut && t->tlutfmt == tlutfmt)))
        {
            if (t->epoch != sEpoch)
            {
                uint64_t now = tex_hash(t);
                if (now != t->hash)
                {
                    /* queued triangles may still sample the old texels: decode into a new
                     * buffer, the old one goes when the queue is drawn */
                    retire(t->px);
                    t->px = (uint32_t *)malloc((size_t)t->w * t->h * 4);
                    decode(t);
                    t->hash = now;
                }
                t->epoch = sEpoch;
            }
            return t;
        }
    }
    if (sTexCount == TEX_CACHE) free_textures();
    {
        TexEntry *t = &sTex[sTexCount];
        t->next = sTexBucket[key] - 1;
        sTexBucket[key] = ++sTexCount;
        t->addr = addr;
        t->fmt = fmt;
        t->w = w;
        t->h = h;
        t->tlut = tlut;
        t->tlutfmt = tlutfmt;
        t->px = (uint32_t *)malloc((size_t)w * h * 4);
        decode(t);
        t->hash = tex_hash(t);
        t->epoch = sEpoch;
        return t;
    }
}

static int wrap(int c, int size, int mode)
{
    if (mode == 0) return c < 0 ? 0 : c >= size ? size - 1 : c; /* clamp */
    if ((size & (size - 1)) == 0)                               /* power of two: masks */
    {
        if (mode == 2) return (c & size) ? (size - 1) - (c & (size - 1)) : (c & (size - 1));
        return c & (size - 1);
    }
    if (mode == 2) /* mirror */
    {
        int p = c / size, r = c % size;
        if (r < 0) { r += size; p--; }
        return (p & 1) ? size - 1 - r : r;
    }
    c %= size; /* repeat */
    return c < 0 ? c + size : c;
}

static int ifloor(float f)
{
    int i = (int)f;
    return i - (f < (float)i);
}

typedef struct
{
    int r, g, b, a;
} Col;

/* one TEV combiner (color or alpha) as setup_state decodes it */
typedef struct
{
    int bias, sub, scale, clampv;
} TevOp;


/* the values a pixel's TEV stages read and write: PREV, REG0-2 (11-bit signed), the
 * stage's texture color and rasterised color, constants, its konstant color */
enum { SL_PREV, SL_REG0, SL_REG1, SL_REG2, SL_TEX, SL_RAS, SL_ONE, SL_HALF, SL_KONST, SL_ZERO, SL_COUNT };

/* a TEV stage, decoded once per primitive (setup_state) */
typedef struct
{
    int texon, texmap, texcoord, chan; /* chan: 0 = color0, 1 = color1, else zero */
    int rswap[4], tswap[4];            /* component order of the ras / tex swap tables */
    Col kcol;                          /* konstant inputs this stage selects */
    int cin[4], ain[4];                /* input selectors a, b, c, d */
    TevOp cop, aop;
    int cdest, adest;
    /* the inputs resolved to the pixel's value slots (see shade): color input k reads
     * slot cslot[k], components ccomp[k][0..2] (rgb, or alpha three times); alpha input k
     * reads component 3 of slot aslot[k] */
    int cslot[4], ccomp[4][3], aslot[4];
} TevStage;

/* a texture map as the pixels sample it */
typedef struct
{
    const uint32_t *px;
    int w, h, ws, wt, linear;
    int mx, my; /* wrap masks for repeat on power-of-two sizes, -1 = use wrap() */
    float fw, fh;
} Sampler;

/* Everything drawing a primitive needs, captured when the game issues it: triangles are
 * queued and drawn later (in parallel), while the registers move on. */
typedef struct
{
    int sScX0, sScY0, sScX1, sScY1; /* scissor, EFB pixels */
    int sNumStages, sNumTex, sNumChan;
    int cull;                       /* gen mode reject: 0 none, 1 back, 2 front, 3 all */
    TevStage sStage[16];
    Col sReg[4];                    /* PREV, REG0..2 (11-bit signed) */
    TexEntry *sTexMap[8];
    int sTexWrapS[8], sTexWrapT[8], sTexLinear[8];
    int sZTest, sZFunc, sZUpd;
    int sBlend, sBlendSub, sBlendSF, sBlendDF, sColorUpd, sAlphaUpd, sPeFmt, sDstAlphaOn, sDstAlpha;
    int sAR0, sAR1, sAF0, sAF1, sAOp;
    /* derived for the pixel loop (setup_pixel_state) */
    Sampler smp[8];
    int tcused, tcproj;  /* texture coordinates the stages read / that need the divide by q */
    int chanused;        /* bit 0: color0, bit 1: color1 */
    int atest_always;    /* the alpha test passes every value */
    /* fog (BP 0xEE-0xF2): type 0 off, 2 linear, 4 exp, 5 exp2, 6/7 backwards exp/exp2 */
    int fog_type, fog_ortho, fog_bmag, fog_bshift, fog_r, fog_g, fog_b;
    float fog_a, fog_c;
} PrimState;

static PrimState sCur; /* what setup_state decoded for the primitive being issued */


static void resolve_texture(int map)
{
    static const uint8_t mode0r[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    uint32_t m0 = BP(mode0r[map]);

    sCur.sTexMap[map] = texture(map);
    sCur.sTexWrapS[map] = BITS(m0, 0, 2);
    sCur.sTexWrapT[map] = BITS(m0, 2, 2);
    sCur.sTexLinear[map] = BITS(m0, 4, 1);
}

/* a + (b - a) * f / 256 on all four 8-bit channels at once, two per 32-bit lane pair */
static inline uint32_t lerp4(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t g = 256 - f;
    uint32_t rb = (((a & 0x00FF00FFu) * g + (b & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * g + ((b >> 8) & 0x00FF00FFu) * f) & 0xFF00FF00u;
    return rb | ag;
}

static inline int wrap_x(const Sampler *t, int x) { return t->mx >= 0 ? (x & t->mx) : wrap(x, t->w, t->ws); }
static inline int wrap_y(const Sampler *t, int y) { return t->my >= 0 ? (y & t->my) : wrap(y, t->h, t->wt); }

static inline uint32_t sample(const Sampler *t, float s, float tt)
{
    float u = s * t->fw, v = tt * t->fh;

    if (!(u > -1e6f && u < 1e6f)) u = 0; /* also NaN */
    if (!(v > -1e6f && v < 1e6f)) v = 0;
    if (!t->linear) return t->px[wrap_y(t, ifloor(v)) * t->w + wrap_x(t, ifloor(u))];
    {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = ifloor(fu), y0 = ifloor(fv);
        uint32_t ax = (uint32_t)((fu - (float)x0) * 256.0f), ay = (uint32_t)((fv - (float)y0) * 256.0f);
        int xa = wrap_x(t, x0), xb = wrap_x(t, x0 + 1);
        const uint32_t *ra = t->px + wrap_y(t, y0) * t->w, *rb = t->px + wrap_y(t, y0 + 1) * t->w;
        return lerp4(lerp4(ra[xa], ra[xb], ax), lerp4(rb[xa], rb[xb], ax), ay);
    }
}

/* ---- XF: transform and lighting --------------------------------------------------------- */
typedef struct
{
    float x, y, z, w;        /* clip space */
    float col[2][4];         /* 0..255 */
    float tc[8][3];
} VtxOut;

static void mul34(uint32_t addr, const float *in, float *out)
{
    int r;
    for (r = 0; r < 3; r++)
    {
        uint32_t a = addr + r * 4;
        out[r] = xff(a) * in[0] + xff(a + 1) * in[1] + xff(a + 2) * in[2] + xff(a + 3) * in[3];
    }
}

/* XF lighting of a color channel, decoded once per primitive (setup_lighting) */
typedef struct
{
    float col[3], pos[3], dir[3], a[3], k[3]; /* color, position, direction, angle and distance attenuation */
} LightInfo;

typedef struct
{
    int mat_vtx, amb_vtx, alpha_vtx, lit, diffuse, atten, spot, nlights;
    float m[4], a[4];
    LightInfo l[8];
} ChanSetup;

static ChanSetup sChan[2];

static void setup_lighting(void)
{
    int chan, i, k;

    for (chan = 0; chan < 2; chan++)
    {
        ChanSetup *c = &sChan[chan];
        uint32_t cc = XF(0x100E + chan), ac = XF(0x1010 + chan);
        uint32_t mat = XF(0x100C + chan), amb = XF(0x100A + chan);
        uint32_t mask = BITS(cc, 2, 4) | (BITS(cc, 11, 4) << 4);

        for (k = 0; k < 4; k++)
        {
            c->m[k] = (float)((mat >> (24 - k * 8)) & 255);
            c->a[k] = (float)((amb >> (24 - k * 8)) & 255);
        }
        c->mat_vtx = BITS(cc, 0, 1);
        c->amb_vtx = BITS(cc, 6, 1);
        c->alpha_vtx = BITS(ac, 0, 1);
        c->lit = BITS(cc, 1, 1);
        c->diffuse = BITS(cc, 7, 2);
        c->atten = BITS(cc, 9, 1);
        c->spot = BITS(cc, 10, 1); /* else specular: not modelled, treated as spot */
        c->nlights = 0;
        if (!c->lit) continue;
        for (i = 0; i < 8; i++)
        {
            uint32_t lb = 0x600 + i * 16;
            LightInfo *l = &c->l[c->nlights];

            if (!(mask & (1u << i))) continue;
            for (k = 0; k < 3; k++)
            {
                l->col[k] = (float)((XF(lb + 3) >> (24 - k * 8)) & 255);
                l->a[k] = xff(lb + 4 + k);
                l->k[k] = xff(lb + 7 + k);
                l->pos[k] = xff(lb + 10 + k);
                l->dir[k] = xff(lb + 13 + k);
            }
            c->nlights++;
        }
    }
}

static void light_channel(int chan, const GcnVertexIn *in, const float *wpos, const float *wnrm, float *out)
{
    const ChanSetup *c = &sChan[chan];
    int has = in->has_col[chan], k, i;
    float m[4];

    for (k = 0; k < 4; k++) m[k] = c->m[k];
    if (c->mat_vtx)
        for (k = 0; k < 3; k++) m[k] = has ? in->col[chan][k] * 255.0f : 255.0f;
    if (c->alpha_vtx) m[3] = has ? in->col[chan][3] * 255.0f : 255.0f;

    if (c->lit)
    {
        float acc[3];

        for (k = 0; k < 3; k++) acc[k] = (c->amb_vtx && has) ? in->col[chan][k] * 255.0f : c->a[k];
        for (i = 0; i < c->nlights; i++)
        {
            const LightInfo *l = &c->l[i];
            float d[3], len, ndl, att = 1.0f;

            d[0] = l->pos[0] - wpos[0];
            d[1] = l->pos[1] - wpos[1];
            d[2] = l->pos[2] - wpos[2];
            len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0.0f)
            {
                float il = 1.0f / len;
                d[0] *= il; d[1] *= il; d[2] *= il;
            }
            ndl = d[0] * wnrm[0] + d[1] * wnrm[1] + d[2] * wnrm[2];
            if (c->diffuse == 0) ndl = 1.0f;
            else if (c->diffuse == 2 && ndl < 0) ndl = 0;
            if (c->atten)
            {
                float cosx = -(d[0] * l->dir[0] + d[1] * l->dir[1] + d[2] * l->dir[2]);
                float aa = c->spot ? l->a[0] + l->a[1] * cosx + l->a[2] * cosx * cosx : 1.0f;
                float dd = l->k[0] + l->k[1] * len + l->k[2] * len * len;
                att = dd != 0.0f ? (aa < 0 ? 0 : aa) / dd : 0.0f;
            }
            for (k = 0; k < 3; k++) acc[k] += l->col[k] * ndl * att;
        }
        for (k = 0; k < 3; k++)
        {
            float v = acc[k] < 0 ? 0 : acc[k] > 255 ? 255 : acc[k];
            out[k] = m[k] * v * (1.0f / 255.0f);
        }
    }
    else
    {
        for (k = 0; k < 3; k++) out[k] = m[k];
    }
    out[3] = m[3]; /* alpha lighting: material only (lit alpha is rare) */
}

static void transform(const GcnVertexIn *in, VtxOut *o)
{
    uint32_t mia = XF(0x1018), mib = XF(0x1019);
    int pm = in->pnmtx >= 0 ? in->pnmtx : (int)BITS(mia, 0, 6);
    float p[4] = {in->pos[0], in->pos[1], in->pos[2], 1.0f}, v[3], n[3] = {0, 0, 1};
    uint32_t pt = XF(0x1026);
    int i, k;

    mul34((uint32_t)pm * 4, p, v);
    if (in->has_nrm)
    {
        uint32_t na = 0x400 + (uint32_t)(pm & 31) * 3;
        float len;
        for (k = 0; k < 3; k++) n[k] = xff(na + k * 3) * in->nrm[0] + xff(na + k * 3 + 1) * in->nrm[1] + xff(na + k * 3 + 2) * in->nrm[2];
        len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }
    if (pt == 0)
    {
        o->x = xff(0x1020) * v[0] + xff(0x1021) * v[2];
        o->y = xff(0x1022) * v[1] + xff(0x1023) * v[2];
        o->z = xff(0x1024) * v[2] + xff(0x1025);
        o->w = -v[2];
    }
    else
    {
        o->x = xff(0x1020) * v[0] + xff(0x1021);
        o->y = xff(0x1022) * v[1] + xff(0x1023);
        o->z = xff(0x1024) * v[2] + xff(0x1025);
        o->w = 1.0f;
    }
    light_channel(0, in, v, n, o->col[0]);
    if (sCur.sNumChan >= 2)
        light_channel(1, in, v, n, o->col[1]);
    else
        o->col[1][0] = o->col[1][1] = o->col[1][2] = o->col[1][3] = 0.0f;
    for (i = 0; i < 8; i++)
    {
        uint32_t tg = XF(0x1040 + i);
        int type = BITS(tg, 4, 3), src = BITS(tg, 7, 5), stq = BITS(tg, 1, 1), abc1 = BITS(tg, 2, 1);
        int tm = in->texmtx[i] >= 0 ? in->texmtx[i]
                                    : (int)(i < 4 ? BITS(mia, 6 + i * 6, 6) : BITS(mib, (i - 4) * 6, 6));
        float s[4] = {0, 0, 1, 1}, r[3];

        if (i >= sCur.sNumTex) /* not generated */
        {
            o->tc[i][0] = o->tc[i][1] = 0.0f;
            o->tc[i][2] = 1.0f;
            continue;
        }
        if (type == 2 || type == 3) /* color channels as coordinates */
        {
            o->tc[i][0] = o->col[0][0] / 255.0f;
            o->tc[i][1] = o->col[0][1] / 255.0f;
            o->tc[i][2] = 1.0f;
            continue;
        }
        switch (src)
        {
        case 0: s[0] = v[0]; s[1] = v[1]; s[2] = v[2]; s[0] = in->pos[0]; s[1] = in->pos[1]; s[2] = in->pos[2]; break;
        case 1: s[0] = in->nrm[0]; s[1] = in->nrm[1]; s[2] = in->nrm[2]; break;
        case 3: s[0] = in->tan[0]; s[1] = in->tan[1]; s[2] = in->tan[2]; break;
        case 4: s[0] = in->bin[0]; s[1] = in->bin[1]; s[2] = in->bin[2]; break;
        default:
            if (src >= 5 && src <= 12)
            {
                s[0] = in->tex[src - 5][0];
                s[1] = in->tex[src - 5][1];
                s[2] = 1.0f;
            }
            break;
        }
        if (!abc1) s[2] = 1.0f;
        if (type == 1) /* emboss: not modelled */
        {
            o->tc[i][0] = s[0];
            o->tc[i][1] = s[1];
            o->tc[i][2] = 1.0f;
            continue;
        }
        if (stq)
        {
            mul34((uint32_t)tm * 4, s, r);
        }
        else
        {
            uint32_t a = (uint32_t)tm * 4;
            r[0] = xff(a) * s[0] + xff(a + 1) * s[1] + xff(a + 2) * s[2] + xff(a + 3);
            r[1] = xff(a + 4) * s[0] + xff(a + 5) * s[1] + xff(a + 6) * s[2] + xff(a + 7);
            r[2] = 1.0f;
        }
        /* dual-texture (post) transform, when enabled */
        if (XF(0x1012) & 1)
        {
            uint32_t pti = XF(0x1050 + i), pa = 0x500 + BITS(pti, 0, 6) * 4;
            float q[4] = {r[0], r[1], r[2], 1.0f}, t3[3];
            if (BITS(pti, 8, 1))
            {
                float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (len > 0) { q[0] /= len; q[1] /= len; q[2] /= len; }
            }
            mul34(pa, q, t3);
            r[0] = t3[0]; r[1] = t3[1]; r[2] = t3[2];
        }
        o->tc[i][0] = r[0];
        o->tc[i][1] = r[1];
        o->tc[i][2] = r[2];
    }
}

/* ---- the TEV and the pixel engine --------------------------------------------------------- */

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* v / 255, exact for 0..65535 */
static int div255(int v) { return (v + 1 + (v >> 8)) >> 8; }
static int s11(uint32_t v) { return (int)((v & 0x7FF) ^ 0x400) - 0x400; }

static Col sKonst[4];

static void load_tev_regs(void)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        uint32_t lo = BP(0xE0 + i * 2), hi = BP(0xE1 + i * 2);
        /* the register pair holds either a color register or a konstant color (bit 23) */
        (void)lo;
        (void)hi;
    }
}

/* BP 0xE0-0xE7 are written as pairs; gcn_gpu.c keeps the color and konst values apart. */
extern uint32_t gcn_gpu_tev_color[4][2], gcn_gpu_tev_konst[4][2];
extern uint32_t gcn_gpu_state_serial;

static void tev_regs(void)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        sCur.sReg[i].r = s11(gcn_gpu_tev_color[i][0]);
        sCur.sReg[i].a = s11(gcn_gpu_tev_color[i][0] >> 12);
        sCur.sReg[i].b = s11(gcn_gpu_tev_color[i][1]);
        sCur.sReg[i].g = s11(gcn_gpu_tev_color[i][1] >> 12);
        sKonst[i].r = (int)(gcn_gpu_tev_konst[i][0] & 255);
        sKonst[i].a = (int)((gcn_gpu_tev_konst[i][0] >> 12) & 255);
        sKonst[i].b = (int)(gcn_gpu_tev_konst[i][1] & 255);
        sKonst[i].g = (int)((gcn_gpu_tev_konst[i][1] >> 12) & 255);
    }
}

static int konst_c(int sel, const Col *k, int comp)
{
    static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    if (sel < 8) return frac[sel];
    if (sel >= 0x0C && sel <= 0x0F)
    {
        const Col *c = &sKonst[sel - 0x0C];
        return comp == 0 ? c->r : comp == 1 ? c->g : c->b;
    }
    if (sel >= 0x10)
    {
        const Col *c = &sKonst[(sel - 0x10) & 3];
        switch ((sel - 0x10) >> 2)
        {
        case 0: return c->r;
        case 1: return c->g;
        case 2: return c->b;
        default: return c->a;
        }
    }
    (void)k;
    return 0;
}

static int konst_a(int sel)
{
    static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    if (sel < 8) return frac[sel];
    if (sel >= 0x10)
    {
        const Col *c = &sKonst[(sel - 0x10) & 3];
        switch ((sel - 0x10) >> 2)
        {
        case 0: return c->r;
        case 1: return c->g;
        case 2: return c->b;
        default: return c->a;
        }
    }
    return 0;
}





/* pixel engine state of the primitive (setup_state) */


static int alpha_test(const PrimState *ps, int a)
{
    int t0, t1;

#define CMP(f, r) ((f) == 0 ? 0 : (f) == 1 ? a < (r) : (f) == 2 ? a == (r) : (f) == 3 ? a <= (r) : (f) == 4 ? a > (r) : (f) == 5 ? a != (r) : (f) == 6 ? a >= (r) : 1)
    t0 = CMP(ps->sAF0, ps->sAR0);
    t1 = CMP(ps->sAF1, ps->sAR1);
#undef CMP
    switch (ps->sAOp)
    {
    case 0: return t0 && t1;
    case 1: return t0 || t1;
    case 2: return t0 != t1;
    default: return t0 == t1;
    }
}

/* ---- rasterisation ------------------------------------------------------------------------- */
typedef struct
{
    float x, y, z, iw;       /* screen x, y, depth (0..2^24), 1/w */
    float col[2][4];         /* / w */
    float tc[8][3];          /* / w */
} Scr;


static void to_screen(const VtxOut *v, Scr *s)
{
    float iw = 1.0f / v->w;
    float sx = xff(0x101A), sy = xff(0x101B), sz = xff(0x101C), ox = xff(0x101D), oy = xff(0x101E), oz = xff(0x101F);
    int k, i;

    s->x = ox + sx * v->x * iw - 342.0f;
    s->y = oy + sy * v->y * iw - 342.0f;
    s->z = oz + sz * v->z * iw;
    s->iw = iw;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 4; i++) s->col[k][i] = v->col[k][i] * iw;
    for (k = 0; k < 8; k++)
        for (i = 0; i < 3; i++) s->tc[k][i] = v->tc[k][i] * iw;
}

/* GCN_TRACE_PRIMS=<display copy number>: what happens to every primitive of that frame */
static int sTrace, sTraceFrame = -2, sCopies;
static unsigned sTrZ, sTrA, sTrDrawn, sTrTris, sTrCulled;
static unsigned sFlushes, sFlushTris; /* GCN_RASTER_STATS */
static double sFlushTime, sPrimTime, sBandTime[16];
#include <time.h>
static double now_ms(void)
{
#if defined(GEKKO) || defined(__3DS__)
    return 0.0; /* the statistics only (not drawn on the console) */
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

void gcn_raster_frame_done(void)
{
    if (sTraceFrame == -2)
    {
        const char *e = getenv("GCN_TRACE_PRIMS");
        sTraceFrame = e ? atoi(e) : -1;
    }
    sCopies++;
    sTrace = sTraceFrame >= 0 && sCopies == sTraceFrame;
    {
        static int stats = -1;
        if (stats < 0) stats = getenv("GCN_RASTER_STATS") != NULL;
        if (stats && sCopies % 60 == 0)
        {
            static double last;
            double t = now_ms();
            fprintf(stderr, "raster: frame %d: %u flushes, %u triangles queued, primitives %.2f ms, drawing %.2f ms, "
                            "%.1f ms per frame (last 60)\n",
                    sCopies, sFlushes, sFlushTris, sPrimTime, sFlushTime, last ? (t - last) / 60.0 : 0.0);
            last = t;
        }
        sFlushes = sFlushTris = 0;
        if (stats && sCopies % 60 == 0)
        {
            int b;
            fprintf(stderr, "raster: per thread ms:");
            for (b = 0; b < 16; b++) fprintf(stderr, " %.1f", sBandTime[b]);
            fputc('\n', stderr);
        }
        memset(sBandTime, 0, sizeof(sBandTime));
        sFlushTime = sPrimTime = 0;
    }
}

/* decodes the TEV stages and the pixel engine registers for the primitive */
static void setup_pixel_state(void)
{
    uint32_t zmode = BP(0x40), cmode = BP(0x41), pec = BP(0x43), ac = BP(0xF3), dsta = BP(0x42);
    int s, k;

    sCur.tcused = sCur.tcproj = sCur.chanused = 0;
    sCur.sZTest = BITS(zmode, 0, 1);
    sCur.sZFunc = BITS(zmode, 1, 3);
    sCur.sZUpd = BITS(zmode, 4, 1);
    sCur.sBlend = BITS(cmode, 0, 1);
    sCur.sColorUpd = BITS(cmode, 3, 1);
    sCur.sAlphaUpd = BITS(cmode, 4, 1);
    sCur.sBlendDF = BITS(cmode, 5, 3);
    sCur.sBlendSF = BITS(cmode, 8, 3);
    sCur.sBlendSub = BITS(cmode, 11, 1);
    sCur.sPeFmt = BITS(pec, 0, 3);
    sCur.sDstAlphaOn = BITS(dsta, 8, 1);
    sCur.sDstAlpha = BITS(dsta, 0, 8);
    sCur.sAR0 = BITS(ac, 0, 8);
    sCur.sAR1 = BITS(ac, 8, 8);
    sCur.sAF0 = BITS(ac, 16, 3);
    sCur.sAF1 = BITS(ac, 19, 3);
    sCur.sAOp = BITS(ac, 22, 2);

    for (s = 0; s < sCur.sNumStages; s++)
    {
        TevStage *st = &sCur.sStage[s];
        uint32_t tref = BP(0x28 + s / 2) >> ((s & 1) * 12);
        uint32_t cenv = BP(0xC0 + s * 2), aenv = BP(0xC1 + s * 2);
        uint32_t ksel = BP(0xF6 + s / 2) >> ((s & 1) * 10);
        int kc = BITS(ksel, 4, 5), ka = BITS(ksel, 9, 5);
        int rs = BITS(aenv, 0, 2), ts = BITS(aenv, 2, 2);
        uint32_t ra = BP(0xF6 + rs * 2), rb = BP(0xF7 + rs * 2), ta = BP(0xF6 + ts * 2), tb = BP(0xF7 + ts * 2);

        st->texmap = BITS(tref, 0, 3);
        st->texcoord = BITS(tref, 3, 3);
        st->texon = BITS(tref, 6, 1) && sCur.sTexMap[st->texmap] != NULL;
        st->chan = BITS(tref, 7, 3);
        st->rswap[0] = BITS(ra, 0, 2); st->rswap[1] = BITS(ra, 2, 2);
        st->rswap[2] = BITS(rb, 0, 2); st->rswap[3] = BITS(rb, 2, 2);
        st->tswap[0] = BITS(ta, 0, 2); st->tswap[1] = BITS(ta, 2, 2);
        st->tswap[2] = BITS(tb, 0, 2); st->tswap[3] = BITS(tb, 2, 2);
        st->kcol.r = konst_c(kc, 0, 0);
        st->kcol.g = konst_c(kc, 0, 1);
        st->kcol.b = konst_c(kc, 0, 2);
        st->kcol.a = konst_a(ka);
        st->cin[0] = BITS(cenv, 12, 4); st->cin[1] = BITS(cenv, 8, 4);
        st->cin[2] = BITS(cenv, 4, 4); st->cin[3] = BITS(cenv, 0, 4);
        st->ain[0] = BITS(aenv, 13, 3); st->ain[1] = BITS(aenv, 10, 3);
        st->ain[2] = BITS(aenv, 7, 3); st->ain[3] = BITS(aenv, 4, 3);
        st->cop.bias = BITS(cenv, 16, 2); st->cop.sub = BITS(cenv, 18, 1);
        st->cop.clampv = BITS(cenv, 19, 1); st->cop.scale = BITS(cenv, 20, 2);
        st->aop.bias = BITS(aenv, 16, 2); st->aop.sub = BITS(aenv, 18, 1);
        st->aop.clampv = BITS(aenv, 19, 1); st->aop.scale = BITS(aenv, 20, 2);
        st->cdest = BITS(cenv, 22, 2);
        st->adest = BITS(aenv, 22, 2);
        for (k = 0; k < 4; k++)
        {
            static const int cslot[16] = {SL_PREV, SL_PREV, SL_REG0, SL_REG0, SL_REG1, SL_REG1, SL_REG2, SL_REG2,
                                          SL_TEX, SL_TEX, SL_RAS, SL_RAS, SL_ONE, SL_HALF, SL_KONST, SL_ZERO};
            static const int aslot[8] = {SL_PREV, SL_REG0, SL_REG1, SL_REG2, SL_TEX, SL_RAS, SL_KONST, SL_ZERO};
            int sel = st->cin[k], alpha = sel < 12 && (sel & 1), j;

            st->cslot[k] = cslot[sel];
            for (j = 0; j < 3; j++) st->ccomp[k][j] = alpha ? 3 : j;
            st->aslot[k] = aslot[st->ain[k]];
        }
        if (st->texon)
        {
            sCur.tcused |= 1 << st->texcoord;
        }
        if (st->chan == 0) sCur.chanused |= 1;
        else if (st->chan == 1) sCur.chanused |= 2;
    }

    /* samplers of the maps in use; texture coordinates that need the divide by q */
    for (s = 0; s < 8; s++)
    {
        Sampler *sm = &sCur.smp[s];
        const TexEntry *t = sCur.sTexMap[s];

        memset(sm, 0, sizeof(*sm));
        if (!t) continue;
        sm->px = t->px;
        sm->w = (int)t->w;
        sm->h = (int)t->h;
        sm->fw = (float)t->w;
        sm->fh = (float)t->h;
        sm->ws = sCur.sTexWrapS[s];
        sm->wt = sCur.sTexWrapT[s];
        sm->linear = sCur.sTexLinear[s];
        sm->mx = (sm->ws == 1 && (sm->w & (sm->w - 1)) == 0) ? sm->w - 1 : -1;
        sm->my = (sm->wt == 1 && (sm->h & (sm->h - 1)) == 0) ? sm->h - 1 : -1;
    }
    for (s = 0; s < 8; s++)
    {
        uint32_t tg = XF(0x1040 + s);
        int type = BITS(tg, 4, 3);
        if ((type == 0 && BITS(tg, 1, 1)) || (type == 0 && (XF(0x1012) & 1))) sCur.tcproj |= 1 << s;
    }
    {
        /* fog: A and C are 20-bit floats (11-bit mantissa, 8-bit exponent, sign) */
        uint32_t f0 = BP(0xEE), f3 = BP(0xF1), col = BP(0xF2), bits;

        sCur.fog_type = BITS(f3, 21, 3);
        sCur.fog_ortho = BITS(f3, 20, 1);
        bits = (BITS(f0, 19, 1) << 31) | (BITS(f0, 11, 8) << 23) | (BITS(f0, 0, 11) << 12);
        memcpy(&sCur.fog_a, &bits, 4);
        bits = (BITS(f3, 19, 1) << 31) | (BITS(f3, 11, 8) << 23) | (BITS(f3, 0, 11) << 12);
        memcpy(&sCur.fog_c, &bits, 4);
        sCur.fog_bmag = (int)(BP(0xEF) & 0xFFFFFF);
        sCur.fog_bshift = (int)BITS(BP(0xF0), 0, 5);
        sCur.fog_r = (int)BITS(col, 16, 8);
        sCur.fog_g = (int)BITS(col, 8, 8);
        sCur.fog_b = (int)BITS(col, 0, 8);
        if (sCur.fog_type == 1 || sCur.fog_type == 3) sCur.fog_type = 0; /* not valid modes */
        if (sCur.fog_type == 0)
        {
            /* keep the state identical across primitives that differ only in unused fog values */
            sCur.fog_ortho = sCur.fog_bmag = sCur.fog_bshift = sCur.fog_r = sCur.fog_g = sCur.fog_b = 0;
            sCur.fog_a = sCur.fog_c = 0.0f;
        }
    }
    {
        /* CMP(f): 0 never, 7 always passes */
        int t0 = sCur.sAF0 == 7, t1 = sCur.sAF1 == 7;
        sCur.atest_always = sCur.sAOp == 0 ? (t0 && t1) : sCur.sAOp == 1 ? (t0 || t1) : 0;
    }
}

static int blend_factor(int f, int sc, int out_a, int dc, int da)
{
    switch (f)
    {
    case 0: return 0;
    case 1: return 255;
    case 2: return dc; /* source factor: destination color; destination factor: source color */
    case 3: return 255 - dc;
    case 4: return out_a;
    case 5: return 255 - out_a;
    case 6: return da;
    default: return 255 - da;
    }
    (void)sc;
}

static inline int tev_op_i(int a, int b, int c, int d, const TevOp *op)
{
    int v;

    c = c + (c >> 7); /* 0..255 -> 0..256 */
    if (op->bias == 3)
    {
        /* compare modes: only the 8-bit component compare here */
        return d + ((op->sub == 0 ? (a > b) : (a == b)) ? c : 0);
    }
    v = (a * (256 - c) + b * c) >> 8;
    if (op->sub) v = -v;
    v += d;
    if (op->bias == 1) v += 128;
    else if (op->bias == 2) v -= 128;
    switch (op->scale)
    {
    case 1: v *= 2; break;
    case 2: v *= 4; break;
    case 3: v /= 2; break;
    }
    if (op->clampv) return v < 0 ? 0 : v > 255 ? 255 : v;
    return v < -1024 ? -1024 : v > 1023 ? 1023 : v;
}

static inline int depth_test(int func, uint32_t z, uint32_t dz)
{
    switch (func)
    {
    case 0: return 0;
    case 1: return z < dz;
    case 2: return z == dz;
    case 3: return z <= dz;
    case 4: return z > dz;
    case 5: return z != dz;
    case 6: return z >= dz;
    default: return 1;
    }
}

/* one pixel that passed the depth test: TEV stages, alpha test, depth write, blending */
static void shade(const PrimState *ps, int px, int py, int idx, uint32_t zi, const int *c0, const int *c1, float (*tc)[2])
{
    int v[SL_COUNT][4];
    int s, k;
    int out[4];

    for (k = 0; k < 4; k++)
    {
        v[SL_PREV][k] = (&ps->sReg[0].r)[k];
        v[SL_REG0][k] = (&ps->sReg[1].r)[k];
        v[SL_REG1][k] = (&ps->sReg[2].r)[k];
        v[SL_REG2][k] = (&ps->sReg[3].r)[k];
        v[SL_ONE][k] = 255;
        v[SL_HALF][k] = 128;
        v[SL_ZERO][k] = 0;
    }
    for (s = 0; s < ps->sNumStages; s++)
    {
        const TevStage *st = &ps->sStage[s];
        int res[4];

        if (st->texon)
        {
            uint32_t t = sample(&ps->smp[st->texmap], tc[st->texcoord][0], tc[st->texcoord][1]);
            int src[4] = {(int)(t & 255), (int)((t >> 8) & 255), (int)((t >> 16) & 255), (int)(t >> 24)};
            for (k = 0; k < 4; k++) v[SL_TEX][k] = src[st->tswap[k]];
        }
        else
        {
            v[SL_TEX][0] = v[SL_TEX][1] = v[SL_TEX][2] = v[SL_TEX][3] = 255;
        }
        if (st->chan == 0)
            for (k = 0; k < 4; k++) v[SL_RAS][k] = c0[st->rswap[k]];
        else if (st->chan == 1)
            for (k = 0; k < 4; k++) v[SL_RAS][k] = c1[st->rswap[k]];
        else
            v[SL_RAS][0] = v[SL_RAS][1] = v[SL_RAS][2] = v[SL_RAS][3] = 0;
        v[SL_KONST][0] = st->kcol.r;
        v[SL_KONST][1] = st->kcol.g;
        v[SL_KONST][2] = st->kcol.b;
        v[SL_KONST][3] = st->kcol.a;

        /* a, b, c are 8-bit inputs, d the full register range */
        for (k = 0; k < 3; k++)
            res[k] = tev_op_i(v[st->cslot[0]][st->ccomp[0][k]] & 255, v[st->cslot[1]][st->ccomp[1][k]] & 255,
                              v[st->cslot[2]][st->ccomp[2][k]] & 255, v[st->cslot[3]][st->ccomp[3][k]], &st->cop);
        res[3] = tev_op_i(v[st->aslot[0]][3] & 255, v[st->aslot[1]][3] & 255, v[st->aslot[2]][3] & 255,
                          v[st->aslot[3]][3], &st->aop);
        v[st->cdest][0] = res[0];
        v[st->cdest][1] = res[1];
        v[st->cdest][2] = res[2];
        v[st->adest][3] = res[3];
    }
    for (k = 0; k < 4; k++) out[k] = clamp255(v[SL_PREV][k]);

    if (sTrace)
    {
        /* GCN_TRACE_PIXEL=x,y: what every primitive of the traced frame writes there */
        static int probe_x = -2, probe_y;
        if (probe_x == -2)
        {
            const char *e = getenv("GCN_TRACE_PIXEL");
            probe_x = -1;
            if (e) sscanf(e, "%d,%d", &probe_x, &probe_y);
        }
        if (px == probe_x && py == probe_y)
        {
            fprintf(stderr, "  pixel %d,%d: out %d %d %d %d, color0 %d %d %d %d, color1 %d %d %d %d, z %u\n", px, py,
                    out[0], out[1], out[2], out[3], c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3], zi);
            for (k = 0; k < ps->sNumStages; k++)
            {
                const TevStage *st = &ps->sStage[k];
                fprintf(stderr, "    stage %d: tex %s map %d coord %d chan %d  c-in %d %d %d %d  a-in %d %d %d %d  "
                                "dest %d/%d konst %d %d %d %d\n",
                        k, st->texon ? "on" : "off", st->texmap, st->texcoord, st->chan, st->cin[0], st->cin[1], st->cin[2],
                        st->cin[3], st->ain[0], st->ain[1], st->ain[2], st->ain[3], st->cdest, st->adest, st->kcol.r,
                        st->kcol.g, st->kcol.b, st->kcol.a);
            }
        }
    }
    if (!ps->atest_always && !alpha_test(ps, out[3])) { if (sTrace) sTrA++; return; }
    if (sTrace) sTrDrawn++;
    if (ps->fog_type)
    {
        /* eye-space depth from the pixel's depth, minus C, as the fog amount (0..1) */
        float ze, f;
        int fi;

        if (!ps->fog_ortho)
        {
            int denom = ps->fog_bmag - (int)(zi >> ps->fog_bshift);
            ze = denom != 0 ? ps->fog_a * 16777215.0f / (float)denom : 1e30f;
        }
        else
        {
            ze = ps->fog_a * ((float)zi / 16777215.0f);
        }
        f = ze - ps->fog_c;
        f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        switch (ps->fog_type)
        {
        case 4: f = 1.0f - exp2f(-8.0f * f); break;
        case 5: f = 1.0f - exp2f(-8.0f * f * f); break;
        case 6: f = exp2f(-8.0f * (1.0f - f)); break;
        case 7: f = exp2f(-8.0f * (1.0f - f) * (1.0f - f)); break;
        default: break; /* linear */
        }
        fi = (int)(f * 256.0f);
        out[0] = (out[0] * (256 - fi) + ps->fog_r * fi) >> 8;
        out[1] = (out[1] * (256 - fi) + ps->fog_g * fi) >> 8;
        out[2] = (out[2] * (256 - fi) + ps->fog_b * fi) >> 8;
    }
    if (ps->sZUpd) R.depth[idx] = zi;

    {
        uint32_t d = R.efb[idx];
        int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255, da = d >> 24;
        int nr = out[0], ng = out[1], nb = out[2], na = out[3];

        if (ps->sPeFmt == 0) da = 255;
        if (ps->sBlend)
        {
            if (ps->sBlendSub)
            {
                nr = clamp255(dr - nr); ng = clamp255(dg - ng); nb = clamp255(db - nb);
            }
            else
            {
                /* source factor reads the destination, destination factor the source */
                nr = clamp255(div255(out[0] * blend_factor(ps->sBlendSF, 0, out[3], dr, da) + dr * blend_factor(ps->sBlendDF, 0, out[3], out[0], da)));
                ng = clamp255(div255(out[1] * blend_factor(ps->sBlendSF, 0, out[3], dg, da) + dg * blend_factor(ps->sBlendDF, 0, out[3], out[1], da)));
                nb = clamp255(div255(out[2] * blend_factor(ps->sBlendSF, 0, out[3], db, da) + db * blend_factor(ps->sBlendDF, 0, out[3], out[2], da)));
                na = clamp255(div255(out[3] * blend_factor(ps->sBlendSF, 0, out[3], da, da) + da * blend_factor(ps->sBlendDF, 0, out[3], out[3], da)));
            }
        }
        if (!ps->sColorUpd) { nr = dr; ng = dg; nb = db; }
        if (!ps->sAlphaUpd) na = da;
        if (ps->sDstAlphaOn) na = ps->sDstAlpha;
        R.efb[idx] = rgba(nr, ng, nb, na);
    }
}

static float edge(const Scr *a, const Scr *b, float x, float y)
{
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

/* Attributes interpolated across a triangle: 1/w, z, two colors, up to 8 texcoords (all
 * but z divided by w at the vertices), each as a plane a*x + b*y + c in screen space. */
#define ATTR_IW 0
#define ATTR_Z 1
#define ATTR_COL 2
#define ATTR_TC 10
#define ATTR_MAX (ATTR_TC + 24)

static inline int clamp_col(float f)
{
    int i = (int)f;
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

/* A triangle set up for drawing: bounding box, edge planes and attribute planes (only the
 * attributes the stages use: 1/w, z, colors, the used texture coordinates). Computed once
 * per triangle, then each thread draws the rows it owns from it. */
typedef struct
{
    int minx, maxx, miny, maxy;
    int n, ntc, persp, state;
    int tci[8];
    float iw0;
    float ea[3], eb[3], ec[3];
    float dx[ATTR_MAX], dy[ATTR_MAX], d0[ATTR_MAX];
} TriSetup;

/* 0: nothing to draw (culled, degenerate, outside the scissor) */
static int tri_setup(const PrimState *ps, const Scr *a, const Scr *b, const Scr *c, TriSetup *t)
{
    float area, inv, va[ATTR_MAX], vb[ATTR_MAX], vc[ATTR_MAX];
    int cull = ps->cull, tcused = ps->tcused, i, k, n;

    t->miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    t->maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (t->miny < ps->sScY0) t->miny = ps->sScY0;
    if (t->maxy > ps->sScY1) t->maxy = ps->sScY1;
    if (t->miny > t->maxy) return 0;

    area = edge(a, b, c->x, c->y);
    if (sTrace) sTrTris++;
    if (area == 0.0f) return 0;
    if (cull == 3) { if (sTrace) sTrCulled++; return 0; }
    /* front faces are clockwise on screen (y down: area > 0); register value 1 culls the
     * back faces, 2 the front ones (GXSetCullMode swaps the API's values) */
    if (cull == 1 && area < 0) { if (sTrace) sTrCulled++; return 0; }
    if (cull == 2 && area > 0) { if (sTrace) sTrCulled++; return 0; }
    t->minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    t->maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    if (t->minx < ps->sScX0) t->minx = ps->sScX0;
    if (t->maxx > ps->sScX1) t->maxx = ps->sScX1;
    if (t->minx > t->maxx) return 0;
    inv = 1.0f / area;

    /* barycentric weights as planes: w0 = edge(b, c), w1 = edge(c, a), w2 = edge(a, b), / area */
    t->ea[0] = -(c->y - b->y) * inv; t->eb[0] = (c->x - b->x) * inv; t->ec[0] = ((c->y - b->y) * b->x - (c->x - b->x) * b->y) * inv;
    t->ea[1] = -(a->y - c->y) * inv; t->eb[1] = (a->x - c->x) * inv; t->ec[1] = ((a->y - c->y) * c->x - (a->x - c->x) * c->y) * inv;
    t->ea[2] = -(b->y - a->y) * inv; t->eb[2] = (b->x - a->x) * inv; t->ec[2] = ((b->y - a->y) * a->x - (b->x - a->x) * a->y) * inv;

    /* gather the attributes the stages use; a triangle whose vertices share 1/w (2D,
     * orthographic) needs no perspective division per pixel */
    va[ATTR_IW] = a->iw; vb[ATTR_IW] = b->iw; vc[ATTR_IW] = c->iw;
    va[ATTR_Z] = a->z; vb[ATTR_Z] = b->z; vc[ATTR_Z] = c->z;
    for (k = 0; k < 4; k++)
    {
        va[ATTR_COL + k] = a->col[0][k]; vb[ATTR_COL + k] = b->col[0][k]; vc[ATTR_COL + k] = c->col[0][k];
        va[ATTR_COL + 4 + k] = a->col[1][k]; vb[ATTR_COL + 4 + k] = b->col[1][k]; vc[ATTR_COL + 4 + k] = c->col[1][k];
    }
    n = ATTR_TC;
    t->ntc = 0;
    for (i = 0; i < 8; i++)
    {
        if (!(tcused & (1 << i))) continue;
        t->tci[t->ntc++] = i;
        for (k = 0; k < 3; k++)
        {
            va[n + k] = a->tc[i][k];
            vb[n + k] = b->tc[i][k];
            vc[n + k] = c->tc[i][k];
        }
        n += 3;
    }
    for (i = 0; i < n; i++)
    {
        t->dx[i] = t->ea[0] * va[i] + t->ea[1] * vb[i] + t->ea[2] * vc[i];
        t->dy[i] = t->eb[0] * va[i] + t->eb[1] * vb[i] + t->eb[2] * vc[i];
        t->d0[i] = t->ec[0] * va[i] + t->ec[1] * vb[i] + t->ec[2] * vc[i];
    }
    t->n = n;
    t->persp = !(a->iw == b->iw && b->iw == c->iw);
    t->iw0 = a->iw;
    return 1;
}

/* Draws the rows of the triangle in this thread's bands: rows y with (y / 8) % bands == band. */
static void tri_raster(const PrimState *ps, const TriSetup *t, int band, int bands)
{
    int blk = t->miny >> 3, x, y, i, k, n = t->n, persp = t->persp;
    float cur[ATTR_MAX];

    if (bands > 1)
    {
        blk += ((band - blk % bands) + bands) % bands; /* this thread's first band at or below miny */
        if (blk * 8 > t->maxy) return;
    }
    for (; blk * 8 <= t->maxy; blk += bands)
    {
        int y0 = blk * 8 < t->miny ? t->miny : blk * 8, y1 = blk * 8 + 7 > t->maxy ? t->maxy : blk * 8 + 7;

        for (y = y0; y <= y1; y++)
        {
            float py = y + 0.5f, xl = (float)t->minx, xr = (float)t->maxx;
            int x0, x1, row = y * GCN_EFB_W;

            /* the pixels inside all three edges: w_k(x) = ea*(x + 0.5) + r >= 0 */
            for (k = 0; k < 3; k++)
            {
                float r = t->eb[k] * py + t->ec[k];
                if (t->ea[k] > 1e-12f)
                {
                    float v = ceilf(-r / t->ea[k] - 0.5f);
                    if (v > xl) xl = v;
                }
                else if (t->ea[k] < -1e-12f)
                {
                    float v = floorf(-r / t->ea[k] - 0.5f);
                    if (v < xr) xr = v;
                }
                else if (r < 0)
                {
                    xl = 1.0f;
                    xr = 0.0f;
                }
            }
            if (xl > xr) continue;
            x0 = (int)xl;
            x1 = (int)xr;
            {
                float px = x0 + 0.5f;
                for (i = 0; i < n; i++) cur[i] = t->dx[i] * px + t->dy[i] * py + t->d0[i];
            }
            for (x = x0; x <= x1; x++)
            {
                if (cur[ATTR_IW] > 0)
                {
                    float z = cur[ATTR_Z];
                    uint32_t zi = z < 0 ? 0 : z > 16777215.0f ? 16777215u : (uint32_t)z;
                    int idx = row + x;

                    if (ps->sZTest && !depth_test(ps->sZFunc, zi, R.depth[idx]))
                    {
                        if (sTrace) sTrZ++;
                    }
                    else
                    {
                        float w = persp ? 1.0f / cur[ATTR_IW] : 1.0f / t->iw0, tc[8][2];
                        int col0[4] = {0, 0, 0, 0}, col1[4] = {0, 0, 0, 0};

                        if (ps->chanused & 1)
                            for (k = 0; k < 4; k++) col0[k] = clamp_col(cur[ATTR_COL + k] * w);
                        if (ps->chanused & 2)
                            for (k = 0; k < 4; k++) col1[k] = clamp_col(cur[ATTR_COL + 4 + k] * w);
                        for (i = 0; i < t->ntc; i++)
                        {
                            const float *a = &cur[ATTR_TC + i * 3];
                            int j = t->tci[i];
                            float s = a[0] * w, tt = a[1] * w;

                            if (ps->tcproj & (1 << j))
                            {
                                float q = a[2] * w;
                                if (q != 0.0f)
                                {
                                    float iq = 1.0f / q;
                                    s *= iq;
                                    tt *= iq;
                                }
                            }
                            tc[j][0] = s;
                            tc[j][1] = tt;
                        }
                        shade(ps, x, y, idx, zi, col0, col1, tc);
                    }
                }
                for (i = 0; i < n; i++) cur[i] += t->dx[i];
            }
        }
    }
}

/* set up and draw at once (one thread, tracing) */
static void triangle(const PrimState *ps, const Scr *a, const Scr *b, const Scr *c)
{
    TriSetup t;
    if (tri_setup(ps, a, b, c, &t)) tri_raster(ps, &t, 0, 1);
}

/* ---- deferred, parallel drawing ---------------------------------------------------------
 * Triangles are queued with a snapshot of the state they need (PrimState) and drawn by
 * worker threads while the game goes on producing more: each worker owns every n-th band
 * of 8 rows and draws all queued triangles in order within its rows, so the result is
 * the same as drawing them one after the other. A triangle's setup (edge and attribute
 * planes) is done once, by whichever worker reaches it first. gcn_raster_flush waits until
 * everything queued is drawn (before an EFB copy, a texture changing under the queue...).
 * GCN_RASTER_THREADS=n sets the worker count (1 = draw at once, as triangles come). */
typedef struct
{
    Scr v[3];
    int state;
} QueuedTri;

static QueuedTri *sQueue;
static int sQueueCount, sQueueCap;
static PrimState *sStates;
static int sStateCount, sStateCap;
static int sStateDirty = 1; /* sCur differs from the last snapshot */
static int sThreads = -1;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static CRITICAL_SECTION sLock;
static CONDITION_VARIABLE sStart, sFinished;
#define LOCK() EnterCriticalSection(&sLock)
#define UNLOCK() LeaveCriticalSection(&sLock)
#define WAIT(cv) SleepConditionVariableCS(&(cv), &sLock, INFINITE)
#define WAKE_ALL(cv) WakeAllConditionVariable(&(cv))
/* claiming a triangle's setup: 0 to do, 1 being done, 2 ready, 3 nothing to draw */
#define CLAIM(p) (InterlockedCompareExchange((volatile LONG *)(p), 1, 0) == 0)
#define PUBLISH(p, v) InterlockedExchange((volatile LONG *)(p), (v))
#define PEEK(p) (*(volatile LONG *)(p))
#define PAUSE() YieldProcessor()
#elif defined(GEKKO) || defined(__3DS__)
/* one core (and the real GPU draws there): no worker threads, everything at once */
static int sLock, sStart, sFinished;
#define LOCK() ((void)sLock)
#define UNLOCK() ((void)sLock)
#define WAIT(cv) ((void)(cv))
#define WAKE_ALL(cv) ((void)(cv))
#define CLAIM(p) (*(p) == 0 ? (*(p) = 1, 1) : 0)
#define PUBLISH(p, v) (*(p) = (v))
#define PEEK(p) (*(volatile int *)(p))
#define PAUSE() ((void)0)
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sStart = PTHREAD_COND_INITIALIZER, sFinished = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&sLock)
#define UNLOCK() pthread_mutex_unlock(&sLock)
#define WAIT(cv) pthread_cond_wait(&(cv), &sLock)
#define WAKE_ALL(cv) pthread_cond_broadcast(&(cv))
#define CLAIM(p) __atomic_compare_exchange_n((p), &(int){0}, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define PUBLISH(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define PEEK(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#if defined(__x86_64__) || defined(__i386__)
#define PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define PAUSE() __asm__ __volatile__("yield")
#else
#define PAUSE() sched_yield()
#endif
#endif

static TriSetup *sSetup;   /* one per queued triangle */
static int *sSetupState;   /* CLAIM / PUBLISH states, see above */

/* guarded by sLock: what the workers may draw, how far each got, resets of the queue */
static int sPublished;     /* triangles handed to the workers */
static int sProgress[16];  /* per worker: triangles drawn in its rows */
static int sReset;         /* counts queue resets (the workers start over at 0) */
static int sWaiting;       /* the game thread waits for the workers */
static int sIdle;          /* workers asleep */
static volatile int sPublishedHint; /* sPublished, read without the lock by spinning workers */

static void draw_range(int band, int from, int to)
{
    int i;
    double t0 = now_ms();

    for (i = from; i < to; i++)
    {
        int *st = &sSetupState[i];
        const QueuedTri *q = &sQueue[i];

        if (PEEK(st) == 0 && CLAIM(st))
            PUBLISH(st, tri_setup(&sStates[q->state], &q->v[0], &q->v[1], &q->v[2], &sSetup[i]) ? 2 : 3);
        while (PEEK(st) == 1) PAUSE(); /* another worker is setting it up */
        if (PEEK(st) == 2) tri_raster(&sStates[q->state], &sSetup[i], band, sThreads);
    }
    sBandTime[band] += now_ms() - t0;
}

#ifdef _WIN32
static DWORD WINAPI worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    int band = (int)(intptr_t)arg, done = 0, reset = 0;

    for (;;)
    {
        int to;

        LOCK();
        for (;;)
        {
            if (reset != sReset)
            {
                reset = sReset;
                done = 0;
            }
            if (sPublished > done) break;
            sIdle++;
            WAIT(sStart);
            sIdle--;
        }
        to = sPublished;
        UNLOCK();
        draw_range(band, done, to);
        done = to;
        LOCK();
        sProgress[band] = done;
        if (sWaiting) WAKE_ALL(sFinished);
        UNLOCK();
        {
            /* more is usually on its way while the game issues primitives: wait a moment
             * before sleeping, a wake-up costs the game thread more */
            int spin;
            for (spin = 0; spin < 1500 && sPublishedHint <= done && !sWaiting; spin++) PAUSE();
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void start_threads(void)
{
    const char *e = getenv("GCN_RASTER_THREADS");
    int i, cpus;

#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cpus = (int)si.dwNumberOfProcessors;
    InitializeCriticalSection(&sLock);
    InitializeConditionVariable(&sStart);
    InitializeConditionVariable(&sFinished);
#elif defined(GEKKO) || defined(__3DS__)
    (void)e;
    (void)i;
    sThreads = 1;
    return;
#else
    cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    /* the game thread keeps a core of its own */
    sThreads = e ? atoi(e) : cpus - 1;
    if (sThreads > 16) sThreads = 16;
    if (sThreads < 1) sThreads = 1;
    if (sThreads == 1) return;
    for (i = 0; i < sThreads; i++)
    {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, worker, (void *)(intptr_t)i, 0, NULL);
        if (h) CloseHandle(h);
#elif defined(GEKKO) || defined(__3DS__)
        (void)worker;
#else
        pthread_t t;
        pthread_create(&t, NULL, worker, (void *)(intptr_t)i);
        pthread_detach(t);
#endif
    }
}

/* hands what is queued to the workers */
static void publish(void)
{
    LOCK();
    if (sQueueCount > sPublished)
    {
        sPublished = sQueueCount;
        sPublishedHint = sQueueCount;
        if (sIdle) WAKE_ALL(sStart);
    }
    UNLOCK();
}

void gcn_raster_flush(void)
{
    double t0;
    int i, busy;

    if (!sQueueCount)
    {
        free_retired(); /* nothing queued samples them */
        return;
    }
    t0 = now_ms();
    sFlushes++;
    sFlushTris += (unsigned)sQueueCount;
    publish();
    LOCK();
    sWaiting = 1;
    for (;;)
    {
        for (i = 0, busy = 0; i < sThreads; i++) busy |= sProgress[i] < sPublished;
        if (!busy) break;
        WAIT(sFinished);
    }
    sWaiting = 0;
    /* everything is drawn and the workers are idle: start the queue over */
    sPublished = 0;
    sPublishedHint = 0;
    for (i = 0; i < sThreads; i++) sProgress[i] = 0;
    sReset++;
    UNLOCK();
    sQueueCount = 0;
    sStateCount = 0;
    sStateDirty = 1;
    free_retired();
    sFlushTime += now_ms() - t0;
}

/* a triangle of the current primitive: queued, or drawn at once */
static void emit(const Scr *a, const Scr *b, const Scr *c)
{
    QueuedTri *t;

    if (sThreads < 0) start_threads();
    if (sThreads <= 1 || sTrace)
    {
        if (sQueueCount) gcn_raster_flush(); /* tracing starts with a frame's first copy */
        triangle(&sCur, a, b, c);
        return;
    }
    if (sQueueCap == 0)
    {
        sQueueCap = 65536;
        sQueue = (QueuedTri *)malloc(sizeof(QueuedTri) * (size_t)sQueueCap);
        sSetup = (TriSetup *)malloc(sizeof(TriSetup) * (size_t)sQueueCap);
        sSetupState = (int *)malloc(sizeof(int) * (size_t)sQueueCap);
        sStateCap = 4096;
        sStates = (PrimState *)malloc(sizeof(PrimState) * (size_t)sStateCap);
    }
    if (sQueueCount == sQueueCap || (sStateDirty && sStateCount == sStateCap))
    {
        gcn_raster_flush(); /* full: draw what is queued, then start over */
    }
    if (sStateDirty)
    {
        sStates[sStateCount++] = sCur;
        sStateDirty = 0;
    }
    t = &sQueue[sQueueCount];
    t->v[0] = *a;
    t->v[1] = *b;
    t->v[2] = *c;
    t->state = sStateCount - 1;
    sSetupState[sQueueCount] = 0;
    sQueueCount++;
    if (sQueueCount - sPublished >= 128) publish(); /* sPublished: only the game thread changes it */
}

/* Clipping in clip space, as the hardware does: against the near plane (z >= -w) and
 * the far plane (z <= 0) unless the game turned clipping off (XF 0x1005), and always
 * against w > 0. Points behind or barely in front of the eye would otherwise project to
 * enormous screen coordinates. Then the polygon is drawn as a fan. */
static void lerp(const VtxOut *a, const VtxOut *b, float t, VtxOut *o)
{
    int i, k;
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;
    o->w = a->w + (b->w - a->w) * t;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 4; i++) o->col[k][i] = a->col[k][i] + (b->col[k][i] - a->col[k][i]) * t;
    for (k = 0; k < 8; k++)
        for (i = 0; i < 3; i++) o->tc[k][i] = a->tc[k][i] + (b->tc[k][i] - a->tc[k][i]) * t;
}

static float clip_dist(const VtxOut *v, int plane)
{
    switch (plane)
    {
    case 0: return v->w - 1e-5f;
    case 1: return v->z + v->w + 1e-6f * fabsf(v->w);
    default: return 1e-6f * fabsf(v->w) - v->z;
    }
}

static void clip_triangle(const VtxOut *a, const VtxOut *b, const VtxOut *c)
{
    VtxOut buf[2][9];
    int n = 3, plane, i, cur = 0, planes = (XF(0x1005) & 1) ? 1 : 3;
    Scr s[9];

    /* all three inside every plane: the common case, nothing to copy */
    for (plane = 0; plane < planes; plane++)
        if (clip_dist(a, plane) < 0 || clip_dist(b, plane) < 0 || clip_dist(c, plane) < 0) break;
    if (plane == planes)
    {
        to_screen(a, &s[0]);
        to_screen(b, &s[1]);
        to_screen(c, &s[2]);
        emit(&s[0], &s[1], &s[2]);
        return;
    }
    buf[0][0] = *a; buf[0][1] = *b; buf[0][2] = *c;
    for (plane = 0; plane < planes && n >= 3; plane++)
    {
        const VtxOut *in = buf[cur];
        VtxOut *out = buf[cur ^ 1];
        int m = 0;

        for (i = 0; i < n; i++)
        {
            const VtxOut *p = &in[i], *q = &in[(i + 1) % n];
            float dp = clip_dist(p, plane), dq = clip_dist(q, plane);

            if (dp >= 0) out[m++] = *p;
            if ((dp >= 0) != (dq >= 0)) lerp(p, q, dp / (dp - dq), &out[m++]);
        }
        n = m;
        cur ^= 1;
    }
    if (n < 3) return;
    for (i = 0; i < n; i++) to_screen(&buf[cur][i], &s[i]);
    for (i = 1; i + 1 < n; i++) emit(&s[0], &s[i], &s[i + 1]);
}

static void setup_state(void)
{
    static uint32_t serial, epoch, generation;
    static int valid;
    uint32_t gm = BP(0x00), tl = BP(0x20), br = BP(0x21);

    /* nothing it reads changed since the last primitive: sCur still holds it */
    if (valid && serial == gcn_gpu_state_serial && epoch == sEpoch && generation == (uint32_t)sTexGeneration && !sTrace)
        return;
    valid = 1;
    serial = gcn_gpu_state_serial;
    epoch = sEpoch;

    sCur.cull = BITS(gm, 14, 2);

    sCur.sNumTex = BITS(gm, 0, 4);
    sCur.sNumChan = BITS(gm, 4, 3);
    sCur.sNumStages = BITS(gm, 10, 4) + 1;
    if (sCur.sNumTex > 8) sCur.sNumTex = 8;
    sCur.sScX0 = (int)BITS(tl, 12, 11) - 342;
    sCur.sScY0 = (int)BITS(tl, 0, 11) - 342;
    sCur.sScX1 = (int)BITS(br, 12, 11) - 342;
    sCur.sScY1 = (int)BITS(br, 0, 11) - 342;
    if (sCur.sScX0 < 0) sCur.sScX0 = 0;
    if (sCur.sScY0 < 0) sCur.sScY0 = 0;
    if (sCur.sScX1 >= GCN_EFB_W) sCur.sScX1 = GCN_EFB_W - 1;
    if (sCur.sScY1 >= GCN_EFB_H) sCur.sScY1 = GCN_EFB_H - 1;
    tev_regs();
    {
        /* textures of the enabled stages, looked up once for the primitive */
        int s, gen;
        do
        {
            /* again if the cache was emptied meanwhile (earlier pointers are gone) */
            gen = sTexGeneration;
            for (s = 0; s < 8; s++) sCur.sTexMap[s] = NULL;
            for (s = 0; s < sCur.sNumStages; s++)
            {
                uint32_t tref = BP(0x28 + s / 2) >> ((s & 1) * 12);
                int map = BITS(tref, 0, 3);
                if (BITS(tref, 6, 1) && sCur.sTexMap[map] == NULL) resolve_texture(map);
            }
        } while (gen != sTexGeneration);
        generation = (uint32_t)sTexGeneration;
    }
    setup_pixel_state();
    if (!sStateDirty && (sStateCount == 0 || memcmp(&sStates[sStateCount - 1], &sCur, sizeof(sCur)) != 0))
        sStateDirty = 1;
}

static void thick_line(const VtxOut *a, const VtxOut *b, float width)
{
    Scr sa, sb, q[4];
    float dx, dy, len, nx, ny;
    int i;

    if (a->w <= 0 || b->w <= 0) return;
    to_screen(a, &sa);
    to_screen(b, &sb);
    dx = sb.x - sa.x;
    dy = sb.y - sa.y;
    len = sqrtf(dx * dx + dy * dy);
    if (len == 0) return;
    nx = -dy / len * width * 0.5f;
    ny = dx / len * width * 0.5f;
    q[0] = sa; q[1] = sb; q[2] = sb; q[3] = sa;
    q[0].x += nx; q[0].y += ny; q[1].x += nx; q[1].y += ny;
    q[2].x -= nx; q[2].y -= ny; q[3].x -= nx; q[3].y -= ny;
    {
        /* both windings: lines are never culled */
        int cull = sCur.cull;
        if (cull)
        {
            sCur.cull = 0;
            sStateDirty = 1;
        }
        emit(&q[0], &q[1], &q[2]);
        emit(&q[0], &q[2], &q[3]);
        if (cull)
        {
            sCur.cull = cull;
            sStateDirty = 1;
        }
    }
    (void)i;
}

void gcn_raster_primitive(int prim, const GcnVertexIn *v, int count)
{
#if defined(GEKKO) || defined(__3DS__)
    static VtxOut *out; /* the console draws with its own GPU: only if this path ever runs */
    if (!out) out = (VtxOut *)malloc(sizeof(VtxOut) * (65536 / 8));
#else
    static VtxOut out[65536 / 8];
#endif
    int i, n = count;

    if (n > 65536 / 8) n = 65536 / 8;
    {
        /* debugging / profiling: GCN_NO_RASTER=1 skips drawing */
        static int skip = -1;
        if (skip < 0) skip = getenv("GCN_NO_RASTER") != NULL;
        if (skip) return;
    }
    double t0 = now_ms();
    setup_state();
    setup_lighting();
    for (i = 0; i < n; i++) transform(&v[i], &out[i]);
    sTrZ = sTrA = sTrDrawn = sTrTris = sTrCulled = 0;
    switch (prim)
    {
    case 0x80: /* quads */
    case 0x88:
        for (i = 0; i + 3 < n; i += 4)
        {
            clip_triangle(&out[i], &out[i + 1], &out[i + 2]);
            clip_triangle(&out[i], &out[i + 2], &out[i + 3]);
        }
        break;
    case 0x90: /* triangles */
        for (i = 0; i + 2 < n; i += 3) clip_triangle(&out[i], &out[i + 1], &out[i + 2]);
        break;
    case 0x98: /* strip */
        for (i = 0; i + 2 < n; i++)
        {
            if (i & 1) clip_triangle(&out[i + 1], &out[i], &out[i + 2]);
            else clip_triangle(&out[i], &out[i + 1], &out[i + 2]);
        }
        break;
    case 0xA0: /* fan */
        for (i = 1; i + 1 < n; i++) clip_triangle(&out[0], &out[i], &out[i + 1]);
        break;
    case 0xA8: /* lines */
        for (i = 0; i + 1 < n; i += 2) thick_line(&out[i], &out[i + 1], BITS(BP(0x22), 0, 8) / 6.0f + 1.0f);
        break;
    case 0xB0: /* line strip */
        for (i = 0; i + 1 < n; i++) thick_line(&out[i], &out[i + 1], BITS(BP(0x22), 0, 8) / 6.0f + 1.0f);
        break;
    case 0xB8: /* points */
        for (i = 0; i < n; i++) thick_line(&out[i], &out[i], BITS(BP(0x22), 8, 8) / 6.0f + 1.0f);
        break;
    }
    (void)load_tev_regs;
    sPrimTime += now_ms() - t0;
    if (sTrace)
    {
        Scr s0;
        uint32_t ti = BP(0x88), tm = BP(0x94);
        to_screen(&out[0], &s0);
        fprintf(stderr, "prim %02X n=%d proj=%u v0=(%.1f,%.1f,%.0f w=%.3f) tex=%d %ux%u fmt=%u @%06X stages=%d chans=%d "
                "cull=%u z=%03X blend=%04X alpha=%06X k0=%06X | tris %u culled %u zfail %u afail %u drawn %u\n",
                prim, count, XF(0x1026), s0.x, s0.y, s0.z, out[0].w, sCur.sNumTex, BITS(ti, 0, 10) + 1, BITS(ti, 10, 10) + 1,
                BITS(ti, 20, 4), (tm & 0xFFFFFF) << 5, sCur.sNumStages, sCur.sNumChan, BITS(BP(0x00), 14, 2), BP(0x40) & 0xFFF,
                BP(0x41) & 0xFFFF, BP(0xF3) & 0xFFFFFF, gcn_gpu_tev_konst[0][0] & 0xFFFFFF, sTrTris, sTrCulled, sTrZ, sTrA, sTrDrawn);
        if (getenv("GCN_TRACE_VERTS"))
            for (i = 0; i < n && i < 8; i++)
            {
                to_screen(&out[i], &s0);
                fprintf(stderr, "    v%d in=(%.2f,%.2f,%.2f) pm=%d scr=(%.1f,%.1f,%.0f) tc0=(%.3f,%.3f)\n", i, v[i].pos[0], v[i].pos[1],
                        v[i].pos[2], v[i].pnmtx, s0.x, s0.y, s0.z, out[i].tc[0][0], out[i].tc[0][1]);
            }
    }
}
