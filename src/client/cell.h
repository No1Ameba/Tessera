#ifndef TESSERA_CELL_H
#define TESSERA_CELL_H

/*
 * Terminal cell — shared between screen buffer and GL renderer.
 * Keeping this in its own header avoids pulling OpenGL headers into
 * screen.c which has no GPU dependency.
 */

#include <stdint.h>

/* SGR attribute flags */
#define CELL_ATTR_BOLD      (1 << 0)
#define CELL_ATTR_ITALIC    (1 << 1)
#define CELL_ATTR_UNDERLINE (1 << 2)
#define CELL_ATTR_BLINK     (1 << 3)
#define CELL_ATTR_REVERSE   (1 << 4)
/* Wide character flags (CJK / emoji double-width) */
#define CELL_ATTR_WIDE      (1 << 5)   /* double-width 선두 셀 */
#define CELL_ATTR_WIDE_CONT (1 << 6)   /* double-width continuation 셀 */

/* One terminal cell. */
typedef struct {
    uint32_t codepoint;       /* Unicode codepoint (0 = empty / space) */
    uint8_t  fg_r, fg_g, fg_b;
    uint8_t  bg_r, bg_g, bg_b;
    uint8_t  attrs;           /* CELL_ATTR_* bitmask */
    uint16_t link_id;         /* OSC 8 하이퍼링크 id (0 = 링크 없음) */
} term_cell_t;

/* Pixel rectangle used by both layout and renderer. */
typedef struct { int x, y, w, h; } pane_rect_t;

/* Default colour values (dark terminal theme). */
#define CELL_DEFAULT_FG_R 220
#define CELL_DEFAULT_FG_G 220
#define CELL_DEFAULT_FG_B 220
#define CELL_DEFAULT_BG_R  20
#define CELL_DEFAULT_BG_G  20
#define CELL_DEFAULT_BG_B  20

#endif /* TESSERA_CELL_H */
