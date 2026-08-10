#ifndef TESSERA_GL_RENDERER_H
#define TESSERA_GL_RENDERER_H

/*
 * OpenGL 3.3 Core cell renderer.
 *
 * Renders a terminal grid with two instanced draw passes per pane:
 *   1. Background quads  — solid-colour quad per cell
 *   2. Glyph quads       — alpha-blended textured quad per visible glyph
 *
 * Frame loop:
 *   gl_renderer_begin_frame() → gl_renderer_draw_cells() × N → (caller swaps buffers)
 */

#include <stdint.h>
#include <glad/glad.h>
#include "font.h"
#include "glyph_atlas.h"
#include "../cell.h"  /* term_cell_t, pane_rect_t, CELL_ATTR_* */

typedef struct gl_renderer gl_renderer_t;

/*
 * Create the renderer.  A valid OpenGL context must be current.
 * Compiles shaders and allocates GPU buffers.
 */
gl_renderer_t *gl_renderer_create(int viewport_w, int viewport_h,
                                   font_face_t *font, glyph_atlas_t *atlas);
void           gl_renderer_destroy(gl_renderer_t *r);
void           gl_renderer_resize(gl_renderer_t *r, int w, int h);

/* 폰트/아틀라스 교체 (hot reload). cell 메트릭 재계산. 이전 font/atlas 는
 * 호출자가 destroy 해야 한다. 렌더러는 포인터만 바꾼다. */
void           gl_renderer_set_font(gl_renderer_t *r,
                                     font_face_t *font, glyph_atlas_t *atlas);

/* Clear the screen.  Call once at the start of each frame. */
void gl_renderer_begin_frame(gl_renderer_t *r);

/* 배경 clear 색상을 설정한다 (0.0–1.0). 기본값: (0.1, 0.1, 0.1). */
void gl_renderer_set_clear_color(gl_renderer_t *r, float rf, float gf, float bf);

/* 배경 투명도를 설정한다 (0.0=투명, 1.0=불투명). 기본값: 1.0. */
void gl_renderer_set_opacity(gl_renderer_t *r, float opacity);

/*
 * Draw a pane's cell grid.
 * cells[row * cols + col] is the cell at (col, row).
 * Glyphs not yet in the atlas are rasterized on the fly.
 */
/*
 * sel_* = 선택 영역 (col/row 0-based). 모두 -1이면 선택 없음.
 * 선택된 셀은 fg/bg 반전 렌더링.
 */
void gl_renderer_draw_cells(gl_renderer_t *r,
                             const term_cell_t *cells, int cols, int rows,
                             pane_rect_t pane,
                             int sel_sc, int sel_sr,
                             int sel_ec, int sel_er);

/*
 * 커서를 렌더링한다. draw_cells() 이후에 호출.
 * cursor_style: CURSOR_STYLE_* (0=default/block, 1=block_blink, 2=block, ...)
 * visible: 블링크 토글 상태 (1=보임, 0=숨김)
 * cells: reverse-video 글리프 재그리기를 위해 현재 셀 버퍼 전달.
 * cols: cells 그리드의 한 행 폭.
 */
void gl_renderer_draw_cursor(gl_renderer_t *r,
                              const term_cell_t *cells, int cols,
                              int col, int row, int cursor_style, int visible,
                              float fg_r, float fg_g, float fg_b,
                              pane_rect_t pane);

#endif /* TESSERA_GL_RENDERER_H */
