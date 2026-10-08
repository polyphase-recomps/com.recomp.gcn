/*
 * GameCube GPU on the host's GPU (Vulkan): the pixel half of the software rasteriser
 * (gcn_raster.c) on a GPU. The CPU still does what is cheap there - vertex transform,
 * lighting, texture coordinate generation, clipping, culling and texture decoding - and hands
 * over screen-space triangles, the state their pixels need (the TEV stages, alpha test, fog:
 * GcnVkState, read by one shader that runs every TEV configuration) and RGBA8 textures. The
 * embedded frame buffer lives on the GPU at the render resolution; gcn_gpu.c reads it back
 * into its own copy when the game copies from it (display and texture copies), so those copies
 * work as before.
 *
 * Own Vulkan instance and device (Vulkan 1.2 with descriptor indexing), loaded at run time
 * (vulkan-1.dll / libvulkan.so.1): no link dependency, and it runs on the game's thread. Windows
 * and Linux; elsewhere gcn_vk_init fails and the software rasteriser draws.
 *
 * The work is batched: triangles, clears and texture uploads are recorded as they come and
 * submitted at a read (gcn_vk_read), when a buffer fills up, or with gcn_vk_sync. Shaders:
 * Runtime/tools/gpu (GLSL), compiled into gcn_vk_spv.h by gen_spv.py.
 */
#ifndef GCN_VK_H
#define GCN_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A vertex in screen space, as gcn_raster.c's Scr: attributes multiplied by 1/w, so the shader
 * interpolates them linearly in screen space and divides by the interpolated 1/w, exactly as the
 * software rasteriser does. */
typedef struct GcnVkVertex
{
    float pos[4];       /* x, y in normalised device coordinates, depth 0..1 (z / 2^24), 1/w */
    float col[2][4];    /* color channels 0..255, times 1/w */
    float tc[8][3];     /* texture coordinates s, t, q, times 1/w */
    uint32_t state;     /* GcnVkState index (gcn_vk_state) */
} GcnVkVertex;

/* What a primitive's pixels need (std430, mirrored in Runtime/tools/gpu/gcn_vk.frag). */
typedef struct GcnVkState
{
    int32_t hdr[4];        /* stages, chanused | tcproj << 8, alpha test: f0 | f1 << 3 | op << 6 | always << 8,
                              references r0 | r1 << 8 */
    int32_t reg[4][4];     /* PREV, REG0..2 (rgba, 11-bit signed) */
    int32_t fog[8];        /* type, ortho, bmag, bshift, r, g, b, - */
    float fogf[4];         /* a, c */
    int32_t smp[8][4];     /* texture slot (-1 none), w, h, wrap s | wrap t << 2 | linear << 4 */
    int32_t stage[16][16]; /* texon, texmap, texcoord, chan, ras swap | tex swap << 8, konst rgba,
                              input k: color slot | alpha << 4 | alpha slot << 8 (4),
                              color op, alpha op (bias | sub << 2 | scale << 3 | clamp << 5),
                              color dest | alpha dest << 4 */
} GcnVkState;

/* A vertex as the vertex loader decoded it (gcn_raster.h GcnVertexIn), for the transform on the
 * GPU (gcn_vk_xf.vert, std430). */
typedef struct GcnVkXfVertex
{
    float pos[3];
    uint32_t mtx;        /* position matrix (0-63), ~0 = from MATINDEX_A */
    float nrm[3];
    uint32_t flags;      /* has normal | has color0 << 1 | has color1 << 2 */
    float bin[3];
    uint32_t texmtx_lo;  /* texture matrices 0-3, a byte each, 0xFF = from MATINDEX */
    float tan[3];
    uint32_t texmtx_hi;  /* 4-7 */
    float col[2][4];     /* 0..1 */
    float tex[8][2];
    uint32_t xf, state;  /* gcn_vk_xf_state, gcn_vk_state */
    uint32_t pad[2];
} GcnVkXfVertex;

/* What the transform unit needs for a primitive (std430, mirrored in gcn_vk_xf.vert): the XF
 * registers and the parts of XF memory the vertices can address. */
typedef struct GcnVkXf
{
    uint32_t misc[4];      /* MATINDEX_A, MATINDEX_B, projection type, textures | channels << 4 | dual texture << 8 */
    uint32_t texgen[8];    /* XF 0x1040-0x1047 */
    uint32_t post[8];      /* XF 0x1050-0x1057 */
    float proj[8];         /* XF 0x1020-0x1025 */
    float vp[8];           /* viewport sx, sy, sz, ox, oy, oz, 2 * scale / frame buffer width, 2 * scale / height */
    float chan[4][4];      /* channel 0 material, ambient; channel 1 material, ambient (0..255) */
    uint32_t chanf[4];     /* channel 0 flags (material from vertex | ambient from vertex << 1 | alpha from
                              vertex << 2 | lit << 3 | diffuse << 4 | attenuation << 6 | spot << 7), lights; channel 1 */
    float light[2][8][4][4]; /* per channel, per enabled light: (color, a0), (position, a1), (direction, a2), (k0-k2, -) */
    float mtx[0x110];      /* XF memory 0x000-0x10F: position and texture matrices */
    float nrm[0x68];       /* 0x400-0x467: normal matrices */
    float pmx[0x110];      /* 0x500-0x60F: post matrices */
} GcnVkXf;

/* The pixel engine state that is fixed-function on the GPU (blending, depth, write masks):
 * GCN_VK_PIPE(...) packs it into a key. */
typedef struct GcnVkPipe
{
    uint8_t blend, sf, df, sub;  /* BP 0x41: enable, source / destination factor (0-7), subtract */
    uint8_t color_upd, alpha_upd;
    uint8_t dst_alpha_on, dst_alpha; /* BP 0x42: constant alpha written */
    uint8_t no_alpha;            /* pixel format without alpha: destination alpha reads 255 */
    uint8_t ztest, zfunc, zupd;  /* BP 0x40 */
    /* gcn_vk_xf_triangles only: the GPU culls (gen mode: 1 back, 2 front, 3 all, front = clockwise
     * on screen) and clips (clip_off: XF 0x1005, the game turned near / far clipping off) */
    uint8_t cull, clip_off;
} GcnVkPipe;

/* 1: the GPU draws (efb_w x efb_h, the render resolution); 0: no usable Vulkan (the reason is
 * logged), the software rasteriser draws. */
int gcn_vk_init(int efb_w, int efb_h);
int gcn_vk_active(void);
/* The GPU's name and frame buffer size, or why it is not used. */
const char *gcn_vk_status(void);
/* A new render resolution: the frame buffer is recreated, its contents scaled over. */
void gcn_vk_resize(int efb_w, int efb_h);

/* A decoded texture (RGBA8, r in the low byte) for the shader: returns its slot. Released
 * slots stay valid for what was drawn with them before. */
int gcn_vk_texture(const uint32_t *px, int w, int h);
void gcn_vk_texture_release(int slot);

/* A state for the triangles that follow: its index. */
uint32_t gcn_vk_state(const GcnVkState *state);
/* A triangle (counter-clockwise or not: culling is the caller's), scissor in frame buffer
 * pixels (inclusive). */
void gcn_vk_triangle(const GcnVkVertex v[3], const GcnVkPipe *pipe, const int scissor[4]);
/* The transform state for the gcn_vk_xf_triangles that follow: its index. */
uint32_t gcn_vk_xf_state(const GcnVkXf *xf);
/* Triangles of untransformed vertices: the GPU transforms, lights, generates texture coordinates,
 * clips and culls. gcn_vk_xf_begin gives room for n vertices (each carries its XF and pixel
 * state) in the GPU's buffer, and *base, the first one's index; gcn_vk_xf_end draws `count`
 * indices (absolute, at most 3 * n, a multiple of 3) into them. Nothing else in between. */
GcnVkXfVertex *gcn_vk_xf_begin(int n, uint32_t *base);
void gcn_vk_xf_end(const uint32_t *idx, int count, const GcnVkPipe *pipe, const int scissor[4]);
/* Clears a rectangle of the frame buffer (pixels, x1 / y1 exclusive): color RGBA8 (r low),
 * depth 24-bit. */
void gcn_vk_clear(int x0, int y0, int x1, int y1, uint32_t rgba, uint32_t z, int color, int depth);
/* Draws everything recorded so far and copies the rectangle (pixels, x1 / y1 exclusive) of the
 * frame buffer into color (RGBA8, r low) and depth (24-bit) at the same coordinates, rows
 * `stride` pixels apart; either may be NULL. Waits for the GPU. */
void gcn_vk_read(int x0, int y0, int x1, int y1, uint32_t *color, uint32_t *depth, int stride);
/* gcn_vk_read, `shrink` x smaller (the render resolution down to the console's): the rectangle
 * (frame buffer pixels, rounded to multiples of shrink) averaged down on the GPU (depth: the
 * nearest sample), written at (x0, y0) / shrink. What the game's copies into textures read. */
void gcn_vk_read_shrunk(int x0, int y0, int x1, int y1, int shrink, uint32_t *color, uint32_t *depth, int stride);
/* The color of the rectangle into dst, its top left at dst[0], opaque (the display copy). */
void gcn_vk_read_into(int x0, int y0, int x1, int y1, uint32_t *dst, int dst_stride);
/* Draws everything recorded so far (waits). */
void gcn_vk_sync(void);
/* A game (re)starts: what was batched and not drawn is dropped. Texture slots stay. */
void gcn_vk_reset(void);
/* Milliseconds the GPU took for the batches since the last call, and their count. */
double gcn_vk_take_gpu_ms(uint32_t *batches);
/* GCN_RASTER_STATS: what went to and came from the GPU over the last `frames` frames (stderr) */
void gcn_vk_print_stats(int frames);

#ifdef __cplusplus
}
#endif

#endif /* GCN_VK_H */
