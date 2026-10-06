#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes one THP video frame (file: its bytes, at most file_cap readable) into the Y, U
 * and V planes in GX I8 tile layout (each at least plane_cap bytes). Returns 0 or the
 * SDK's error code; *luma_bytes gets the size of the Y plane written (U and V: a quarter). */
int gcn_thp_decode(const uint8_t *file, size_t file_cap, uint8_t *tile_y, uint8_t *tile_u, uint8_t *tile_v,
                   size_t plane_cap, size_t *luma_bytes);

#ifdef __cplusplus
}
#endif
