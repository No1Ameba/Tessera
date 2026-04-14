#define _GNU_SOURCE
#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include <hb.h>
#include <hb-ft.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct font_face {
    FT_Library library;
    FT_Face    face;
    hb_font_t *hb_font;
    int        cell_width;
    int        cell_height;
    int        ascender;
};

font_face_t *font_face_load(const char *path, float size_pt, int dpi)
{
    font_face_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;

    if (FT_Init_FreeType(&f->library) != 0) {
        free(f); return NULL;
    }
    if (FT_New_Face(f->library, path, 0, &f->face) != 0) {
        FT_Done_FreeType(f->library); free(f); return NULL;
    }
    /* Size in 1/64 pts. Using 0 for width = same as height. */
    if (FT_Set_Char_Size(f->face, 0, (FT_F26Dot6)(size_pt * 64.0f), dpi, dpi) != 0) {
        FT_Done_Face(f->face);
        FT_Done_FreeType(f->library); free(f); return NULL;
    }

    /* Compute cell metrics from the face's size metrics (all in 26.6 fixed point). */
    f->ascender    = (int)(f->face->size->metrics.ascender >> 6);
    int descender  = (int)(-(f->face->size->metrics.descender >> 6));
    f->cell_height = f->ascender + descender;

    /* Cell width = advance of 'M' (monospace fonts have uniform advance). */
    FT_UInt gidx = FT_Get_Char_Index(f->face, 'M');
    FT_Fixed adv = 0;
    FT_Get_Advance(f->face, gidx, FT_LOAD_NO_HINTING, &adv);
    f->cell_width = (int)(adv >> 16); /* 16.16 fixed → integer pixels */
    if (f->cell_width <= 0)
        f->cell_width = (int)(f->face->size->metrics.max_advance >> 6);

    /* HarfBuzz font wrapping the FreeType face. */
    f->hb_font = hb_ft_font_create(f->face, NULL);
    if (!f->hb_font) {
        FT_Done_Face(f->face);
        FT_Done_FreeType(f->library); free(f); return NULL;
    }

    return f;
}

void font_face_destroy(font_face_t *f)
{
    if (!f) return;
    if (f->hb_font) hb_font_destroy(f->hb_font);
    if (f->face)    FT_Done_Face(f->face);
    if (f->library) FT_Done_FreeType(f->library);
    free(f);
}

int font_rasterize(font_face_t *f, uint32_t codepoint,
                   uint8_t **bitmap_out, glyph_metrics_t *metrics_out)
{
    FT_UInt gidx = FT_Get_Char_Index(f->face, codepoint);
    if (FT_Load_Glyph(f->face, gidx, FT_LOAD_RENDER) != 0)
        return -1;

    FT_GlyphSlot slot = f->face->glyph;
    FT_Bitmap   *bm   = &slot->bitmap;

    metrics_out->bearing_x = slot->bitmap_left;
    metrics_out->bearing_y = slot->bitmap_top;
    metrics_out->width     = bm->width;
    metrics_out->height    = bm->rows;
    metrics_out->advance_x = (int)(slot->advance.x >> 6);

    if (bm->width > 0 && bm->rows > 0) {
        /* Copy row-by-row to strip FreeType's row padding (pitch >= width).
         * pitch can be negative for bottom-up bitmaps; abs() handles both. */
        int pitch = bm->pitch < 0 ? -bm->pitch : bm->pitch;
        size_t sz = (size_t)bm->width * bm->rows;
        *bitmap_out = malloc(sz);
        if (!*bitmap_out) return -1;
        for (unsigned int row = 0; row < bm->rows; row++) {
            memcpy(*bitmap_out + row * bm->width,
                   bm->buffer  + row * pitch,
                   bm->width);
        }
    } else {
        *bitmap_out = NULL;
    }
    return 0;
}

void font_bitmap_free(uint8_t *bitmap) { free(bitmap); }

int font_shape(font_face_t *f, const char *utf8, int byte_len,
               shaped_glyph_t *out, int max)
{
    if (!f || !utf8 || max <= 0) return -1;
    if (byte_len < 0) byte_len = (int)strlen(utf8);

    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));
    hb_buffer_add_utf8(buf, utf8, byte_len, 0, -1);
    hb_shape(f->hb_font, buf, NULL, 0);

    unsigned int n = 0;
    hb_glyph_info_t     *info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *pos  = hb_buffer_get_glyph_positions(buf, &n);

    int count = (int)n < max ? (int)n : max;
    for (int i = 0; i < count; i++) {
        out[i].codepoint = info[i].codepoint;
        out[i].x_offset  = pos[i].x_offset  >> 6;
        out[i].y_offset  = pos[i].y_offset  >> 6;
        out[i].x_advance = pos[i].x_advance >> 6;
    }

    hb_buffer_destroy(buf);
    return count;
}

int font_cell_width(font_face_t *f)  { return f->cell_width; }
int font_cell_height(font_face_t *f) { return f->cell_height; }
int font_ascender(font_face_t *f)    { return f->ascender; }
