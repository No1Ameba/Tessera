#include "glyph_atlas.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ATLAS_HASH_SIZE   4096  /* must be power of 2 */
#define ATLAS_MAX_SHELVES 128
#define ATLAS_PADDING     1     /* pixel gap between glyphs */

typedef struct {
    int cur_x;  /* next available x on this shelf */
    int y;      /* y of shelf top */
    int height; /* height of tallest glyph on this shelf */
} shelf_t;

typedef struct {
    uint32_t     codepoint;
    atlas_glyph_t glyph;
    int          used;
} hash_entry_t;

struct glyph_atlas {
    int      width, height;
    GLuint   texture;
    uint8_t *cpu_buf;

    shelf_t     shelves[ATLAS_MAX_SHELVES];
    int         shelf_count;

    hash_entry_t entries[ATLAS_HASH_SIZE];
    int          entry_count;
};

glyph_atlas_t *glyph_atlas_create(int width, int height)
{
    glyph_atlas_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->width   = width;
    a->height  = height;
    a->cpu_buf = calloc((size_t)width * height, 1);
    if (!a->cpu_buf) { free(a); return NULL; }

    glGenTextures(1, &a->texture);
    glBindTexture(GL_TEXTURE_2D, a->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, a->cpu_buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return a;
}

void glyph_atlas_destroy(glyph_atlas_t *a)
{
    if (!a) return;
    if (a->texture) glDeleteTextures(1, &a->texture);
    free(a->cpu_buf);
    free(a);
}

int glyph_atlas_lookup(const glyph_atlas_t *a, uint32_t cp, atlas_glyph_t *out)
{
    uint32_t h = cp & (ATLAS_HASH_SIZE - 1);
    for (int i = 0; i < ATLAS_HASH_SIZE; i++) {
        uint32_t idx = (h + (uint32_t)i) & (ATLAS_HASH_SIZE - 1);
        if (!a->entries[idx].used) return 0;
        if (a->entries[idx].codepoint == cp) {
            *out = a->entries[idx].glyph;
            return 1;
        }
    }
    return 0;
}

int glyph_atlas_insert(glyph_atlas_t *a, uint32_t cp,
                       const uint8_t *bitmap, int bw, int bh,
                       int bx, int by, int adv,
                       atlas_glyph_t *out)
{
    if (a->entry_count >= (ATLAS_HASH_SIZE * 3 / 4)) return -1;

    int gx = 0, gy = 0;

    if (bw > 0 && bh > 0) {
        /* Try to fit on an existing shelf (search newest first). */
        int placed = 0;
        for (int i = a->shelf_count - 1; i >= 0; i--) {
            shelf_t *s = &a->shelves[i];
            if (s->cur_x + bw + ATLAS_PADDING <= a->width &&
                s->y  + bh + ATLAS_PADDING <= a->height) {
                gx = s->cur_x;
                gy = s->y;
                s->cur_x += bw + ATLAS_PADDING;
                if (bh > s->height) s->height = bh;
                placed = 1;
                break;
            }
        }
        if (!placed) {
            /* Open a new shelf below the last one. */
            if (a->shelf_count >= ATLAS_MAX_SHELVES) return -1;
            int shelf_y = 0;
            if (a->shelf_count > 0) {
                shelf_t *last = &a->shelves[a->shelf_count - 1];
                shelf_y = last->y + last->height + ATLAS_PADDING;
            }
            if (shelf_y + bh + ATLAS_PADDING > a->height) return -1;
            shelf_t *s = &a->shelves[a->shelf_count++];
            s->y      = shelf_y;
            s->height = bh;
            s->cur_x  = bw + ATLAS_PADDING;
            gx = 0;
            gy = shelf_y;
        }

        /* Upload to GPU. */
        glBindTexture(GL_TEXTURE_2D, a->texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, gx, gy, bw, bh,
                        GL_RED, GL_UNSIGNED_BYTE, bitmap);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    *out = (atlas_glyph_t){
        .u         = (float)gx / (float)a->width,
        .v         = (float)gy / (float)a->height,
        .uw        = (float)bw / (float)a->width,
        .vh        = (float)bh / (float)a->height,
        .bearing_x = (int16_t)bx,
        .bearing_y = (int16_t)by,
        .advance_x = (int16_t)adv,
        .px_w      = (uint16_t)bw,
        .px_h      = (uint16_t)bh,
    };

    /* Insert into hash table with linear probing. */
    uint32_t h = cp & (ATLAS_HASH_SIZE - 1);
    for (int i = 0; i < ATLAS_HASH_SIZE; i++) {
        uint32_t idx = (h + (uint32_t)i) & (ATLAS_HASH_SIZE - 1);
        if (!a->entries[idx].used) {
            a->entries[idx].codepoint = cp;
            a->entries[idx].glyph     = *out;
            a->entries[idx].used      = 1;
            a->entry_count++;
            return 0;
        }
    }
    return -1; /* table full (shouldn't happen given 3/4 load check above) */
}

GLuint glyph_atlas_texture(const glyph_atlas_t *a) { return a->texture; }
