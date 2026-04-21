#include "gl_renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>  /* offsetof */

/* ── Instance data layouts ─────────────────────────────────────────────── */

/* Per-instance data for the background pass (20 bytes). */
typedef struct {
    float col, row;   /* grid position */
    float r, g, b;    /* background colour */
} bg_instance_t;

/* Per-instance data for the text pass (52 bytes). */
typedef struct {
    float col, row;                            /* grid position */
    float bearing_x, bearing_y;               /* glyph bearing in pixels */
    float atlas_x, atlas_y, atlas_w, atlas_h; /* normalized atlas UV */
    float glyph_w, glyph_h;                   /* glyph bitmap size in pixels */
    float r, g, b;                             /* foreground colour */
} text_instance_t;

#define MAX_INSTANCES 8192

/* ── Embedded GLSL shaders ─────────────────────────────────────────────── */

static const char *BG_VERT =
    "#version 330 core\n"
    "layout(location=0) in vec2 v_pos;\n"
    "layout(location=1) in vec2 i_cell;\n"
    "layout(location=2) in vec3 i_bg;\n"
    "uniform vec2 u_cell_size;\n"
    "uniform vec2 u_quad_size;\n"
    "uniform vec2 u_viewport;\n"
    "uniform vec2 u_pane_offset;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    "  vec2 qs = (u_quad_size.x > 0.0) ? u_quad_size : u_cell_size;\n"
    "  vec2 pix = u_pane_offset + i_cell * u_cell_size + v_pos * qs;\n"
    "  vec2 ndc = (pix / u_viewport) * 2.0 - 1.0;\n"
    "  ndc.y = -ndc.y;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_color = i_bg;\n"
    "}\n";

static const char *BG_FRAG =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "out vec4 frag_color;\n"
    "uniform float u_opacity;\n"
    "void main() { frag_color = vec4(v_color, u_opacity); }\n";

static const char *TEXT_VERT =
    "#version 330 core\n"
    "layout(location=0) in vec2 v_pos;\n"
    "layout(location=1) in vec2 i_cell;\n"
    "layout(location=2) in vec2 i_bearing;\n"
    "layout(location=3) in vec4 i_atlas_uv;\n"
    "layout(location=4) in vec2 i_glyph_sz;\n"
    "layout(location=5) in vec3 i_fg;\n"
    "uniform vec2  u_cell_size;\n"
    "uniform vec2  u_viewport;\n"
    "uniform vec2  u_pane_offset;\n"
    "uniform float u_ascender;\n"
    "out vec2 v_uv;\n"
    "out vec3 v_fg;\n"
    "void main() {\n"
    "  float cx = u_pane_offset.x + i_cell.x * u_cell_size.x;\n"
    "  float cy = u_pane_offset.y + i_cell.y * u_cell_size.y;\n"
    "  /* glyph top-left: bearing_y pixels above baseline, baseline at ascender */\n"
    "  float gx = cx + i_bearing.x;\n"
    "  float gy = cy + u_ascender - i_bearing.y;\n"
    "  vec2 pix = vec2(gx, gy) + v_pos * i_glyph_sz;\n"
    "  vec2 ndc = (pix / u_viewport) * 2.0 - 1.0;\n"
    "  ndc.y = -ndc.y;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = i_atlas_uv.xy + v_pos * i_atlas_uv.zw;\n"
    "  v_fg = i_fg;\n"
    "}\n";

static const char *TEXT_FRAG =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec3 v_fg;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main() {\n"
    "  float a = texture(u_atlas, v_uv).r;\n"
    "  frag_color = vec4(v_fg, a);\n"
    "}\n";

/* ── Shader helpers ────────────────────────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gl_renderer: shader compile error:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "gl_renderer: program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* ── Renderer struct ───────────────────────────────────────────────────── */

struct gl_renderer {
    int viewport_w, viewport_h;
    int cell_w, cell_h, ascender;
    float clear_r, clear_g, clear_b, clear_a;  /* 배경 clear 색상 + 투명도 */

    font_face_t   *font;
    glyph_atlas_t *atlas;

    /* Background pass */
    GLuint bg_prog;
    GLuint bg_vao, bg_quad_vbo, bg_inst_vbo;

    /* Text pass */
    GLuint text_prog;
    GLuint text_vao, text_quad_vbo, text_inst_vbo;

    /* Scratch instance buffers (on the heap to avoid large stack allocs) */
    bg_instance_t   *bg_buf;
    text_instance_t *text_buf;
};

/* Unit quad: 6 vertices (2 triangles), each vertex is (x, y) in [0,1]. */
static const float QUAD_VERTS[12] = {
    0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,
    1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
};

static void setup_bg_vao(gl_renderer_t *r)
{
    glGenVertexArrays(1, &r->bg_vao);
    glBindVertexArray(r->bg_vao);

    glGenBuffers(1, &r->bg_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->bg_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof QUAD_VERTS, QUAD_VERTS, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    glGenBuffers(1, &r->bg_inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->bg_inst_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(MAX_INSTANCES * (int)sizeof(bg_instance_t)),
                 NULL, GL_DYNAMIC_DRAW);

    /* location 1: i_cell (col, row) */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(bg_instance_t),
                          (void*)offsetof(bg_instance_t, col));
    glVertexAttribDivisor(1, 1);

    /* location 2: i_bg (r, g, b) */
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(bg_instance_t),
                          (void*)offsetof(bg_instance_t, r));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
}

static void setup_text_vao(gl_renderer_t *r)
{
    glGenVertexArrays(1, &r->text_vao);
    glBindVertexArray(r->text_vao);

    glGenBuffers(1, &r->text_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->text_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof QUAD_VERTS, QUAD_VERTS, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    glGenBuffers(1, &r->text_inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->text_inst_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(MAX_INSTANCES * (int)sizeof(text_instance_t)),
                 NULL, GL_DYNAMIC_DRAW);

    GLsizei stride = (GLsizei)sizeof(text_instance_t);

    glEnableVertexAttribArray(1); /* i_cell */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(text_instance_t, col));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2); /* i_bearing */
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(text_instance_t, bearing_x));
    glVertexAttribDivisor(2, 1);

    glEnableVertexAttribArray(3); /* i_atlas_uv */
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(text_instance_t, atlas_x));
    glVertexAttribDivisor(3, 1);

    glEnableVertexAttribArray(4); /* i_glyph_sz */
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(text_instance_t, glyph_w));
    glVertexAttribDivisor(4, 1);

    glEnableVertexAttribArray(5); /* i_fg */
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(text_instance_t, r));
    glVertexAttribDivisor(5, 1);

    glBindVertexArray(0);
}

gl_renderer_t *gl_renderer_create(int vw, int vh,
                                   font_face_t *font, glyph_atlas_t *atlas)
{
    gl_renderer_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;

    r->viewport_w = vw;
    r->viewport_h = vh;
    r->font       = font;
    r->atlas      = atlas;
    r->cell_w     = font_cell_width(font);
    r->cell_h     = font_cell_height(font);
    r->ascender   = font_ascender(font);
    r->clear_r    = 0.078f;  /* 기본 배경 #141414 */
    r->clear_g    = 0.078f;
    r->clear_b    = 0.078f;
    r->clear_a    = 1.0f;

    r->bg_buf   = malloc(MAX_INSTANCES * sizeof(bg_instance_t));
    r->text_buf = malloc(MAX_INSTANCES * sizeof(text_instance_t));
    if (!r->bg_buf || !r->text_buf) { gl_renderer_destroy(r); return NULL; }

    r->bg_prog   = link_program(BG_VERT,   BG_FRAG);
    r->text_prog = link_program(TEXT_VERT, TEXT_FRAG);
    if (!r->bg_prog || !r->text_prog) { gl_renderer_destroy(r); return NULL; }

    setup_bg_vao(r);
    setup_text_vao(r);

    glViewport(0, 0, vw, vh);
    return r;
}

void gl_renderer_set_font(gl_renderer_t *r, font_face_t *font, glyph_atlas_t *atlas)
{
    if (!r || !font || !atlas) return;
    r->font     = font;
    r->atlas    = atlas;
    r->cell_w   = font_cell_width(font);
    r->cell_h   = font_cell_height(font);
    r->ascender = font_ascender(font);
}

void gl_renderer_destroy(gl_renderer_t *r)
{
    if (!r) return;
    if (r->bg_prog)       glDeleteProgram(r->bg_prog);
    if (r->text_prog)     glDeleteProgram(r->text_prog);
    if (r->bg_vao)        glDeleteVertexArrays(1, &r->bg_vao);
    if (r->text_vao)      glDeleteVertexArrays(1, &r->text_vao);
    if (r->bg_quad_vbo)   glDeleteBuffers(1, &r->bg_quad_vbo);
    if (r->bg_inst_vbo)   glDeleteBuffers(1, &r->bg_inst_vbo);
    if (r->text_quad_vbo) glDeleteBuffers(1, &r->text_quad_vbo);
    if (r->text_inst_vbo) glDeleteBuffers(1, &r->text_inst_vbo);
    free(r->bg_buf);
    free(r->text_buf);
    free(r);
}

void gl_renderer_resize(gl_renderer_t *r, int w, int h)
{
    r->viewport_w = w;
    r->viewport_h = h;
    glViewport(0, 0, w, h);
}

void gl_renderer_begin_frame(gl_renderer_t *r)
{
    glClearColor(r->clear_r, r->clear_g, r->clear_b, r->clear_a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gl_renderer_set_clear_color(gl_renderer_t *r, float rf, float gf, float bf)
{
    if (!r) return;
    r->clear_r = rf;
    r->clear_g = gf;
    r->clear_b = bf;
}

void gl_renderer_set_opacity(gl_renderer_t *r, float opacity)
{
    if (!r) return;
    r->clear_a = opacity;
}

/* Get or rasterize a glyph, inserting it into the atlas. */
static int ensure_glyph(gl_renderer_t *r, uint32_t cp, atlas_glyph_t *out)
{
    if (glyph_atlas_lookup(r->atlas, cp, out)) return 0;

    uint8_t       *bitmap  = NULL;
    glyph_metrics_t metrics = {0};
    if (font_rasterize(r->font, cp, &bitmap, &metrics) != 0) return -1;

    int ret = glyph_atlas_insert(r->atlas, cp,
                                  bitmap,
                                  (int)metrics.width, (int)metrics.height,
                                  metrics.bearing_x, metrics.bearing_y,
                                  metrics.advance_x,
                                  out);
    font_bitmap_free(bitmap);
    return ret;
}

/* 셀 (col,row)가 선택 범위 안에 있는지 판정.
 * 좌표는 정규화된 상태 (r0 <= r1). */
static int cell_in_selection(int col, int row,
                              int sc, int sr, int ec, int er)
{
    if (sr < 0) return 0;
    /* 정규화: r0 <= r1 */
    int r0, c0, r1, c1;
    if (sr < er || (sr == er && sc <= ec)) {
        r0 = sr; c0 = sc; r1 = er; c1 = ec;
    } else {
        r0 = er; c0 = ec; r1 = sr; c1 = sc;
    }
    if (row < r0 || row > r1) return 0;
    if (row > r0 && row < r1) return 1;          /* 중간 행 전체 */
    if (r0 == r1) return (col >= c0 && col <= c1); /* 단일 행 */
    if (row == r0) return (col >= c0);             /* 첫 행 */
    return (col <= c1);                            /* 마지막 행 */
}

void gl_renderer_draw_cells(gl_renderer_t *r,
                             const term_cell_t *cells, int cols, int rows,
                             pane_rect_t pane,
                             int sel_sc, int sel_sr,
                             int sel_ec, int sel_er)
{
    int bg_count   = 0;
    int text_count = 0;

    for (int row = 0; row < rows && bg_count < MAX_INSTANCES; row++) {
        for (int col = 0; col < cols && bg_count < MAX_INSTANCES; col++) {
            const term_cell_t *c = &cells[row * cols + col];
            uint8_t fg_r = c->fg_r, fg_g = c->fg_g, fg_b = c->fg_b;
            uint8_t bg_r = c->bg_r, bg_g = c->bg_g, bg_b = c->bg_b;

            /* Reverse video: swap fg/bg. */
            int rev = (c->attrs & CELL_ATTR_REVERSE) ||
                      cell_in_selection(col, row, sel_sc, sel_sr, sel_ec, sel_er);
            if (rev) {
                uint8_t tmp_r = fg_r, tmp_g = fg_g, tmp_b = fg_b;
                fg_r = bg_r; fg_g = bg_g; fg_b = bg_b;
                bg_r = tmp_r; bg_g = tmp_g; bg_b = tmp_b;
            }

            bg_instance_t *bi = &r->bg_buf[bg_count++];
            bi->col = (float)col;
            bi->row = (float)row;
            bi->r   = bg_r / 255.0f;
            bi->g   = bg_g / 255.0f;
            bi->b   = bg_b / 255.0f;

            /* WIDE_CONT 셀은 텍스트 패스 스킵 (선두 셀이 2칸 글리프를 그림) */
            if (c->attrs & CELL_ATTR_WIDE_CONT) continue;

            if (c->codepoint && c->codepoint != ' ' &&
                text_count < MAX_INSTANCES) {
                atlas_glyph_t ag = {0};
                if (ensure_glyph(r, c->codepoint, &ag) == 0 && ag.px_w > 0) {
                    text_instance_t *ti = &r->text_buf[text_count++];
                    ti->col       = (float)col;
                    ti->row       = (float)row;
                    ti->bearing_x = (float)ag.bearing_x;
                    ti->bearing_y = (float)ag.bearing_y;
                    ti->atlas_x   = ag.u;
                    ti->atlas_y   = ag.v;
                    ti->atlas_w   = ag.uw;
                    ti->atlas_h   = ag.vh;
                    ti->glyph_w   = (float)ag.px_w;
                    ti->glyph_h   = (float)ag.px_h;
                    ti->r         = fg_r / 255.0f;
                    ti->g         = fg_g / 255.0f;
                    ti->b         = fg_b / 255.0f;
                }
            }
        }
    }

    float vw = (float)r->viewport_w;
    float vh = (float)r->viewport_h;
    float cw = (float)r->cell_w;
    float ch = (float)r->cell_h;
    float ox = (float)pane.x;
    float oy = (float)pane.y;

    /* ── Background pass ────────────────────────────────────────────────── */
    if (bg_count > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, r->bg_inst_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)(bg_count * (int)sizeof(bg_instance_t)),
                        r->bg_buf);

        if (r->clear_a < 1.0f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        glUseProgram(r->bg_prog);
        glUniform2f(glGetUniformLocation(r->bg_prog, "u_cell_size"),   cw, ch);
        glUniform2f(glGetUniformLocation(r->bg_prog, "u_quad_size"),   0.0f, 0.0f);
        glUniform2f(glGetUniformLocation(r->bg_prog, "u_viewport"),    vw, vh);
        glUniform2f(glGetUniformLocation(r->bg_prog, "u_pane_offset"), ox, oy);
        glUniform1f(glGetUniformLocation(r->bg_prog, "u_opacity"),     r->clear_a);

        glBindVertexArray(r->bg_vao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, bg_count);
        glBindVertexArray(0);

        if (r->clear_a < 1.0f)
            glDisable(GL_BLEND);
    }

    /* ── Text pass ──────────────────────────────────────────────────────── */
    if (text_count > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, r->text_inst_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)(text_count * (int)sizeof(text_instance_t)),
                        r->text_buf);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(r->text_prog);
        glUniform2f(glGetUniformLocation(r->text_prog, "u_cell_size"),   cw, ch);
        glUniform2f(glGetUniformLocation(r->text_prog, "u_viewport"),    vw, vh);
        glUniform2f(glGetUniformLocation(r->text_prog, "u_pane_offset"), ox, oy);
        glUniform1f(glGetUniformLocation(r->text_prog, "u_ascender"), (float)r->ascender);
        glUniform1i(glGetUniformLocation(r->text_prog, "u_atlas"),    0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, glyph_atlas_texture(r->atlas));

        glBindVertexArray(r->text_vao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, text_count);
        glBindVertexArray(0);

        glDisable(GL_BLEND);
    }

    /* ── Underline pass (OSC 8 hyperlink 및 SGR underline) ─────────────── */
    {
        int ul_count = 0;
        for (int row = 0; row < rows && ul_count < MAX_INSTANCES; row++) {
            for (int col = 0; col < cols && ul_count < MAX_INSTANCES; col++) {
                const term_cell_t *c = &cells[row * cols + col];
                if (!(c->attrs & CELL_ATTR_UNDERLINE)) continue;
                if (c->attrs & CELL_ATTR_WIDE_CONT) continue;
                bg_instance_t *ui = &r->bg_buf[ul_count++];
                /* cell_size=(cw,ch) 로 그리드 배치, quad_size=(cw,2)로 두께만 변경.
                 * pane_offset 에 ch-2 를 더해 셀 하단에 배치. */
                ui->col = (float)col;
                ui->row = (float)row;
                ui->r   = c->fg_r / 255.0f;
                ui->g   = c->fg_g / 255.0f;
                ui->b   = c->fg_b / 255.0f;
            }
        }
        if (ul_count > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, r->bg_inst_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            (GLsizeiptr)(ul_count * (int)sizeof(bg_instance_t)),
                            r->bg_buf);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(r->bg_prog);
            glUniform2f(glGetUniformLocation(r->bg_prog, "u_cell_size"), cw, ch);
            glUniform2f(glGetUniformLocation(r->bg_prog, "u_quad_size"), cw, 2.0f);
            glUniform2f(glGetUniformLocation(r->bg_prog, "u_viewport"),  vw, vh);
            glUniform2f(glGetUniformLocation(r->bg_prog, "u_pane_offset"),
                        ox, oy + ch - 2.0f);
            glUniform1f(glGetUniformLocation(r->bg_prog, "u_opacity"), 1.0f);
            glBindVertexArray(r->bg_vao);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, ul_count);
            glBindVertexArray(0);
            glDisable(GL_BLEND);
        }
    }
}

void gl_renderer_draw_cursor(gl_renderer_t *r,
                              const term_cell_t *cells, int cols,
                              int col, int row, int cursor_style, int visible,
                              float fg_r, float fg_g, float fg_b,
                              pane_rect_t pane)
{
    if (!visible) return;

    float cw = (float)r->cell_w;
    float ch = (float)r->cell_h;
    float vw = (float)r->viewport_w;
    float vh = (float)r->viewport_h;
    float ox = (float)pane.x;
    float oy = (float)pane.y;

    /*
     * BG 셰이더를 재활용한다.
     * 셰이더 공식: pix = pane_offset + cell_pos * cell_size + v_pos * cell_size
     * cell_pos=(col,row), v_pos=[0,1] quad → 결과는 pane_offset 기준 좌상단.
     *
     * 커서 형태별로 pane_offset과 cell_size를 조작하면 된다:
     *   block     → cell_size 그대로, offset 그대로
     *   underline → cell_size=(cw, 2px), offset.y += row*ch + (ch-2)
     *   bar       → cell_size=(2px, ch), offset.y += row*ch
     *
     * 인스턴스의 col/row를 직접 픽셀 위치로 매핑하기 위해
     * cell_size=(1,1)로 놓고 col/row에 픽셀 좌표를 넣는다.
     */

    float cell_x = ox + (float)col * cw;
    float cell_y = oy + (float)row * ch;

    /* Hollow block: 4 edge quads (top/bottom/left/right), no reverse video. */
    if (cursor_style == 7 || cursor_style == 8) {
        float thick = 1.5f;
        struct { float x, y, w, h; } edges[4] = {
            { cell_x,          cell_y,              cw,    thick },  /* top */
            { cell_x,          cell_y + ch - thick, cw,    thick },  /* bottom */
            { cell_x,          cell_y,              thick, ch    },  /* left */
            { cell_x + cw - thick, cell_y,          thick, ch    },  /* right */
        };
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r->bg_prog);
        glUniform2f(glGetUniformLocation(r->bg_prog, "u_quad_size"), 0.0f, 0.0f);
        glUniform2f(glGetUniformLocation(r->bg_prog, "u_viewport"),  vw, vh);
        glUniform1f(glGetUniformLocation(r->bg_prog, "u_opacity"),   1.0f);
        for (int e = 0; e < 4; e++) {
            bg_instance_t ei = { .col = 0.0f, .row = 0.0f,
                                  .r = fg_r, .g = fg_g, .b = fg_b };
            glBindBuffer(GL_ARRAY_BUFFER, r->bg_inst_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(ei), &ei);
            glUniform2f(glGetUniformLocation(r->bg_prog, "u_cell_size"),
                        edges[e].w, edges[e].h);
            glUniform2f(glGetUniformLocation(r->bg_prog, "u_pane_offset"),
                        edges[e].x, edges[e].y);
            glBindVertexArray(r->bg_vao);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
            glBindVertexArray(0);
        }
        glDisable(GL_BLEND);
        return;
    }

    float px_x = cell_x;
    float px_y = cell_y;
    float qw, qh;

    switch (cursor_style) {
    case 3: case 4: /* underline */
        qw = cw; qh = 2.0f;
        px_y += ch - 2.0f;
        break;
    case 5: case 6: /* bar */
        qw = 2.0f; qh = ch;
        break;
    default: /* block */
        qw = cw; qh = ch;
        break;
    }

    /* 셰이더 수식: pix = u_pane_offset + i_cell * u_cell_size + v_pos * u_cell_size
     * 쿼드 한 개만 그리므로 i_cell=(0,0) 으로 두고 pane_offset 에 픽셀 위치를,
     * cell_size 에 쿼드 크기를 넣는다. */
    bg_instance_t inst = { .col = 0.0f, .row = 0.0f,
                            .r = fg_r, .g = fg_g, .b = fg_b };

    glBindBuffer(GL_ARRAY_BUFFER, r->bg_inst_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(inst), &inst);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->bg_prog);
    glUniform2f(glGetUniformLocation(r->bg_prog, "u_cell_size"), qw, qh);
    glUniform2f(glGetUniformLocation(r->bg_prog, "u_quad_size"), 0.0f, 0.0f);
    glUniform2f(glGetUniformLocation(r->bg_prog, "u_viewport"),  vw, vh);
    glUniform2f(glGetUniformLocation(r->bg_prog, "u_pane_offset"), px_x, px_y);
    glUniform1f(glGetUniformLocation(r->bg_prog, "u_opacity"), 1.0f);

    glBindVertexArray(r->bg_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
    glBindVertexArray(0);

    glDisable(GL_BLEND);

    /* Block 스타일이면 커서 아래 글자를 배경색으로 덧그려 reverse video 구현.
     * bar/underline 은 글자를 가리지 않으므로 스킵. */
    int is_block = (cursor_style == 0 || cursor_style == 1 || cursor_style == 2);
    if (!is_block || !cells) return;
    if (cols <= 0 || col < 0 || col >= cols || row < 0) return;

    const term_cell_t *c = &cells[row * cols + col];
    if (!c->codepoint || c->codepoint == ' ') return;
    if (c->attrs & CELL_ATTR_WIDE_CONT) return;

    atlas_glyph_t ag = {0};
    if (ensure_glyph(r, c->codepoint, &ag) != 0 || ag.px_w <= 0) return;

    /* 커서 아래 원래 배경색을 글리프 색으로 사용.
     * 이 셀이 원래 reverse 였거나 선택 영역이었다면 그 상태에서 스왑된 bg = 원래 fg. */
    text_instance_t ti = {0};
    ti.col       = (float)col;
    ti.row       = (float)row;
    ti.bearing_x = (float)ag.bearing_x;
    ti.bearing_y = (float)ag.bearing_y;
    ti.atlas_x   = ag.u;
    ti.atlas_y   = ag.v;
    ti.atlas_w   = ag.uw;
    ti.atlas_h   = ag.vh;
    ti.glyph_w   = (float)ag.px_w;
    ti.glyph_h   = (float)ag.px_h;
    ti.r         = c->bg_r / 255.0f;
    ti.g         = c->bg_g / 255.0f;
    ti.b         = c->bg_b / 255.0f;

    glBindBuffer(GL_ARRAY_BUFFER, r->text_inst_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(ti), &ti);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->text_prog);
    glUniform2f(glGetUniformLocation(r->text_prog, "u_cell_size"),   cw, ch);
    glUniform2f(glGetUniformLocation(r->text_prog, "u_viewport"),    vw, vh);
    glUniform2f(glGetUniformLocation(r->text_prog, "u_pane_offset"), ox, oy);
    glUniform1f(glGetUniformLocation(r->text_prog, "u_ascender"),    (float)r->ascender);
    glUniform1i(glGetUniformLocation(r->text_prog, "u_atlas"),       0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glyph_atlas_texture(r->atlas));

    glBindVertexArray(r->text_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}
