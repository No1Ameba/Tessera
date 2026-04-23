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

#define FONT_MAX_FALLBACKS 4

/* One FreeType face + its HarfBuzz wrapper. Used both for the primary face
 * and for any fallback faces attached via font_face_add_fallback(). */
typedef struct {
    FT_Face    face;
    hb_font_t *hb_font;
} face_entry_t;

struct font_face {
    FT_Library   library;       /* shared across primary + fallback faces */
    float        size_pt;
    int          dpi;
    face_entry_t primary;
    face_entry_t fallbacks[FONT_MAX_FALLBACKS];
    int          fallback_count;
    int          cell_width;    /* derived from primary */
    int          cell_height;   /* derived from primary */
    int          ascender;      /* derived from primary */
};

/* Apply the face's char size (pt/dpi, 1/64 units) and build an hb_font. */
static int face_entry_init(face_entry_t *e, FT_Face ft_face,
                            float size_pt, int dpi)
{
    if (FT_Set_Char_Size(ft_face, 0, (FT_F26Dot6)(size_pt * 64.0f),
                          dpi, dpi) != 0) {
        FT_Done_Face(ft_face);
        return -1;
    }
    e->face    = ft_face;
    e->hb_font = hb_ft_font_create(ft_face, NULL);
    if (!e->hb_font) {
        FT_Done_Face(ft_face);
        e->face = NULL;
        return -1;
    }
    return 0;
}

static void face_entry_destroy(face_entry_t *e)
{
    if (!e) return;
    if (e->hb_font) { hb_font_destroy(e->hb_font); e->hb_font = NULL; }
    if (e->face)    { FT_Done_Face(e->face);       e->face    = NULL; }
}

font_face_t *font_face_load(const char *path, float size_pt, int dpi)
{
    font_face_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;

    if (FT_Init_FreeType(&f->library) != 0) {
        free(f); return NULL;
    }

    FT_Face ft_face = NULL;
    if (FT_New_Face(f->library, path, 0, &ft_face) != 0) {
        FT_Done_FreeType(f->library); free(f); return NULL;
    }
    if (face_entry_init(&f->primary, ft_face, size_pt, dpi) != 0) {
        FT_Done_FreeType(f->library); free(f); return NULL;
    }

    f->size_pt = size_pt;
    f->dpi     = dpi;

    /* Compute cell metrics from the primary face (all in 26.6 fixed point). */
    FT_Face pf = f->primary.face;
    f->ascender    = (int)(pf->size->metrics.ascender >> 6);
    int descender  = (int)(-(pf->size->metrics.descender >> 6));
    f->cell_height = f->ascender + descender;

    /* Cell width = advance of 'M' (monospace fonts have uniform advance). */
    FT_UInt gidx = FT_Get_Char_Index(pf, 'M');
    FT_Fixed adv = 0;
    FT_Get_Advance(pf, gidx, FT_LOAD_NO_HINTING, &adv);
    f->cell_width = (int)(adv >> 16); /* 16.16 fixed → integer pixels */
    if (f->cell_width <= 0)
        f->cell_width = (int)(pf->size->metrics.max_advance >> 6);

    return f;
}

void font_face_destroy(font_face_t *f)
{
    if (!f) return;
    face_entry_destroy(&f->primary);
    for (int i = 0; i < f->fallback_count; i++)
        face_entry_destroy(&f->fallbacks[i]);
    if (f->library) FT_Done_FreeType(f->library);
    free(f);
}

int font_face_add_fallback(font_face_t *f, const char *path)
{
    if (!f || !path || !path[0]) return -1;
    if (f->fallback_count >= FONT_MAX_FALLBACKS) return -1;

    /* For .ttc/.otc files, pick the face index that covers Hangul Syllables.
     * For regular .ttf/.otf, only index 0 is valid (num_faces == 1). */
    FT_Face probe = NULL;
    if (FT_New_Face(f->library, path, 0, &probe) != 0) return -1;
    long   num_faces = probe->num_faces;
    FT_Long chosen   = 0;
    if (FT_Get_Char_Index(probe, 0xAC00) == 0 && num_faces > 1) {
        FT_Done_Face(probe); probe = NULL;
        for (FT_Long idx = 1; idx < num_faces; idx++) {
            FT_Face cand = NULL;
            if (FT_New_Face(f->library, path, idx, &cand) != 0) continue;
            if (FT_Get_Char_Index(cand, 0xAC00) != 0) {
                chosen = idx;
                FT_Done_Face(cand);
                break;
            }
            FT_Done_Face(cand);
        }
    }
    if (probe) { FT_Done_Face(probe); probe = NULL; }

    FT_Face ft_face = NULL;
    if (FT_New_Face(f->library, path, chosen, &ft_face) != 0) return -1;

    face_entry_t *slot = &f->fallbacks[f->fallback_count];
    if (face_entry_init(slot, ft_face, f->size_pt, f->dpi) != 0)
        return -1;

    f->fallback_count++;
    return 0;
}

/* Locate the face that contains the requested codepoint.
 * Returns the primary face as a last resort (so .notdef rasterization still
 * produces consistent cell metrics). */
static face_entry_t *resolve_face_for_codepoint(font_face_t *f, uint32_t cp)
{
    if (FT_Get_Char_Index(f->primary.face, cp) != 0)
        return &f->primary;
    for (int i = 0; i < f->fallback_count; i++) {
        if (FT_Get_Char_Index(f->fallbacks[i].face, cp) != 0)
            return &f->fallbacks[i];
    }
    return &f->primary;
}

int font_rasterize(font_face_t *f, uint32_t codepoint,
                   uint8_t **bitmap_out, glyph_metrics_t *metrics_out)
{
    if (!f) return -1;
    face_entry_t *e    = resolve_face_for_codepoint(f, codepoint);
    FT_Face       face = e->face;

    FT_UInt gidx = FT_Get_Char_Index(face, codepoint);
    if (FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) != 0)
        return -1;

    FT_GlyphSlot slot = face->glyph;
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
    hb_shape(f->primary.hb_font, buf, NULL, 0);

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
