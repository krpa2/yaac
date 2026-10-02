#pragma once

/* AXS15231B 3.49" panel (QSPI) used in landscape. The panel is natively 172x640
 * portrait; the framebuffer is kept in panel order and drawing code maps
 * landscape (x, y) to it (see scope_draw.c), so frames go out without a
 * rotation pass. */
#include <stdint.h>

#define PANEL_W         172             /* native panel width (portrait) */
#define PANEL_H         640
#define SCREEN_W        PANEL_H         /* landscape */
#define SCREEN_H        PANEL_W

/* Framebuffer in PSRAM, PANEL_W x PANEL_H, byte-swapped RGB565 */
uint16_t *display_init(void);

/* Send the whole framebuffer to the panel (blocks until done) */
void display_present(void);

/* Backlight brightness 0..255 */
void display_set_backlight(uint8_t level);
