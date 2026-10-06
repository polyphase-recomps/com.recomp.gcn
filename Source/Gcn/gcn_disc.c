/*
 * The game disc, from an image file or from an unpacked folder.
 *
 * An unpacked disc (Runtime/tools/gcn_disc.py unpack, done by a game package's build so
 * packaged games carry their data as plain files) is a list of segments: each file of
 * the disc, its boot block, executable and file table, with the disc offset it sits at
 * (disc.idx). Reads are served from those files, so the game sees the original disc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcn_disc.h"

typedef struct
{
    uint64_t offset;
    uint32_t size;
    char *path; /* absolute */
} Segment;

#define OPEN_FILES 8

struct GcnDisc
{
    FILE *image; /* an image file, or NULL for an unpacked disc */
    uint64_t size;
    Segment *seg;
    int count;
    struct
    {
        int seg;
        FILE *f;
        unsigned used;
    } open[OPEN_FILES];
    unsigned clock;
};

static int seek(FILE *f, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

static char *join(const char *dir, const char *rel)
{
    size_t a = strlen(dir), b = strlen(rel);
    char *p = (char *)malloc(a + b + 2);

    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, rel, b + 1);
    return p;
}

static int cmp_seg(const void *x, const void *y)
{
    const Segment *a = (const Segment *)x, *b = (const Segment *)y;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

int gcn_disc_is_unpacked(const char *path)
{
    char *idx = join(path, "disc.idx");
    FILE *f = fopen(idx, "rb");

    free(idx);
    if (!f) return 0;
    fclose(f);
    return 1;
}

static GcnDisc *open_unpacked(const char *dir)
{
    char *idx = join(dir, "disc.idx"), line[1024];
    FILE *f = fopen(idx, "rb");
    GcnDisc *d;
    int cap = 0;
    unsigned long long total = 0;

    free(idx);
    if (!f) return NULL;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "GCNDISC 1 %llu", &total) != 1)
    {
        fclose(f);
        return NULL;
    }
    d = (GcnDisc *)calloc(1, sizeof(*d));
    d->size = total;
    while (fgets(line, sizeof(line), f))
    {
        unsigned long long off;
        unsigned size;
        char *tab, *rel, *end;

        if (sscanf(line, "%llu\t%u", &off, &size) != 2) continue;
        tab = strchr(line, '\t');
        rel = tab ? strchr(tab + 1, '\t') : NULL;
        if (!rel) continue;
        rel++;
        for (end = rel + strlen(rel); end > rel && (end[-1] == '\n' || end[-1] == '\r'); end--) *(end - 1) = 0;
        if (d->count == cap)
        {
            cap = cap ? cap * 2 : 256;
            d->seg = (Segment *)realloc(d->seg, sizeof(Segment) * (size_t)cap);
        }
        d->seg[d->count].offset = off;
        d->seg[d->count].size = size;
        d->seg[d->count].path = join(dir, rel);
        d->count++;
    }
    fclose(f);
    qsort(d->seg, (size_t)d->count, sizeof(Segment), cmp_seg);
    return d;
}

GcnDisc *gcn_disc_open(const char *path)
{
    GcnDisc *d;
    FILE *f;

    if (gcn_disc_is_unpacked(path)) return open_unpacked(path);
    f = fopen(path, "rb");
    if (!f) return NULL;
    d = (GcnDisc *)calloc(1, sizeof(*d));
    d->image = f;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    d->size = (uint64_t)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    d->size = (uint64_t)ftello(f);
#endif
    return d;
}

void gcn_disc_close(GcnDisc *d)
{
    int i;

    if (!d) return;
    if (d->image) fclose(d->image);
    for (i = 0; i < OPEN_FILES; i++)
        if (d->open[i].f) fclose(d->open[i].f);
    for (i = 0; i < d->count; i++) free(d->seg[i].path);
    free(d->seg);
    free(d);
}

uint64_t gcn_disc_size(const GcnDisc *d) { return d ? d->size : 0; }

/* the segment's file, from the few kept open */
static FILE *segment_file(GcnDisc *d, int seg)
{
    int i, victim = 0;

    for (i = 0; i < OPEN_FILES; i++)
        if (d->open[i].f && d->open[i].seg == seg)
        {
            d->open[i].used = ++d->clock;
            return d->open[i].f;
        }
    for (i = 1; i < OPEN_FILES; i++)
        if (!d->open[i].f || (d->open[victim].f && d->open[i].used < d->open[victim].used)) victim = i;
    if (d->open[victim].f) fclose(d->open[victim].f);
    d->open[victim].f = fopen(d->seg[seg].path, "rb");
    d->open[victim].seg = seg;
    d->open[victim].used = ++d->clock;
    return d->open[victim].f;
}

uint32_t gcn_disc_read(GcnDisc *d, void *dst, uint64_t offset, uint32_t size)
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;

    if (!d) return 0;
    if (d->image)
    {
        if (!seek(d->image, offset)) return 0;
        return (uint32_t)fread(dst, 1, size, d->image);
    }
    if (offset >= d->size) return 0;
    if (offset + size > d->size) size = (uint32_t)(d->size - offset);
    while (done < size)
    {
        uint64_t pos = offset + done;
        int lo = 0, hi = d->count - 1, s = -1;
        uint32_t n;

        /* the last segment starting at or before pos */
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            if (d->seg[mid].offset <= pos)
            {
                s = mid;
                lo = mid + 1;
            }
            else
            {
                hi = mid - 1;
            }
        }
        if (s >= 0 && pos < d->seg[s].offset + d->seg[s].size)
        {
            FILE *f = segment_file(d, s);
            uint64_t in = pos - d->seg[s].offset;

            n = (uint32_t)(d->seg[s].offset + d->seg[s].size - pos);
            if (n > size - done) n = size - done;
            if (!f || !seek(f, in) || fread(out + done, 1, n, f) != n) memset(out + done, 0, n);
        }
        else
        {
            /* between files: zeros up to the next one */
            uint64_t next = s + 1 < d->count ? d->seg[s + 1].offset : d->size;
            n = (uint32_t)(next - pos < (uint64_t)(size - done) ? next - pos : (uint64_t)(size - done));
            if (n == 0) break;
            memset(out + done, 0, n);
        }
        done += n;
    }
    return done;
}
