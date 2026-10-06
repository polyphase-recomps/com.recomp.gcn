/*
 * Software rasteriser of the GameCube GPU emulation (gcn_gpu.c): vertex transform and
 * lighting (XF), texture coordinate generation, rasterisation, texturing, the TEV and the
 * pixel engine, into the embedded frame buffer.
 */
#ifndef GCN_RASTER_H
#define GCN_RASTER_H

#include <stdint.h>

/* register files and memories, owned by gcn_gpu.c */
typedef struct GcnGpuRegs
{
    uint32_t *bp;      /* 256 */
    uint32_t *cp;      /* 256 */
    uint32_t *xf;      /* 0x1100 words: memory 0x000-0x7FF, registers 0x1000- */
    uint8_t *mem;      /* guest memory buffer (GCNW_OFFSET addressing) */
    uint8_t *tmem;     /* 1 MB texture memory (TLUTs, preloaded textures) */
    uint32_t *efb;     /* RGBA8, GCN_EFB_W x GCN_EFB_H (byte order r,g,b,a in memory) */
    uint32_t *depth;   /* 24-bit */
} GcnGpuRegs;

/* A vertex as it comes out of the vertex loader (before XF). */
typedef struct GcnVertexIn
{
    float pos[3];
    float nrm[3], bin[3], tan[3];
    float col[2][4];           /* 0..1 */
    float tex[8][2];
    int pnmtx;                 /* position matrix address (rows), -1 = from MATINDEX_A */
    int texmtx[8];             /* -1 = from MATINDEX */
    uint8_t has_nrm, has_col[2], has_tex[8];
} GcnVertexIn;

void gcn_raster_init(const GcnGpuRegs *regs);
/* GX primitive (0x80 quads ... 0xB8 points) with its loaded vertices. */
void gcn_raster_primitive(int prim, const GcnVertexIn *v, int count);
/* Textures may have changed (GXInvalidateTexAll, a copy into texture memory). */
void gcn_raster_invalidate_textures(void);
/* Draws the queued triangles (before anything reads or clears the EFB). */
void gcn_raster_flush(void);
/* A display copy finished a frame (statistics, tracing). */
void gcn_raster_frame_done(void);

#endif /* GCN_RASTER_H */
