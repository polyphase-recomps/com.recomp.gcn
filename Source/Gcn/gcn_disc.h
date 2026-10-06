#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game disc: an image file (.iso / .gcm / .nkit.iso), or a folder that
 * Runtime/tools/gcn_disc.py unpack wrote (disc.idx + the disc's files). */
typedef struct GcnDisc GcnDisc;

/* NULL if `path` is neither an image nor an unpacked disc. */
GcnDisc *gcn_disc_open(const char *path);
void gcn_disc_close(GcnDisc *disc);
/* Bytes read (gaps between files read as zeros); fewer only past the end. */
uint32_t gcn_disc_read(GcnDisc *disc, void *dst, uint64_t offset, uint32_t size);
uint64_t gcn_disc_size(const GcnDisc *disc);
/* 1 if `path` holds an unpacked disc. */
int gcn_disc_is_unpacked(const char *path);

#ifdef __cplusplus
}
#endif
