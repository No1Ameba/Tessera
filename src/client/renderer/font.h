#ifndef TERMEMU_FONT_H
#define TERMEMU_FONT_H

/*
 * Font rasterization — FreeType 2 + HarfBuzz
 *
 * Workflow:
 *   1. font_face_load()    — open .ttf/.otf, set pixel size
 *   2. font_rasterize()    — render one codepoint to 8-bit grayscale bitmap
 *   3. font_shape()        — HarfBuzz ligature shaping for a UTF-8 run
 *   4. font_face_destroy() — release all resources
 */

#include <stdint.h>
#include <stddef.h>

typedef struct font_face font_face_t;

/* Metrics of one rasterized glyph. */
typedef struct {
    int32_t  bearing_x;  /* pixels from pen to left edge of bitmap */
    int32_t  bearing_y;  /* pixels from baseline to top edge (ascender) */
    uint32_t width;      /* bitmap width in pixels */
    uint32_t height;     /* bitmap height in pixels */
    int32_t  advance_x;  /* horizontal advance in pixels */
} glyph_metrics_t;

/* One shaped glyph (output of font_shape). */
typedef struct {
    uint32_t codepoint;  /* FreeType glyph index (not Unicode) */
    int32_t  x_offset;   /* sub-pixel x offset in pixels */
    int32_t  y_offset;   /* sub-pixel y offset in pixels */
    int32_t  x_advance;  /* advance in pixels */
} shaped_glyph_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * Load a font face.
 * @param path     path to .ttf / .otf file
 * @param size_pt  point size (e.g. 12.0f)
 * @param dpi      screen DPI (e.g. 96)
 * @return opaque handle, NULL on failure
 */
font_face_t *font_face_load(const char *path, float size_pt, int dpi);
void         font_face_destroy(font_face_t *face);

/*
 * Rasterize one Unicode codepoint.
 * *bitmap_out receives a malloc'd 8-bit grayscale buffer (width × height bytes).
 * May be NULL if the glyph has no bitmap (e.g. space). Caller frees with
 * font_bitmap_free().
 * @return 0 success, -1 error
 */
int  font_rasterize(font_face_t *face, uint32_t codepoint,
                    uint8_t **bitmap_out, glyph_metrics_t *metrics_out);
void font_bitmap_free(uint8_t *bitmap);

/*
 * HarfBuzz shaping for a UTF-8 string.
 * @param byte_len  byte length (-1 = strlen)
 * @param out       caller-allocated output array
 * @param max       capacity of out[]
 * @return number of shaped glyphs, -1 on error
 */
int font_shape(font_face_t *face, const char *utf8, int byte_len,
               shaped_glyph_t *out, int max);

/* Advance width of 'M' in pixels (monospace cell width). */
int font_cell_width(font_face_t *face);

/* ascender + |descender| in pixels (cell height). */
int font_cell_height(font_face_t *face);

/* Distance from cell top to text baseline in pixels. */
int font_ascender(font_face_t *face);

#endif /* TERMEMU_FONT_H */
