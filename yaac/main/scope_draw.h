#pragma once

/* Minimal drawing on the panel-order framebuffer, in landscape coordinates
 * (x 0..SCREEN_W-1 left to right, y 0..SCREEN_H-1 top to bottom). */
#include <stdint.h>
#include "display.h"

/* RGB565, byte-swapped for the panel */
#define RGB(r, g, b)    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define PANEL_COLOR(c)  ((uint16_t)(((c) >> 8) | ((c) << 8)))

void draw_bind(uint16_t *framebuffer);
void draw_clear(uint16_t color);
void draw_pixel(int x, int y, uint16_t color);
void draw_vline(int x, int y0, int y1, uint16_t color);     /* inclusive, any order */
void draw_hline_dotted(int x0, int x1, int y, int step, uint16_t color);
void draw_vline_dotted(int x, int y0, int y1, int step, uint16_t color);
void draw_fill_rect(int x, int y, int w, int h, uint16_t color);

/* 5x7 font, upper case + digits + a few symbols; returns x after the text */
int draw_text(int x, int y, const char *text, int scale, uint16_t color);
