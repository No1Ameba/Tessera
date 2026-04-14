#ifndef TERMEMU_GLYPH_ATLAS_H
#define TERMEMU_GLYPH_ATLAS_H

/*
 * Glyph atlas — shelf-based bin packer on a single GL_R8 texture.
 *
 * Workflow:
 *   1. glyph_atlas_create()  — allocate CPU buffer + OpenGL texture
 *   2. glyph_atlas_lookup()  — check if a codepoint is already cached
 *   3. glyph_atlas_insert()  — pack one glyph bitmap into the atlas
 *   4. glyph_atlas_texture() — return the GLuint for rendering
 *   5. glyph_atlas_destroy() — free all resources
 *
 * The atlas texture is single-channel (GL_R8). Fragment shaders read the red
 * channel as the glyph's alpha mask.
 * UV coordinates returned are normalized 0-1.
 */

#include <stdint.h>
#include <glad/glad.h>

/* Cached glyph descriptor returned by lookup / insert. */
typedef struct {
    float    u, v;         /* top-left UV in atlas (0-1) */
    float    uw, vh;       /* size in UV space */
    int16_t  bearing_x;   /* pixels from pen to left edge */
    int16_t  bearing_y;   /* pixels from baseline to top edge */
    int16_t  advance_x;   /* horizontal advance in pixels */
    uint16_t px_w, px_h;  /* bitmap pixel dimensions */
} atlas_glyph_t;

typedef struct glyph_atlas glyph_atlas_t;

glyph_atlas_t *glyph_atlas_create(int width, int height);
void           glyph_atlas_destroy(glyph_atlas_t *atlas);

/*
 * Look up a codepoint in the atlas.
 * @return 1 found (out filled), 0 not found.
 */
int glyph_atlas_lookup(const glyph_atlas_t *atlas, uint32_t codepoint,
                       atlas_glyph_t *out);

/*
 * Insert a glyph bitmap into the atlas.
 * bitmap: 8-bit grayscale, bw × bh bytes (may be NULL for whitespace glyphs).
 * @return 0 success, -1 atlas full.
 */
int glyph_atlas_insert(glyph_atlas_t *atlas, uint32_t codepoint,
                       const uint8_t *bitmap, int bw, int bh,
                       int bearing_x, int bearing_y, int advance_x,
                       atlas_glyph_t *out);

GLuint glyph_atlas_texture(const glyph_atlas_t *atlas);

#endif /* TERMEMU_GLYPH_ATLAS_H */
