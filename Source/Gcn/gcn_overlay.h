#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text line `line` (0..15, up from the bottom left) of the on-screen overlay; NULL or "" clears it. */
void gcn_overlay_set(int line, const char *text);
/* Draws the overlay onto a frame (RGBA8, r in the low byte; `stride` pixels per row). */
void gcn_overlay_draw(uint32_t *rgba, int stride, int w, int h);
/* ... on a frame drawn at `scale` x the console's resolution (the text keeps its size on screen) */
void gcn_overlay_draw_scaled(uint32_t *rgba, int stride, int w, int h, int scale);
/* One line of text at x, y (top left), 6 * scale pixels per character, over a darkened
 * band; color is RGBA8 like the frame. */
void gcn_overlay_text(uint32_t *rgba, int stride, int w, int h, int x, int y, int scale, uint32_t color, const char *text);

#ifdef __cplusplus
}
#endif
