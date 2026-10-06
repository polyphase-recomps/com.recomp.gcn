/*
 * GameCube GPU emulation: command stream, registers, embedded frame buffer.
 *
 * The game's own GX library (compiled from the decomp) turns every GX call into the
 * register writes and vertex data the hardware receives through the write-gather pipe;
 * this file is the other end of that pipe. Stage one: the command processor (parsing,
 * vertex sizes from the vertex descriptor / attribute tables, display lists), BP / XF /
 * CP register state, pixel-engine synchronisation (tokens, draw done) and EFB clears and
 * copies to the display. Rasterisation comes on top of this state.
 *
 * Passthrough (Wii / GameCube hosts, GCN_GPU_PASSTHROUGH): the console's own GPU is there,
 * so every command goes on to it through the real write-gather pipe instead of the
 * software rasteriser. What changes on the way: addresses (texture images, TLUTs,
 * preloads, copy destinations, vertex arrays) point into the guest memory buffer instead
 * of the console's physical memory; display lists are expanded inline so the addresses in
 * them are translated too; the copy to the external frame buffer goes into the host's
 * display texture (gcn_gpu_set_display) as RGBA8; draw-done and token writes stay emulated
 * here, the host's GX library owns the real interrupts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcn_gpu.h"
#include "gcn_raster.h"
#include "gcn_overlay.h"
#include "gcnw.h"

#if defined(GEKKO)
#define GCN_GPU_PASSTHROUGH 1
#include <ogc/lwp_watchdog.h>
#else
#define GCN_GPU_PASSTHROUGH 0
#endif

/* time the game's commands take on their way to the GPU (consoles: diagnostics) */
static uint64_t sFeedTicks;
static uint32_t sFeedCalls;

void gcn_gpu_take_feed(uint32_t *us, uint32_t *calls)
{
#if defined(GEKKO)
    *us = (uint32_t)ticks_to_microsecs(sFeedTicks);
#else
    *us = 0;
#endif
    *calls = sFeedCalls;
    sFeedTicks = 0;
    sFeedCalls = 0;
}

static uint8_t *sMem;

/* ---- register state -------------------------------------------------------------------- */
#define TMEM_BYTES (1u << 20)
static uint8_t *sTmem;            /* texture memory: TLUTs, preloaded textures (software path) */
uint32_t gcn_gpu_tev_color[4][2], gcn_gpu_tev_konst[4][2];
/* changes with every BP write and XF register write: the rasteriser decodes its per-
 * primitive state again only when this moved (matrix and light memory is read per vertex) */
uint32_t gcn_gpu_state_serial;
static uint32_t sCP[256];
static uint32_t sBP[256];
static uint32_t sXF[0x1100];      /* XF memory (matrices 0x000-0x4FF, lights 0x600-) and registers 0x1000- */
/* which registers the game has written (passthrough: what gcn_gpu_resume gives back) */
static uint32_t sCPSet[256 / 32], sBPSet[256 / 32], sXFSet[0x1100 / 32];
#define MARK_SET(bits, i) ((bits)[(i) >> 5] |= 1u << ((i) & 31))
#define IS_SET(bits, i) (((bits)[(i) >> 5] >> ((i) & 31)) & 1)
static uint16_t sCpRegs[0x80];    /* command processor MMIO */
static uint16_t sPeRegs[0x80];    /* pixel engine MMIO */
static uint32_t sPiRegs[0x40];    /* processor interface MMIO */
static uint32_t sPending;
static uint16_t sToken;
static int sBreakpointHit;
#define TOKEN_QUEUE 8
static uint16_t sTokens[TOKEN_QUEUE];
static int sTokenHead, sTokenCount;

void gcn_gpu_retrace(void)
{
    if (sTokenCount)
    {
        sToken = sTokens[sTokenHead];
        sPeRegs[0x0E / 2] = sToken;
        sTokenHead = (sTokenHead + 1) % TOKEN_QUEUE;
        sTokenCount--;
        sPending |= 1u << 18;
    }
}

/* ---- frame buffers ------------------------------------------------------------------------ */
/* software path only (allocated by gcn_gpu_init): about 6 MB a passthrough host needn't carry */
static uint32_t *sEfb, *sDepth, *sDisplay;
static int sDisplayW = 640, sDisplayH = 480;
static GcnGpuStats sStats, sLastStats;

void gcn_gpu_init(uint8_t *guest_mem)
{
    sMem = guest_mem;
#if !GCN_GPU_PASSTHROUGH
    if (!sEfb)
    {
        sTmem = (uint8_t *)calloc(1, TMEM_BYTES);
        sEfb = (uint32_t *)calloc(GCN_EFB_W * GCN_EFB_H, 4);
        sDepth = (uint32_t *)calloc(GCN_EFB_W * GCN_EFB_H, 4);
        sDisplay = (uint32_t *)calloc(GCN_EFB_W * GCN_EFB_H, 4);
    }
#endif
    memset(sCP, 0, sizeof(sCP));
    memset(sBP, 0, sizeof(sBP));
    memset(sXF, 0, sizeof(sXF));
    memset(sCPSet, 0, sizeof(sCPSet));
    memset(sBPSet, 0, sizeof(sBPSet));
    memset(sXFSet, 0, sizeof(sXFSet));
    sPending = 0;
    {
        GcnGpuRegs r;
        r.bp = sBP;
        r.cp = sCP;
        r.xf = sXF;
        r.mem = guest_mem;
        r.tmem = sTmem;
        r.efb = sEfb;
        r.depth = sDepth;
        gcn_raster_init(&r);
    }
}

static uint8_t *guest(uint32_t addr) { return sMem + GCNW_OFFSET(addr); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

uint32_t gcn_gpu_take_interrupts(void)
{
    uint32_t p = sPending;
    sPending = 0;
    return p;
}

const uint32_t *gcn_gpu_frame(int *width, int *height)
{
    *width = sDisplayW;
    *height = sDisplayH;
    return sDisplay; /* NULL when passthrough: the frame is in the host's display texture */
}

const GcnGpuStats *gcn_gpu_last_frame_stats(void) { return &sLastStats; }

static uint32_t sDisplayCopies;
uint32_t gcn_gpu_display_copies(void) { return sDisplayCopies; }

static int sFrameDone;
int gcn_gpu_take_frame_done(void)
{
    const int done = sFrameDone;
    sFrameDone = 0;
    return done;
}

void gcn_gpu_clear_color(uint8_t rgba[4])
{
    rgba[0] = (uint8_t)sBP[0x4F];        /* 0x4F: alpha, red */
    rgba[1] = (uint8_t)(sBP[0x50] >> 8); /* 0x50: green, blue */
    rgba[2] = (uint8_t)sBP[0x50];
    rgba[3] = (uint8_t)(sBP[0x4F] >> 8);
}
const GcnGpuStats *gcn_gpu_current_stats(void) { return &sStats; }

/* ---- vertex sizes ---------------------------------------------------------------------- */
static const uint8_t kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0}; /* u8 s8 u16 s16 f32 */
static const uint8_t kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0}; /* 565 888 888x 4444 6666 8888 */

static int attr_type(int bitpos, uint32_t reg) { return (reg >> bitpos) & 3; } /* 0 none 1 direct 2 idx8 3 idx16 */

static int vertex_size(int fmt)
{
    uint32_t lo = sCP[0x50], hi = sCP[0x60];
    uint32_t a = sCP[0x70 + fmt], b = sCP[0x80 + fmt], c = sCP[0x90 + fmt];
    int size = 0, i, t;

    if (lo & 1) size += 1;                /* position/normal matrix index */
    for (i = 0; i < 8; i++)
    {
        if (lo & (2u << i)) size += 1;    /* texture matrix indices */
    }
    t = attr_type(9, lo);                 /* position */
    if (t == 1) size += kCompSize[(a >> 1) & 7] * ((a & 1) ? 3 : 2);
    else if (t) size += t - 1;
    t = attr_type(11, lo);                /* normal */
    if (t)
    {
        int nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
        if (t == 1) size += kCompSize[(a >> 10) & 7] * (nbt ? 9 : 3);
        else size += (t - 1) * ((nbt && index3) ? 3 : 1);
    }
    t = attr_type(13, lo);                /* color 0 */
    if (t == 1) size += kColorSize[(a >> 14) & 7];
    else if (t) size += t - 1;
    t = attr_type(15, lo);                /* color 1 */
    if (t == 1) size += kColorSize[(a >> 18) & 7];
    else if (t) size += t - 1;
    for (i = 0; i < 8; i++)               /* texture coordinates */
    {
        int cnt, fmtc;

        t = (hi >> (i * 2)) & 3;
        if (!t) continue;
        if (t != 1)
        {
            size += t - 1;
            continue;
        }
        switch (i)
        {
        case 0: cnt = (a >> 21) & 1; fmtc = (a >> 22) & 7; break;
        case 1: cnt = b & 1; fmtc = (b >> 1) & 7; break;
        case 2: cnt = (b >> 9) & 1; fmtc = (b >> 10) & 7; break;
        case 3: cnt = (b >> 18) & 1; fmtc = (b >> 19) & 7; break;
        case 4: cnt = (b >> 27) & 1; fmtc = (b >> 28) & 7; break;
        case 5: cnt = (c >> 5) & 1; fmtc = (c >> 6) & 7; break;
        case 6: cnt = (c >> 14) & 1; fmtc = (c >> 15) & 7; break;
        default: cnt = (c >> 23) & 1; fmtc = (c >> 24) & 7; break;
        }
        size += kCompSize[fmtc] * (cnt ? 2 : 1);
    }
    return size;
}

/* ---- pixel engine / EFB -------------------------------------------------------------------- */

/* An EFB pixel as the copy unit sees it (RGBA8, r in the low byte): colour, or depth
 * spread over r = high, g = middle, b = low byte (a = high) for depth copies. */
static uint32_t copy_texel(int x, int y, int depth)
{
    uint32_t z;

    if (!depth) return sEfb[y * GCN_EFB_W + x];
    z = sDepth[y * GCN_EFB_W + x];
    return ((z >> 16) & 0xFF) | (((z >> 8) & 0xFF) << 8) | ((z & 0xFF) << 16) | (((z >> 16) & 0xFF) << 24);
}

/* the copy's source image, ow x oh: half-scale copies box-filter 2x2 (four channels at
 * once, two per 16-bit lane) */
static uint32_t *sCopySrc;

static void copy_source(int x0, int y0, int x1, int y1, int ow, int oh, int half, int depth, int alpha)
{
    uint32_t force = alpha ? 0 : 0xFF000000u;
    int x, y;

    if (!sCopySrc) sCopySrc = (uint32_t *)malloc(GCN_EFB_W * GCN_EFB_H * 4);

    for (y = 0; y < oh; y++)
    {
        uint32_t *out = sCopySrc + y * ow;

        if (!half)
        {
            int sy = y0 + y > y1 ? y1 : y0 + y;
            for (x = 0; x < ow; x++)
            {
                int sx = x0 + x > x1 ? x1 : x0 + x;
                out[x] = copy_texel(sx, sy, depth) | force;
            }
        }
        else
        {
            int sy0 = y0 + y * 2 > y1 ? y1 : y0 + y * 2, sy1 = sy0 + 1 > y1 ? y1 : sy0 + 1;
            for (x = 0; x < ow; x++)
            {
                int sx0 = x0 + x * 2 > x1 ? x1 : x0 + x * 2, sx1 = sx0 + 1 > x1 ? x1 : sx0 + 1;
                uint32_t a = copy_texel(sx0, sy0, depth), b = copy_texel(sx1, sy0, depth);
                uint32_t c = copy_texel(sx0, sy1, depth), d = copy_texel(sx1, sy1, depth);
                uint32_t rb = (((a & 0x00FF00FFu) + (b & 0x00FF00FFu) + (c & 0x00FF00FFu) + (d & 0x00FF00FFu)) >> 2) & 0x00FF00FFu;
                uint32_t ag = ((((a >> 8) & 0x00FF00FFu) + ((b >> 8) & 0x00FF00FFu) + ((c >> 8) & 0x00FF00FFu) +
                                ((d >> 8) & 0x00FF00FFu)) >> 2) & 0x00FF00FFu;
                out[x] = (rb | (ag << 8)) | force;
            }
        }
    }
}

/* Copy (part of) the EFB into guest memory as a texture, in the tiled layout the texture
 * unit reads: render-to-texture for shadows, blur, heat haze, screen transitions... */
static void efb_copy_to_texture(uint32_t cmd, int x0, int y0, int w, int h)
{
    int raw = (cmd >> 3) & 15, fmt = (raw >> 1) | ((raw & 1) << 3);
    int half = (cmd >> 9) & 1, intensity = (cmd >> 15) & 1;
    int pefmt = sBP[0x43] & 7, depth = pefmt == 3, alpha = pefmt == 1;
    uint32_t dst = (sBP[0x4B] & 0xFFFFFF) << 5, stride = (sBP[0x4D] & 0x3FF) << 5;
    int bw, bh, bpp, bytes, ow, oh, bx, by, px, py, x1, y1;

    if (x0 >= GCN_EFB_W || y0 >= GCN_EFB_H) return;
    x1 = x0 + w - 1;
    y1 = y0 + h - 1;
    if (x1 >= GCN_EFB_W) x1 = GCN_EFB_W - 1;
    if (y1 >= GCN_EFB_H) y1 = GCN_EFB_H - 1;
    ow = half ? w / 2 : w;
    oh = half ? h / 2 : h;
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    switch (fmt)
    {
    case 0x0: bw = 8; bh = 8; bpp = 4; break;                 /* R4 / I4 */
    case 0x1: case 0x2: case 0x7: case 0x8: case 0x9: case 0xA:
        bw = 8; bh = 4; bpp = 8; break;                       /* R8, RA4, A8, G8, B8 */
    case 0x6: bw = 4; bh = 4; bpp = 32; break;                /* RGBA8: two 32-byte planes */
    default: bw = 4; bh = 4; bpp = 16; break;                 /* RA8, RGB565, RGB5A3, RG8, GB8 */
    }
    bytes = bw * bh * bpp / 8;
    if (!stride) stride = (uint32_t)(((ow + bw - 1) / bw) * bytes);
    if (fmt == 0x6 && stride < (uint32_t)(((ow + bw - 1) / bw) * bytes)) stride *= 2;
    copy_source(x0, y0, x1, y1, ow, oh, half, depth, alpha || depth);
    if (getenv("GCN_TRACE_COPIES"))
        fprintf(stderr, "efb copy %d,%d %dx%d -> %08X %dx%d fmt %X stride %u%s%s%s pe %d\n", x0, y0, w, h, (unsigned)dst, ow, oh,
                fmt, (unsigned)stride, half ? " half" : "", intensity ? " intensity" : "", (cmd >> 11) & 1 ? " clear" : "", pefmt);
    {
        /* GCN_DUMP_COPIES=dir:first:count writes copies first.. as PPM (what the copy reads) */
        static int seq;
        const char *e = getenv("GCN_DUMP_COPIES");
        char dir[260];
        int first = 0, count = 0;

        if (e && sscanf(e, "%259[^:]:%d:%d", dir, &first, &count) == 3 && seq >= first && seq < first + count)
        {
            char path[300];
            FILE *f;

            snprintf(path, sizeof(path), "%s/copy_%05d_%08X_f%X.ppm", dir, seq, (unsigned)dst, fmt);
            f = fopen(path, "wb");
            if (f)
            {
                fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                for (py = 0; py < oh; py++)
                {
                    for (px = 0; px < ow; px++)
                    {
                        uint32_t p = sCopySrc[py * ow + px];
                        uint8_t c[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)};
                        fwrite(c, 1, 3, f);
                    }
                }
                fclose(f);
            }
        }
        seq++;
    }

    for (by = 0; by < (oh + bh - 1) / bh; by++)
    {
        for (bx = 0; bx < (ow + bw - 1) / bw; bx++)
        {
            uint8_t *b = guest(dst + (uint32_t)by * stride + (uint32_t)(bx * bytes));
            int i = 0;

            for (py = 0; py < bh; py++)
            {
                for (px = 0; px < bw; px++, i++)
                {
                    int x = bx * bw + px, y = by * bh + py, r, g, bl, a, v;
                    uint32_t c;

                    if (x >= ow) x = ow - 1;
                    if (y >= oh) y = oh - 1;
                    c = sCopySrc[y * ow + x];
                    r = c & 0xFF; g = (c >> 8) & 0xFF; bl = (c >> 16) & 0xFF; a = c >> 24;
                    if (intensity && !depth && fmt < 4) /* RGB to Y (BT.601, 16-235) */
                        r = 16 + ((66 * r + 129 * g + 25 * bl + 128) >> 8);
                    switch (fmt)
                    {
                    case 0x0:
                        if (i & 1) b[i >> 1] = (uint8_t)((b[i >> 1] & 0xF0) | (r >> 4));
                        else b[i >> 1] = (uint8_t)(r & 0xF0);
                        break;
                    case 0x1: case 0x8: b[i] = (uint8_t)r; break;
                    case 0x2: b[i] = (uint8_t)((a & 0xF0) | (r >> 4)); break;
                    case 0x7: b[i] = (uint8_t)a; break;
                    case 0x9: b[i] = (uint8_t)g; break;
                    case 0xA: b[i] = (uint8_t)bl; break;
                    case 0x3: b[i * 2] = (uint8_t)a; b[i * 2 + 1] = (uint8_t)r; break;
                    case 0xB: b[i * 2] = (uint8_t)g; b[i * 2 + 1] = (uint8_t)r; break;
                    case 0xC: b[i * 2] = (uint8_t)bl; b[i * 2 + 1] = (uint8_t)g; break;
                    case 0x4:
                        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3);
                        b[i * 2] = (uint8_t)(v >> 8); b[i * 2 + 1] = (uint8_t)v;
                        break;
                    case 0x5:
                        if (a >= 0xE0) v = 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (bl >> 3);
                        else v = ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (bl >> 4);
                        b[i * 2] = (uint8_t)(v >> 8); b[i * 2 + 1] = (uint8_t)v;
                        break;
                    case 0x6:
                        b[i * 2] = (uint8_t)a; b[i * 2 + 1] = (uint8_t)r;
                        b[32 + i * 2] = (uint8_t)g; b[32 + i * 2 + 1] = (uint8_t)bl;
                        break;
                    default:
                        b[i * 2] = (uint8_t)a; b[i * 2 + 1] = (uint8_t)r;
                        break;
                    }
                }
            }
        }
    }
    gcn_raster_invalidate_textures();
}

static void efb_copy(uint32_t cmd)
{
    uint32_t src = sBP[0x49], dims = sBP[0x4A];
    int x0 = src & 0x3FF, y0 = (src >> 10) & 0x3FF;
    int w = (dims & 0x3FF) + 1, h = ((dims >> 10) & 0x3FF) + 1;
    int to_xfb = (cmd >> 14) & 1, clear = (cmd >> 11) & 1;
    int x, y;

    sStats.efb_copies++;
    gcn_raster_flush(); /* the copy reads (and may clear) what queued triangles draw */
    if (to_xfb)
    {
        if (w > GCN_EFB_W) w = GCN_EFB_W;
        if (h > GCN_EFB_H) h = GCN_EFB_H;
        for (y = 0; y < h && y0 + y < GCN_EFB_H; y++)
        {
            memcpy(&sDisplay[y * GCN_EFB_W], &sEfb[(y0 + y) * GCN_EFB_W + x0], (size_t)w * 4);
        }
        sDisplayW = w;
        sDisplayH = h;
        gcn_overlay_draw(sDisplay, GCN_EFB_W, w, h); /* mods' on-screen text */
        sLastStats = sStats;
        memset(&sStats, 0, sizeof(sStats));
        gcn_raster_invalidate_textures();
        gcn_raster_frame_done();
    }
    else
    {
        efb_copy_to_texture(cmd, x0, y0, w, h);
    }
    if (clear)
    {
        uint32_t ar = sBP[0x4F], gb = sBP[0x50], z = sBP[0x51] & 0xFFFFFF;
        uint32_t rgba = ((ar & 0xFF) << 0) | (((gb >> 8) & 0xFF) << 8) | ((gb & 0xFF) << 16) | (((ar >> 8) & 0xFF) << 24);

        for (y = y0; y < y0 + h && y < GCN_EFB_H; y++)
        {
            for (x = x0; x < x0 + w && x < GCN_EFB_W; x++)
            {
                sEfb[y * GCN_EFB_W + x] = rgba;
                sDepth[y * GCN_EFB_W + x] = z;
            }
        }
    }
}

/* ---- passthrough to the console's GPU ------------------------------------------------------ */
#if GCN_GPU_PASSTHROUGH
/* the write-gather pipe: what is written here goes into the GPU's command FIFO */
#define PIPE_U8 (*(volatile uint8_t *)0xCC008000)
#define PIPE_U16 (*(volatile uint16_t *)0xCC008000)
#define PIPE_U32 (*(volatile uint32_t *)0xCC008000)

static uint32_t sDisplayPhys;            /* the host's display texture (GX RGBA8), physical */
static int sEmit = 1;                    /* 0: a display list the GPU runs itself is only walked */
static int sDisplayTexW, sDisplayTexH;

void gcn_gpu_set_display(void *rgba8_tiled, int width, int height)
{
    sDisplayPhys = (uint32_t)(uintptr_t)rgba8_tiled & 0x3FFFFFFFu;
    sDisplayTexW = width;
    sDisplayTexH = height;
}

/* a guest (physical or virtual) address as the GPU's physical address of that byte of the
 * guest memory buffer */
static uint32_t pt_phys(uint32_t guest_addr)
{
    return (((uint32_t)(uintptr_t)sMem) & 0x3FFFFFFFu) + GCNW_OFFSET(guest_addr);
}

static void pt_bytes(const uint8_t *p, uint32_t n)
{
    if (!sEmit) return;
    for (; n >= 4; n -= 4, p += 4) PIPE_U32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    for (; n; n--, p++) PIPE_U8 = *p;
}

static void pt_bp(uint32_t reg, uint32_t val)
{
    if (!sEmit) return;
    PIPE_U8 = 0x61;
    PIPE_U32 = (reg << 24) | (val & 0xFFFFFF);
}

static void pt_cp(uint32_t reg, uint32_t val)
{
    if (!sEmit) return;
    PIPE_U8 = 0x08;
    PIPE_U8 = (uint8_t)reg;
    PIPE_U32 = val;
}

/* a BP write on its way to the GPU (the full register value: masks are applied here) */
static void pt_bp_write(uint32_t reg, uint32_t val)
{
    switch (reg)
    {
    case 0x45: /* draw done, tokens: emulated (the host's GX library owns the interrupts) */
    case 0x47:
    case 0x48:
    case 0xFE:
        return;
    case 0x4B: /* EFB copy destination */
    case 0x60: /* texture preload source */
    case 0x64: /* TLUT load source */
    case 0x94: case 0x95: case 0x96: case 0x97: /* texture images (maps 0-3, 4-7) */
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        val = (pt_phys((val & 0xFFFFFF) << 5) >> 5) & 0xFFFFFF;
        break;
    case 0x52: /* copy */
        if ((val >> 14) & 1)
        {
            /* to the external frame buffer: into the host's display texture instead, as
             * RGB565 (4x4 tiles of 32 bytes; no alpha, which the host would blend by); the
             * clear (and clamps) as the game asked */
            uint32_t w = (sBP[0x4A] & 0x3FF) + 1;
            if (!sDisplayPhys)
            {
                sFrameDone = 1; /* the picture stays in the EFB: the host's turn (gcnw_backend.c) */
                return;
            }
            if ((int)w > sDisplayTexW) w = (uint32_t)sDisplayTexW;
            pt_bp(0x4D, (uint32_t)sDisplayTexW * 4u * 2u / 32u); /* bytes per row of tiles / 32 */
            pt_bp(0x4B, sDisplayPhys >> 5);
            /* RGB565 (format 4 in bits 4-6), converted from the pixel type (bit 16, as GX does) */
            val = (val & ((1u << 11) | 3u)) | (4u << 4) | (1u << 16);
            pt_bp(0x52, val);
            /* the game's own destination for later copies to textures */
            pt_bp(0x4B, (pt_phys((sBP[0x4B] & 0xFFFFFF) << 5) >> 5) & 0xFFFFFF);
            pt_bp(0x4D, sBP[0x4D] & 0xFFFFFF);
            return;
        }
        break;
    }
    pt_bp(reg, val);
}

void gcn_gpu_resume(void)
{
    uint32_t i;

    /* command processor: matrix indices, vertex descriptor and formats, arrays */
    for (i = 0x30; i < 0xC0; i++)
    {
        uint32_t val = sCP[i];
        if (!IS_SET(sCPSet, i)) continue;
        if (i >= 0xA0 && i <= 0xAF) val = pt_phys(val);
        pt_cp(i, val);
    }
    /* transform unit: memory (matrices, lights) and registers, in runs of at most 16 words;
     * not its error/diagnostic/clock/performance registers */
    for (i = 0; i < 0x1100;)
    {
        uint32_t n = 0, k;
#define XF_REPLAY(a) (IS_SET(sXFSet, (a)) && !((a) >= 0x1000 && (a) <= 0x1007 && (a) != 0x1005))
        if (!XF_REPLAY(i))
        {
            i++;
            continue;
        }
        while (n < 16 && i + n < 0x1100 && XF_REPLAY(i + n) && ((i + n) & 0xFF00) == (i & 0xFF00))
            n++;
#undef XF_REPLAY
        PIPE_U8 = 0x10;
        PIPE_U32 = ((n - 1) << 16) | i;
        for (k = 0; k < n; k++) PIPE_U32 = sXF[i + k];
        i += n;
    }
    /* pixel side: every state register the game set; not the ones that start work (copies,
     * loads, preloads, cache invalidation, tokens, bounding box, performance) */
    for (i = 0; i < 0xFE; i++)
    {
        if (!IS_SET(sBPSet, i)) continue;
        switch (i)
        {
        case 0x45: case 0x46: case 0x47: case 0x48:
        case 0x52: case 0x55: case 0x56:
        case 0x63: case 0x65: case 0x66: case 0x67: case 0x69:
            continue;
        }
        pt_bp_write(i, sBP[i]);
    }
    /* the host's textures and vertices may sit where the game's were cached */
    pt_bp(0x66, 0x001000);
    pt_bp(0x66, 0x001100);
    PIPE_U8 = 0x48;
}
#else
void gcn_gpu_set_display(void *rgba8_tiled, int width, int height)
{
    (void)rgba8_tiled;
    (void)width;
    (void)height;
}

void gcn_gpu_resume(void)
{
}
#endif

static void bp_write(uint32_t v)
{
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;

    if (reg == 0xFE) /* BP mask: applies to the next write */
    {
        sBP[0xFE] = val;
        return;
    }
    if (sBP[0xFE] != 0xFFFFFF && sBP[0xFE] != 0)
    {
        val = (sBP[reg] & ~sBP[0xFE]) | (val & sBP[0xFE]);
    }
    sBP[0xFE] = 0xFFFFFF;
    sBP[reg] = val;
    MARK_SET(sBPSet, reg);
    gcn_gpu_state_serial++;
    sStats.bp_writes++;
#if GCN_GPU_PASSTHROUGH
    pt_bp_write(reg, val);
    switch (reg)
    {
    case 0x45: /* PE done */
        if (val & 2) sPending |= 1u << 19;
        break;
    case 0x47:
    case 0x48:
        if (sTokenCount && sTokens[(sTokenHead + sTokenCount - 1) % TOKEN_QUEUE] == (uint16_t)val) break;
        if (sTokenCount < TOKEN_QUEUE)
        {
            sTokens[(sTokenHead + sTokenCount) % TOKEN_QUEUE] = (uint16_t)val;
            sTokenCount++;
        }
        break;
    case 0x52:
        sStats.efb_copies++;
        if ((val >> 14) & 1)
        {
            sDisplayCopies++;
            sLastStats = sStats;
            memset(&sStats, 0, sizeof(sStats));
        }
        break;
    }
    return;
#endif
    switch (reg)
    {
    case 0x45: /* PE done */
        if (val & 2) sPending |= 1u << 19;
        break;
    case 0x47: /* token, no interrupt */
    case 0x48: /* token with interrupt */
        /* Commands run the moment they are written, but games pace themselves on the token
         * the GPU reports per frame (draw sync) and expect it to advance one frame at a time:
         * tokens become visible one per vertical retrace (gcn_gpu_retrace). */
        if (sTokenCount && sTokens[(sTokenHead + sTokenCount - 1) % TOKEN_QUEUE] == (uint16_t)val) break;
        if (sTokenCount < TOKEN_QUEUE)
        {
            sTokens[(sTokenHead + sTokenCount) % TOKEN_QUEUE] = (uint16_t)val;
            sTokenCount++;
        }
        break;
    case 0x52:
        efb_copy(val);
        break;
    case 0x65: /* load a TLUT into texture memory (source address in 0x64) */
    {
        uint32_t src = (sBP[0x64] & 0xFFFFFF) << 5, dst = (val & 0x3FF) << 9, bytes = ((val >> 10) & 0x7FF) << 5;
        if (dst + bytes > TMEM_BYTES) bytes = TMEM_BYTES - dst;
        memcpy(sTmem + dst, guest(src), bytes);
        gcn_raster_invalidate_textures();
        break;
    }
    case 0x66: /* invalidate texture cache */
        gcn_raster_invalidate_textures();
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        /* TEV color registers / konstant colors (bit 23 says which) */
        if (val & 0x800000) gcn_gpu_tev_konst[(reg - 0xE0) >> 1][reg & 1] = val;
        else gcn_gpu_tev_color[(reg - 0xE0) >> 1][reg & 1] = val;
        break;
    }
}

/* ---- XF ---------------------------------------------------------------------------------------- */
static void xf_write(uint32_t addr, uint32_t value)
{
    sStats.xf_writes++;
    if (addr < 0x1100)
    {
        sXF[addr] = value;
        MARK_SET(sXFSet, addr);
    }
    if (addr >= 0x1000) gcn_gpu_state_serial++;
}

static void xf_indexed(int array, uint32_t v)
{
    uint32_t index = v >> 16, len = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    uint32_t base = sCP[0xA0 + 12 + array], stride = sCP[0xB0 + 12 + array];
    const uint8_t *src = guest(base + index * stride);
    uint32_t i;

    for (i = 0; i < len; i++) xf_write(addr + i, be32(src + i * 4));
}

/* ---- the command stream ------------------------------------------------------------------- */
static int run_commands(const uint8_t *p, uint32_t len, int depth);

/* Primitive vertices: parsed for their size now; the rasteriser consumes them later. */
/* ---- the vertex loader ------------------------------------------------------------------- */
static float read_comp(const uint8_t *p, int fmt, int shift)
{
    float scale = 1.0f / (float)(1 << shift);
    switch (fmt)
    {
    case 0: return p[0] * scale;
    case 1: return (int8_t)p[0] * scale;
    case 2: return (uint16_t)((p[0] << 8) | p[1]) * scale;
    case 3: return (int16_t)((p[0] << 8) | p[1]) * scale;
    case 4:
    {
        uint32_t v = be32(p);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    }
    return 0.0f;
}

static void read_color(const uint8_t *p, int fmt, float *c)
{
    switch (fmt)
    {
    case 0: /* RGB565 */
    {
        uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
        c[0] = ((v >> 11) & 31) / 31.0f; c[1] = ((v >> 5) & 63) / 63.0f; c[2] = (v & 31) / 31.0f; c[3] = 1.0f;
        break;
    }
    case 1: case 2: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[2] / 255.0f; c[3] = 1.0f; break;
    case 3: /* RGBA4444 */
        c[0] = (p[0] >> 4) / 15.0f; c[1] = (p[0] & 15) / 15.0f; c[2] = (p[1] >> 4) / 15.0f; c[3] = (p[1] & 15) / 15.0f;
        break;
    case 4: /* RGBA6666 */
    {
        uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        c[0] = ((v >> 18) & 63) / 63.0f; c[1] = ((v >> 12) & 63) / 63.0f; c[2] = ((v >> 6) & 63) / 63.0f; c[3] = (v & 63) / 63.0f;
        break;
    }
    default: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[2] / 255.0f; c[3] = p[3] / 255.0f; break;
    }
}

/* Where an attribute's data is: in the vertex stream (direct) or in an array (indexed). */
static const uint8_t *attr_data(const uint8_t **cursor, int type, int array, int direct_size)
{
    const uint8_t *p = *cursor;
    uint32_t index;

    if (type == 1)
    {
        *cursor += direct_size;
        return p;
    }
    if (type == 2)
    {
        index = p[0];
        *cursor += 1;
    }
    else
    {
        index = (uint32_t)((p[0] << 8) | p[1]);
        *cursor += 2;
    }
    return guest(sCP[0xA0 + array] + index * sCP[0xB0 + array]);
}

#if GCN_GPU_PASSTHROUGH
static GcnVertexIn sVerts[1]; /* the software path's vertices: unused there */
#else
static GcnVertexIn sVerts[65536 / 8];
#endif

static uint32_t draw(int cmd, int fmt, const uint8_t *data, uint32_t count)
{
    uint32_t size = (uint32_t)vertex_size(fmt);
    uint32_t lo = sCP[0x50], hi = sCP[0x60];
    uint32_t a = sCP[0x70 + fmt], b = sCP[0x80 + fmt], c = sCP[0x90 + fmt];
    const uint8_t *cur = data;
    uint32_t n, i;
    int k;

    sStats.primitives++;
    sStats.vertices += count;
    if (count > sizeof(sVerts) / sizeof(sVerts[0])) count = sizeof(sVerts) / sizeof(sVerts[0]);
    for (n = 0; n < count; n++)
    {
        GcnVertexIn *v = &sVerts[n];
        int t;

        memset(v, 0, sizeof(*v));
        v->pnmtx = -1;
        for (k = 0; k < 8; k++) v->texmtx[k] = -1;
        if (lo & 1) v->pnmtx = *cur++;
        for (k = 0; k < 8; k++)
        {
            if (lo & (2u << k)) v->texmtx[k] = *cur++;
        }
        /* position */
        t = attr_type(9, lo);
        if (t)
        {
            int pfmt = (a >> 1) & 7, cnt = (a & 1) ? 3 : 2, sh = (a >> 4) & 31;
            const uint8_t *p = attr_data(&cur, t, 0, kCompSize[pfmt] * cnt);
            for (k = 0; k < cnt; k++) v->pos[k] = read_comp(p + k * kCompSize[pfmt], pfmt, sh);
        }
        /* normal (+ binormal and tangent) */
        t = attr_type(11, lo);
        if (t)
        {
            int nfmt = (a >> 10) & 7, nbt = (a >> 9) & 1, index3 = (a >> 31) & 1, cs = kCompSize[nfmt];
            int sh = nfmt == 1 ? 6 : nfmt == 3 ? 14 : 0;
            float *dst[3] = {v->nrm, v->bin, v->tan};
            int parts = nbt ? 3 : 1, part;
            const uint8_t *p = NULL;

            for (part = 0; part < parts; part++)
            {
                if (t == 1) p = attr_data(&cur, 1, 1, cs * 3);          /* direct: one after another */
                else if (part == 0 || index3) p = attr_data(&cur, t, 1, 0); /* an index per part */
                else p += cs * 3;                                          /* one index, parts adjacent */
                for (k = 0; k < 3; k++) dst[part][k] = read_comp(p + k * cs, nfmt, sh);
            }
            v->has_nrm = 1;
        }
        /* colors */
        for (k = 0; k < 2; k++)
        {
            int cfmt = (a >> (14 + k * 4)) & 7;
            t = attr_type(13 + k * 2, lo);
            if (t)
            {
                const uint8_t *p = attr_data(&cur, t, 2 + k, kColorSize[cfmt]);
                read_color(p, cfmt, v->col[k]);
                if (!((a >> (13 + k * 4)) & 1)) v->col[k][3] = 1.0f;
                v->has_col[k] = 1;
            }
        }
        /* texture coordinates */
        for (k = 0; k < 8; k++)
        {
            int cnt, tf, sh;
            const uint8_t *p;

            t = (hi >> (k * 2)) & 3;
            if (!t) continue;
            switch (k)
            {
            case 0: cnt = (a >> 21) & 1; tf = (a >> 22) & 7; sh = (a >> 25) & 31; break;
            case 1: cnt = b & 1; tf = (b >> 1) & 7; sh = (b >> 4) & 31; break;
            case 2: cnt = (b >> 9) & 1; tf = (b >> 10) & 7; sh = (b >> 13) & 31; break;
            case 3: cnt = (b >> 18) & 1; tf = (b >> 19) & 7; sh = (b >> 22) & 31; break;
            case 4: cnt = (b >> 27) & 1; tf = (b >> 28) & 7; sh = c & 31; break;
            case 5: cnt = (c >> 5) & 1; tf = (c >> 6) & 7; sh = (c >> 9) & 31; break;
            case 6: cnt = (c >> 14) & 1; tf = (c >> 15) & 7; sh = (c >> 18) & 31; break;
            default: cnt = (c >> 23) & 1; tf = (c >> 24) & 7; sh = (c >> 27) & 31; break;
            }
            p = attr_data(&cur, t, 4 + k, kCompSize[tf] * (cnt ? 2 : 1));
            v->tex[k][0] = read_comp(p, tf, sh);
            if (cnt) v->tex[k][1] = read_comp(p + kCompSize[tf], tf, sh);
            v->has_tex[k] = 1;
        }
        /* keep the stream in step with the computed vertex size whatever was decoded */
        cur = data + (n + 1) * size;
    }
    (void)i;
    gcn_raster_primitive(cmd, sVerts, (int)count);
    return size * count;
}

#if GCN_GPU_PASSTHROUGH
/* Whether the GPU can run a display list as it is: laid out as it wants (32-byte aligned)
 * and nothing in it needs the runtime - addresses to translate (array bases, texture images,
 * copies), commands the runtime emulates (draw done, tokens, copies to the screen) or calls
 * of further lists. Vertex formats the list sets count for the sizes of its draws. */
static int dl_direct(const uint8_t *p, uint32_t len, uint32_t addr)
{
    uint32_t saved[0xA0 - 0x50], pos = 0;
    int ok = 1;

    if ((addr & 31) || (len & 31) || len == 0) return 0;
    memcpy(saved, &sCP[0x50], sizeof(saved)); /* descriptors and formats 0x50-0x9F */
    while (ok && pos < len)
    {
        uint8_t cmd = p[pos];

        switch (cmd)
        {
        case 0x00:
        case 0x44:
        case 0x48:
            pos++;
            break;
        case 0x08:
            if (pos + 6 > len || (p[pos + 1] >= 0xA0 && p[pos + 1] <= 0xAF))
            {
                ok = 0;
                break;
            }
            if (p[pos + 1] >= 0x50 && p[pos + 1] < 0xA0) sCP[p[pos + 1]] = be32(p + pos + 2);
            pos += 6;
            break;
        case 0x10:
            if (pos + 5 > len)
            {
                ok = 0;
                break;
            }
            pos += 5 + ((be32(p + pos + 1) >> 16) + 1) * 4;
            break;
        case 0x20: case 0x28: case 0x30: case 0x38:
            pos += 5;
            break;
        case 0x61:
            if (pos + 5 > len)
            {
                ok = 0;
                break;
            }
            switch (p[pos + 1])
            {
            case 0x45: case 0x47: case 0x48: case 0x4B: case 0x52: case 0x60: case 0x64:
            case 0x94: case 0x95: case 0x96: case 0x97:
            case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                ok = 0;
                break;
            }
            pos += 5;
            break;
        default:
            if (cmd >= 0x80 && cmd < 0xC0 && pos + 3 <= len)
            {
                uint32_t count = ((uint32_t)p[pos + 1] << 8) | p[pos + 2];
                int size = vertex_size(cmd & 7);
                if (size <= 0 && count)
                {
                    ok = 0;
                    break;
                }
                pos += 3 + (uint32_t)size * count;
                break;
            }
            ok = 0;
            break;
        }
    }
    memcpy(&sCP[0x50], saved, sizeof(saved));
    return ok && pos <= len;
}
#endif

/* Parses commands; returns bytes consumed (stops early at an incomplete command). */
/* the pipe's command in progress needs this many bytes before parsing can go on */
static uint32_t sFifoNeed;
/* passthrough: vertex data of the pipe's draw in progress still to come; it goes to the GPU
 * as it is written, without being buffered or parsed (fifo_write) */
uint32_t gcn_gpu_payload;
/* passthrough: an XF load in progress - its words go straight on, kept in sXF as they pass */
static uint32_t sXfLeft, sXfAddr, sXfWord;
static int sXfBytes;

static uint32_t parse(const uint8_t *p, uint32_t len, int depth)
{
    uint32_t pos = 0;

    while (pos < len)
    {
        uint8_t cmd = p[pos];
        uint32_t need;

        switch (cmd)
        {
        case 0x00:
            pos++;
            continue;
        case 0x08: /* CP register */
            if (pos + 6 > len)
            {
                if (depth == 0) sFifoNeed = 6;
                return pos;
            }
            sCP[p[pos + 1]] = be32(p + pos + 2);
            MARK_SET(sCPSet, p[pos + 1]);
            sStats.cp_writes++;
#if GCN_GPU_PASSTHROUGH
            {
                uint32_t reg = p[pos + 1], val = sCP[reg];
                /* vertex array bases (12 arrays + 4 indexed XF arrays): physical addresses */
                if (reg >= 0xA0 && reg <= 0xAF) val = pt_phys(val);
                pt_cp(reg, val);
            }
#endif
            pos += 6;
            continue;
        case 0x10: /* XF registers */
        {
            uint32_t head, count, addr, i;

            if (pos + 5 > len)
            {
                if (depth == 0) sFifoNeed = 5;
                return pos;
            }
            head = be32(p + pos + 1);
            count = (head >> 16) + 1;
            addr = head & 0xFFFF;
            need = 5 + count * 4;
            if (pos + need > len)
            {
#if GCN_GPU_PASSTHROUGH
                if (depth == 0 && sEmit && ((len - pos - 5) & 3) == 0)
                {
                    /* the header and the words so far go out; the rest streams from the
                     * pipe (fifo_write), each word also kept for gcn_gpu_resume */
                    uint32_t have = (len - pos - 5) / 4;
                    for (i = 0; i < have; i++) xf_write(addr + i, be32(p + pos + 5 + i * 4));
                    pt_bytes(p + pos, 5 + have * 4);
                    sXfAddr = addr + have;
                    sXfLeft = count - have;
                    sXfWord = 0;
                    sXfBytes = 0;
                    return len;
                }
#endif
                if (depth == 0) sFifoNeed = need;
                return pos;
            }
            for (i = 0; i < count; i++) xf_write(addr + i, be32(p + pos + 5 + i * 4));
#if GCN_GPU_PASSTHROUGH
            pt_bytes(p + pos, need);
#endif
            pos += need;
            continue;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:
            if (pos + 5 > len)
            {
                if (depth == 0) sFifoNeed = 5;
                return pos;
            }
#if GCN_GPU_PASSTHROUGH
            pt_bytes(p + pos, 5); /* the GPU reads the array (its base is translated) */
#else
            xf_indexed((cmd - 0x20) >> 3, be32(p + pos + 1));
#endif
            pos += 5;
            continue;
        case 0x40: /* call display list */
            if (pos + 9 > len)
            {
                if (depth == 0) sFifoNeed = 9;
                return pos;
            }
            if (depth < 4)
            {
                uint32_t addr = be32(p + pos + 1), size = be32(p + pos + 5);
                sStats.display_lists++;
#if GCN_GPU_PASSTHROUGH
                if (sEmit && dl_direct(guest(addr), size, addr))
                {
                    /* the GPU runs the list from memory; walking it keeps the register copies
                     * (vertex sizes, the state given back after the host drew) current */
                    PIPE_U8 = 0x40;
                    PIPE_U32 = pt_phys(addr);
                    PIPE_U32 = size;
                    sEmit = 0;
                    run_commands(guest(addr), size, depth + 1);
                    sEmit = 1;
                }
                else
#endif
                run_commands(guest(addr), size, depth + 1);
            }
            pos += 9;
            continue;
        case 0x44: /* unknown, one byte */
        case 0x48: /* invalidate vertex cache */
#if GCN_GPU_PASSTHROUGH
            if (sEmit) PIPE_U8 = cmd;
#endif
            pos++;
            continue;
        case 0x61: /* BP register */
            if (pos + 5 > len)
            {
                if (depth == 0) sFifoNeed = 5;
                return pos;
            }
            bp_write(be32(p + pos + 1));
            pos += 5;
            continue;
        default:
            if (cmd >= 0x80 && cmd < 0xC0)
            {
                uint32_t count, bytes;

                if (pos + 3 > len)
                {
                    if (depth == 0) sFifoNeed = 3;
                    return pos;
                }
                count = ((uint32_t)p[pos + 1] << 8) | p[pos + 2];
                bytes = (uint32_t)vertex_size(cmd & 7) * count;
                if (pos + 3 + bytes > len)
                {
#if GCN_GPU_PASSTHROUGH
                    if (depth == 0)
                    {
                        /* the header and the vertex data so far go out; the rest follows
                         * straight from the pipe */
                        uint32_t have = len - pos - 3;
                        sStats.primitives++;
                        sStats.vertices += count;
                        pt_bytes(p + pos, 3 + have);
                        gcn_gpu_payload = bytes - have;
                        return len;
                    }
#endif
                    if (depth == 0) sFifoNeed = 3 + bytes; /* bytes from pos, which becomes 0 */
                    return pos;
                }
#if GCN_GPU_PASSTHROUGH
                sStats.primitives++;
                sStats.vertices += count;
                pt_bytes(p + pos, 3 + bytes);
#else
                draw(cmd & 0xF8, cmd & 7, p + pos + 3, count);
#endif
                pos += 3 + bytes;
                continue;
            }
            /* unknown: skip a byte, as the hardware would desynchronise anyway */
            sStats.unknown_commands++;
            pos++;
            continue;
        }
    }
    return pos;
}

static int run_commands(const uint8_t *p, uint32_t len, int depth)
{
    return (int)parse(p, len, depth);
}

/* ---- the write-gather pipe ------------------------------------------------------------------- */
#define FIFO_SIZE (1u << 20)
static uint8_t sFifo[FIFO_SIZE];
static uint32_t sFifoLen;

/* While the CPU FIFO is not linked to the GPU (GXBeginDisplayList points it at the list
 * buffer), the pipe writes to memory at the PI write pointer, 32 bytes at a time, as the
 * hardware does; GXEndDisplayList reads the pointer back for the list's size. */
static uint8_t sGather[32];
static int sGatherLen;

/* whether the pipe writes to memory (GXBeginDisplayList), kept up to date by
 * gcn_gpu_reg_write: the registers it depends on only change there */
static int sDetached;

/* While detached, the pipe's bytes go straight into the list's memory at gcn_gpu_dl_ptr
 * (also inline in the generated code, gcnw_ops.h); the PI write pointer is worked out from it
 * when the game reads it, in whole 32-byte bursts as the hardware moves it, and the list is
 * written back from the CPU cache when the pipe is linked to the GPU again. */
uint8_t *gcn_gpu_dl_ptr, *gcn_gpu_dl_end;
static uint8_t *sDlStart;
static uint32_t sDlWrap;

static void dl_begin(void)
{
    const uint32_t wp = sPiRegs[5] & 0x03FFFFFF, base = sPiRegs[3] & 0x03FFFFFF, top = sPiRegs[4] & 0x03FFFFFF;
    sDlWrap = sPiRegs[5] & 0x04000000;
    sDlStart = guest(wp);
    gcn_gpu_dl_ptr = sDlStart;
    /* the end of the FIFO (writes there wrap: fifo_to_memory), else of guest memory */
    gcn_gpu_dl_end = top > base && top > wp ? guest(top) : sMem + GCNW_OFFSET(0x01800000u) - 32;
    sGatherLen = 0;
}

static void dl_sync(void)
{
    if (!gcn_gpu_dl_ptr) return;
    sPiRegs[5] = ((uint32_t)(gcn_gpu_dl_ptr - sMem) & ~31u) | sDlWrap;
}

static void dl_finish(void)
{
    dl_sync();
    if (gcn_gpu_dl_ptr > sDlStart) gcnw_dcache_flush(sDlStart, (size_t)(gcn_gpu_dl_ptr - sDlStart));
    gcn_gpu_dl_ptr = gcn_gpu_dl_end = NULL;
}

static void update_detached(void)
{
    uint32_t gp_base = ((uint32_t)sCpRegs[17] << 16) | sCpRegs[16];
    const int was = sDetached;
    sDetached = !(sCpRegs[1] & 0x10) && sPiRegs[3] != (gp_base & 0x3FFFFFFF);
    if (sDetached && !was) dl_begin();
    else if (!sDetached && was) dl_finish();
}

static int fifo_detached(void)
{
    return sDetached;
}

static void fifo_to_memory(uint32_t value, int bytes)
{
    int i;

    if (gcn_gpu_dl_ptr && gcn_gpu_dl_ptr + bytes <= gcn_gpu_dl_end)
    {
        for (i = bytes - 1; i >= 0; i--) *gcn_gpu_dl_ptr++ = (uint8_t)(value >> (i * 8));
        return;
    }
    /* the end of the FIFO: through the hardware's wrap-around, burst by burst */
    if (gcn_gpu_dl_ptr)
    {
        dl_sync();
        sGatherLen = (int)((uint32_t)(gcn_gpu_dl_ptr - sMem) & 31u);
        memcpy(sGather, gcn_gpu_dl_ptr - sGatherLen, (size_t)sGatherLen);
        dl_finish();
    }
    for (i = bytes - 1; i >= 0; i--)
    {
        sGather[sGatherLen++] = (uint8_t)(value >> (i * 8));
        if (sGatherLen == 32)
        {
            uint32_t wp = sPiRegs[5] & 0x03FFFFFF, wrap = sPiRegs[5] & 0x04000000;
            uint32_t base = sPiRegs[3] & 0x03FFFFFF, top = sPiRegs[4] & 0x03FFFFFF;

            memcpy(guest(wp), sGather, 32);
#if GCN_GPU_PASSTHROUGH
            gcnw_dcache_flush(guest(wp), 32); /* the GPU may run this list from memory */
#endif
            wp += 32;
            if (wp >= top && top > base) /* the hardware wraps at the end and flags it */
            {
                wp = base;
                wrap ^= 0x04000000;
            }
            sPiRegs[5] = wp | wrap;
            sGatherLen = 0;
        }
    }
}

static void fifo_write(uint32_t value, int bytes);

void gcn_gpu_fifo_write(uint32_t value, int bytes)
{
    fifo_write(value, bytes);
    sFeedCalls++;
}

static void fifo_write(uint32_t value, int bytes)
{
    int i;

    if (fifo_detached())
    {
        fifo_to_memory(value, bytes);
        return;
    }
    sGatherLen = 0; /* GXFlush padding left over from a display list */
#if GCN_GPU_PASSTHROUGH
    if (gcn_gpu_payload)
    {
        /* a draw's vertex data (the pipe's buffer is empty meanwhile): straight to the GPU */
        if ((uint32_t)bytes <= gcn_gpu_payload)
        {
            if (bytes == 4) PIPE_U32 = value;
            else if (bytes == 2) PIPE_U16 = (uint16_t)value;
            else PIPE_U8 = (uint8_t)value;
            gcn_gpu_payload -= (uint32_t)bytes;
            sStats.fifo_bytes += (uint32_t)bytes;
            return;
        }
        while (gcn_gpu_payload && bytes > 0) /* the write ends the data: its first bytes */
        {
            bytes--;
            PIPE_U8 = (uint8_t)(value >> (bytes * 8));
            gcn_gpu_payload--;
            sStats.fifo_bytes++;
        }
    }
    if (sXfLeft)
    {
        /* an XF load's words: on to the GPU, and into the copy of the XF state */
        if (bytes == 4 && sXfBytes == 0)
        {
            PIPE_U32 = value;
            xf_write(sXfAddr++, value);
            sXfLeft--;
            sStats.fifo_bytes += 4;
            return;
        }
        while (sXfLeft && bytes > 0)
        {
            const uint8_t b = (uint8_t)(value >> (--bytes * 8));
            PIPE_U8 = b;
            sXfWord = (sXfWord << 8) | b;
            sStats.fifo_bytes++;
            if (++sXfBytes == 4)
            {
                xf_write(sXfAddr++, sXfWord);
                sXfLeft--;
                sXfBytes = 0;
            }
        }
        if (!bytes) return;
    }
#endif
    if (bytes == 4)
    {
        sFifo[sFifoLen] = (uint8_t)(value >> 24);
        sFifo[sFifoLen + 1] = (uint8_t)(value >> 16);
        sFifo[sFifoLen + 2] = (uint8_t)(value >> 8);
        sFifo[sFifoLen + 3] = (uint8_t)value;
        sFifoLen += 4;
    }
    else
    {
        for (i = bytes - 1; i >= 0; i--) sFifo[sFifoLen++] = (uint8_t)(value >> (i * 8));
    }
    sStats.fifo_bytes += (uint32_t)bytes;
    /* parse at command boundaries as soon as something complete is there */
    if (sFifoLen >= 1 && sFifoLen >= sFifoNeed)
    {
        uint32_t used;
#if defined(GEKKO)
        const uint64_t t0 = gettime();
#endif

        sFifoNeed = 0;
        used = parse(sFifo, sFifoLen, 0);
#if defined(GEKKO)
        sFeedTicks += gettime() - t0;
#endif
        if (used)
        {
            memmove(sFifo, sFifo + used, sFifoLen - used);
            sFifoLen -= used;
        }
        if (sFifoLen > FIFO_SIZE - 64 || sFifoNeed > FIFO_SIZE - 64)
        {
            sFifoLen = 0; /* desynchronised: start over */
            sFifoNeed = 0;
        }
    }
}

/* ---- MMIO -------------------------------------------------------------------------------------- */
uint32_t gcn_gpu_reg_read(uint32_t addr, int bytes)
{
    uint32_t off = addr & 0xFFF;
    static int trace = -1;

    if (trace < 0) trace = getenv("GCN_TRACE_MMIO") != NULL;
    if (trace) fprintf(stderr, "mmio read %08X (%d)\n", addr, bytes);

    switch (addr & 0xFFFFF000)
    {
    case 0xCC000000: /* CP: status says idle and empty (+ breakpoint reached) */
        if (off == 0) return 0x000C | 0x0002 | (sBreakpointHit ? 0x10 : 0);
        return sCpRegs[(off >> 1) & 0x7F];
    case 0xCC001000: /* PE */
        if (off == 0x0E)
        {
            if (trace) fprintf(stderr, "  token %04X (queued %d)\n", sToken, sTokenCount);
            return sToken;
        }
        return sPeRegs[(off >> 1) & 0x7F];
    case 0xCC003000: /* PI */
        if (sDetached && ((off >> 2) & 0x3F) == 5) dl_sync();
        return sPiRegs[(off >> 2) & 0x3F];
    }
    return 0;
}

void gcn_gpu_reg_write(uint32_t addr, uint32_t value, int bytes)
{
    uint32_t off = addr & 0xFFF;
    static int trace = -1;

    if (trace < 0) trace = getenv("GCN_TRACE_MMIO") != NULL;
    if (trace) fprintf(stderr, "mmio write %08X = %08X (%d)\n", addr, value, bytes);

    switch (addr & 0xFFFFF000)
    {
    case 0xCC000000:
        sCpRegs[(off >> 1) & 0x7F] = (uint16_t)value;
        if (off == 2)
        {
            /* control: a FIFO breakpoint (bit 1) with its interrupt (bit 5). Commands are
             * processed as they are written, so the GPU is already there. */
            if ((value & 0x22) == 0x22)
            {
                sBreakpointHit = 1;
                sPending |= 1u << 17;
            }
            else if (!(value & 2))
            {
                sBreakpointHit = 0;
            }
        }
        break;
    case 0xCC001000:
        sPeRegs[(off >> 1) & 0x7F] = (uint16_t)value;
        break;
    case 0xCC003000:
        if (sDetached && ((off >> 2) & 0x3F) == 5)
        {
            /* a new write pointer while detached (GXSetCPUFifo sets base, top, then this):
             * the list so far is done, the next one starts at the new pointer */
            dl_finish();
            sPiRegs[5] = value;
            dl_begin();
            break;
        }
        sPiRegs[(off >> 2) & 0x3F] = value;
        break;
    }
    update_detached();
}
