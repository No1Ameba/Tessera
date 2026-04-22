#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "nuklear.h"

#include "nk_impl.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <string.h>
#include <stdio.h>

/* ── GL3 backend state ──────────────────────────────────────────────────── */

#define NK_MAX_VERTEX_BUFFER  (512 * 1024)
#define NK_MAX_ELEMENT_BUFFER (128 * 1024)

struct nk_vertex {
    float position[2];
    float uv[2];
    nk_byte col[4];
};

static struct {
    GLFWwindow       *win;
    struct nk_context ctx;
    struct nk_font_atlas atlas;
    struct nk_buffer  cmds;
    struct nk_draw_null_texture null_tex;

    /* GL objects */
    GLuint prog, vert_shdr, frag_shdr;
    GLuint vbo, ebo, vao;
    GLint  uniform_tex, uniform_proj;
    GLuint font_tex;

    /* Input state */
    double last_mouse_x, last_mouse_y;
    float  scroll_x, scroll_y;  /* 콜백에서 누적, new_frame에서 소비 */
    int    mouse_btn[3];        /* 콜백에서 기록: -1=미변경, 0=released, 1=pressed */
    int    is_active; /* nk_window is being interacted with */

    /* 텍스트 입력 버퍼 (char_callback에서 누적, new_frame에서 flush) */
    unsigned int text_buf[64];
    int          text_len;

    /* 키 입력 버퍼 */
    struct { int key; int down; } key_buf[32];
    int key_buf_len;
} nk_gl;

/* ── Shaders ────────────────────────────────────────────────────────────── */

static const char *NK_VERT_SRC =
    "#version 330 core\n"
    "uniform mat4 ProjMtx;\n"
    "layout(location=0) in vec2 Position;\n"
    "layout(location=1) in vec2 TexCoord;\n"
    "layout(location=2) in vec4 Color;\n"
    "out vec2 Frag_UV;\n"
    "out vec4 Frag_Color;\n"
    "void main() {\n"
    "  Frag_UV = TexCoord;\n"
    "  Frag_Color = Color;\n"
    "  gl_Position = ProjMtx * vec4(Position, 0.0, 1.0);\n"
    "}\n";

static const char *NK_FRAG_SRC =
    "#version 330 core\n"
    "precision mediump float;\n"
    "uniform sampler2D Texture;\n"
    "in vec2 Frag_UV;\n"
    "in vec4 Frag_Color;\n"
    "out vec4 Out_Color;\n"
    "void main() {\n"
    "  Out_Color = Frag_Color * texture(Texture, Frag_UV);\n"
    "}\n";

static void nk_gl_create_shaders(void)
{
    nk_gl.vert_shdr = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(nk_gl.vert_shdr, 1, &NK_VERT_SRC, NULL);
    glCompileShader(nk_gl.vert_shdr);

    nk_gl.frag_shdr = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(nk_gl.frag_shdr, 1, &NK_FRAG_SRC, NULL);
    glCompileShader(nk_gl.frag_shdr);

    nk_gl.prog = glCreateProgram();
    glAttachShader(nk_gl.prog, nk_gl.vert_shdr);
    glAttachShader(nk_gl.prog, nk_gl.frag_shdr);
    glLinkProgram(nk_gl.prog);

    nk_gl.uniform_tex  = glGetUniformLocation(nk_gl.prog, "Texture");
    nk_gl.uniform_proj = glGetUniformLocation(nk_gl.prog, "ProjMtx");
}

static void nk_gl_create_buffers(void)
{
    glGenVertexArrays(1, &nk_gl.vao);
    glGenBuffers(1, &nk_gl.vbo);
    glGenBuffers(1, &nk_gl.ebo);

    glBindVertexArray(nk_gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, nk_gl.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nk_gl.ebo);

    GLsizei vs = (GLsizei)sizeof(struct nk_vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vs, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vs, (void*)(sizeof(float)*2));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, vs, (void*)(sizeof(float)*4));

    glBindVertexArray(0);
}

static void nk_gl_upload_font(void)
{
    const void *image;
    int w, h;
    image = nk_font_atlas_bake(&nk_gl.atlas, &w, &h, NK_FONT_ATLAS_RGBA32);

    glGenTextures(1, &nk_gl.font_tex);
    glBindTexture(GL_TEXTURE_2D, nk_gl.font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, image);

    nk_font_atlas_end(&nk_gl.atlas, nk_handle_id((int)nk_gl.font_tex),
                       &nk_gl.null_tex);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

struct nk_context *nk_impl_init(GLFWwindow *win,
                                 const char *font_path, float font_size)
{
    nk_gl.win = win;
    nk_gl.mouse_btn[0] = nk_gl.mouse_btn[1] = nk_gl.mouse_btn[2] = -1;
    nk_init_default(&nk_gl.ctx, NULL);
    nk_buffer_init_default(&nk_gl.cmds);

    nk_gl_create_shaders();
    nk_gl_create_buffers();

    /* Font baking — UI 오버레이(설정/확인/컨텍스트 메뉴) 에서 한글·라틴을 모두 표시해야 하므로
     * CJK glyph range 를 함께 베이킹한다. range 가 지정된 폰트 파일에 해당 glyph 가 없으면
     * 해당 코드포인트는 .notdef 로 폴백하므로 호출측에서 한글 지원 폰트를 전달해야 한다. */
    static const nk_rune ui_ranges[] = {
        0x0020, 0x00FF,  /* Basic Latin + Latin-1 Supplement */
        0x2000, 0x206F,  /* General Punctuation (—, …, 따옴표) */
        0x3000, 0x303F,  /* CJK Symbols and Punctuation */
        0x3130, 0x318F,  /* Hangul Compatibility Jamo */
        0xAC00, 0xD7A3,  /* Hangul Syllables */
        0xFF00, 0xFFEF,  /* Halfwidth/Fullwidth Forms */
        0
    };

    nk_font_atlas_init_default(&nk_gl.atlas);
    nk_font_atlas_begin(&nk_gl.atlas);

    struct nk_font_config cfg = nk_font_config(font_size);
    cfg.range        = ui_ranges;
    cfg.oversample_h = 1;   /* CJK 로 atlas 가 크게 불어나지 않도록 oversample 최소화 */
    cfg.oversample_v = 1;
    cfg.pixel_snap   = 1;

    struct nk_font *font = NULL;
    if (font_path) {
        font = nk_font_atlas_add_from_file(&nk_gl.atlas, font_path,
                                            font_size, &cfg);
    }
    if (!font) {
        /* 한글 포함 폰트 로드 실패 시 기본 폰트로 폴백 — 한글은 깨지지만 UI 는 동작 */
        fprintf(stderr, "[nk_impl] UI font '%s' load failed — CJK glyphs will be missing\n",
                font_path ? font_path : "(null)");
        font = nk_font_atlas_add_default(&nk_gl.atlas, font_size, NULL);
    }

    nk_gl_upload_font();

    if (font)
        nk_style_set_font(&nk_gl.ctx, &font->handle);

    /* 버튼 클릭 영역을 넉넉히 — hit testing 오차 완화 */
    nk_gl.ctx.style.button.padding = nk_vec2(6, 6);
    nk_gl.ctx.style.button.border  = 1.0f;
    nk_gl.ctx.style.window.padding = nk_vec2(8, 8);
    nk_gl.ctx.style.window.spacing = nk_vec2(6, 6);

    return &nk_gl.ctx;
}

void nk_impl_new_frame(void)
{
    int w, h;
    int display_w, display_h;
    glfwGetWindowSize(nk_gl.win, &w, &h);
    glfwGetFramebufferSize(nk_gl.win, &display_w, &display_h);

    /* Window↔Framebuffer 좌표 변환 스케일 (HiDPI 대응) */
    double sx = (w > 0)  ? (double)display_w / (double)w  : 1.0;
    double sy = (h > 0)  ? (double)display_h / (double)h  : 1.0;

    nk_input_begin(&nk_gl.ctx);

    /* Mouse position (Window 좌표 → Framebuffer 좌표로 스케일) */
    double mx, my;
    glfwGetCursorPos(nk_gl.win, &mx, &my);
    int fmx = (int)(mx * sx);
    int fmy = (int)(my * sy);
    nk_input_motion(&nk_gl.ctx, fmx, fmy);

    /* Mouse buttons — 콜백에서 버퍼된 상태 사용, 없으면 폴링 */
    {
        int left  = (nk_gl.mouse_btn[0] >= 0) ? nk_gl.mouse_btn[0]
                  : (glfwGetMouseButton(nk_gl.win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        int mid   = (nk_gl.mouse_btn[1] >= 0) ? nk_gl.mouse_btn[1]
                  : (glfwGetMouseButton(nk_gl.win, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
        int right = (nk_gl.mouse_btn[2] >= 0) ? nk_gl.mouse_btn[2]
                  : (glfwGetMouseButton(nk_gl.win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        nk_input_button(&nk_gl.ctx, NK_BUTTON_LEFT,   fmx, fmy, left);
        nk_input_button(&nk_gl.ctx, NK_BUTTON_MIDDLE, fmx, fmy, mid);
        nk_input_button(&nk_gl.ctx, NK_BUTTON_RIGHT,  fmx, fmy, right);
        nk_gl.mouse_btn[0] = nk_gl.mouse_btn[1] = nk_gl.mouse_btn[2] = -1;
    }

    /* Scroll (콜백에서 누적된 값 소비) */
    if (nk_gl.scroll_x != 0.0f || nk_gl.scroll_y != 0.0f) {
        nk_input_scroll(&nk_gl.ctx, nk_vec2(nk_gl.scroll_x, nk_gl.scroll_y));
        nk_gl.scroll_x = 0.0f;
        nk_gl.scroll_y = 0.0f;
    }

    /* 키 입력 flush */
    for (int i = 0; i < nk_gl.key_buf_len; i++) {
        int k = nk_gl.key_buf[i].key;
        int down = nk_gl.key_buf[i].down;
        switch (k) {
        case GLFW_KEY_BACKSPACE: nk_input_key(&nk_gl.ctx, NK_KEY_BACKSPACE, down); break;
        case GLFW_KEY_DELETE:    nk_input_key(&nk_gl.ctx, NK_KEY_DEL, down); break;
        case GLFW_KEY_ENTER:     nk_input_key(&nk_gl.ctx, NK_KEY_ENTER, down); break;
        case GLFW_KEY_TAB:       nk_input_key(&nk_gl.ctx, NK_KEY_TAB, down); break;
        case GLFW_KEY_LEFT:      nk_input_key(&nk_gl.ctx, NK_KEY_LEFT, down); break;
        case GLFW_KEY_RIGHT:     nk_input_key(&nk_gl.ctx, NK_KEY_RIGHT, down); break;
        case GLFW_KEY_UP:        nk_input_key(&nk_gl.ctx, NK_KEY_UP, down); break;
        case GLFW_KEY_DOWN:      nk_input_key(&nk_gl.ctx, NK_KEY_DOWN, down); break;
        }
    }
    nk_gl.key_buf_len = 0;

    /* 텍스트 입력 flush */
    for (int i = 0; i < nk_gl.text_len; i++)
        nk_input_unicode(&nk_gl.ctx, nk_gl.text_buf[i]);
    nk_gl.text_len = 0;

    nk_input_end(&nk_gl.ctx);

    nk_gl.is_active = nk_window_is_any_hovered(&nk_gl.ctx);
}

void nk_impl_render(void)
{
    int display_w, display_h;
    glfwGetFramebufferSize(nk_gl.win, &display_w, &display_h);

    /* Save GL state */
    GLint last_prog, last_tex, last_vao, last_vbo, last_ebo;
    GLint last_blend_src, last_blend_dst;
    GLboolean last_blend, last_cull, last_depth, last_scissor;
    GLint last_viewport[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_vbo);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_ebo);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &last_blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &last_blend_dst);
    last_blend   = glIsEnabled(GL_BLEND);
    last_cull    = glIsEnabled(GL_CULL_FACE);
    last_depth   = glIsEnabled(GL_DEPTH_TEST);
    last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_VIEWPORT, last_viewport);

    /* Setup GL state */
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, display_w, display_h);

    /* Projection matrix */
    float L = 0.0f, R = (float)display_w;
    float T = 0.0f, B = (float)display_h;
    float ortho[4][4] = {
        {2.0f/(R-L),   0.0f,          0.0f, 0.0f},
        {0.0f,         2.0f/(T-B),    0.0f, 0.0f},
        {0.0f,         0.0f,         -1.0f, 0.0f},
        {(R+L)/(L-R),  (T+B)/(B-T),   0.0f, 1.0f},
    };

    glUseProgram(nk_gl.prog);
    glUniform1i(nk_gl.uniform_tex, 0);
    glUniformMatrix4fv(nk_gl.uniform_proj, 1, GL_FALSE, &ortho[0][0]);

    /* Upload vertex/element data */
    glBindVertexArray(nk_gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, nk_gl.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nk_gl.ebo);
    glBufferData(GL_ARRAY_BUFFER, NK_MAX_VERTEX_BUFFER, NULL, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, NK_MAX_ELEMENT_BUFFER, NULL, GL_STREAM_DRAW);

    void *vertices = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    void *elements = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);

    {
        struct nk_convert_config config;
        static const struct nk_draw_vertex_layout_element vertex_layout[] = {
            {NK_VERTEX_POSITION, NK_FORMAT_FLOAT,    0},
            {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,    sizeof(float)*2},
            {NK_VERTEX_COLOR,    NK_FORMAT_R8G8B8A8, sizeof(float)*4},
            {NK_VERTEX_LAYOUT_END}
        };
        memset(&config, 0, sizeof(config));
        config.vertex_layout = vertex_layout;
        config.vertex_size = sizeof(struct nk_vertex);
        config.vertex_alignment = NK_ALIGNOF(struct nk_vertex);
        config.tex_null = nk_gl.null_tex;
        config.circle_segment_count = 22;
        config.curve_segment_count = 22;
        config.arc_segment_count = 22;
        config.global_alpha = 1.0f;
        config.shape_AA = NK_ANTI_ALIASING_ON;
        config.line_AA = NK_ANTI_ALIASING_ON;

        struct nk_buffer vbuf, ebuf;
        nk_buffer_init_fixed(&vbuf, vertices, NK_MAX_VERTEX_BUFFER);
        nk_buffer_init_fixed(&ebuf, elements, NK_MAX_ELEMENT_BUFFER);
        nk_convert(&nk_gl.ctx, &nk_gl.cmds, &vbuf, &ebuf, &config);
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);

    /* Draw */
    const struct nk_draw_command *cmd;
    nk_size offset = 0;
    nk_draw_foreach(cmd, &nk_gl.ctx, &nk_gl.cmds) {
        if (!cmd->elem_count) continue;
        glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
        glScissor(
            (GLint)(cmd->clip_rect.x),
            (GLint)(display_h - (cmd->clip_rect.y + cmd->clip_rect.h)),
            (GLint)(cmd->clip_rect.w),
            (GLint)(cmd->clip_rect.h));
        glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count,
                       GL_UNSIGNED_SHORT, (const void*)offset);
        offset += cmd->elem_count * sizeof(nk_draw_index);
    }

    nk_clear(&nk_gl.ctx);
    nk_buffer_clear(&nk_gl.cmds);

    /* Restore GL state */
    glUseProgram((GLuint)last_prog);
    glBindTexture(GL_TEXTURE_2D, (GLuint)last_tex);
    glBindVertexArray((GLuint)last_vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)last_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)last_ebo);
    glBlendFunc((GLenum)last_blend_src, (GLenum)last_blend_dst);
    if (last_blend)   glEnable(GL_BLEND);   else glDisable(GL_BLEND);
    if (last_cull)    glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (last_depth)   glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glViewport(last_viewport[0], last_viewport[1],
               last_viewport[2], last_viewport[3]);
}

int nk_impl_handle_key(int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    if (nk_gl.key_buf_len < (int)(sizeof(nk_gl.key_buf)/sizeof(nk_gl.key_buf[0]))) {
        nk_gl.key_buf[nk_gl.key_buf_len].key  = key;
        nk_gl.key_buf[nk_gl.key_buf_len].down = (action != GLFW_RELEASE);
        nk_gl.key_buf_len++;
    }
    return 1;
}

int nk_impl_handle_char(unsigned int codepoint)
{
    if (nk_gl.text_len < (int)(sizeof(nk_gl.text_buf)/sizeof(nk_gl.text_buf[0])))
        nk_gl.text_buf[nk_gl.text_len++] = codepoint;
    return 1;
}

int nk_impl_handle_mouse_button(int button, int action, int mods)
{
    (void)mods;
    int pressed = (action == GLFW_PRESS);
    if (button == GLFW_MOUSE_BUTTON_LEFT)   nk_gl.mouse_btn[0] = pressed;
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) nk_gl.mouse_btn[1] = pressed;
    if (button == GLFW_MOUSE_BUTTON_RIGHT)  nk_gl.mouse_btn[2] = pressed;
    return nk_gl.is_active;
}

int nk_impl_handle_scroll(double xoff, double yoff)
{
    if (!nk_gl.is_active) return 0;
    nk_gl.scroll_x += (float)xoff;
    nk_gl.scroll_y += (float)yoff;
    return 1;
}

void nk_impl_handle_cursor_pos(double x, double y)
{
    nk_gl.last_mouse_x = x;
    nk_gl.last_mouse_y = y;
}

void nk_impl_reset_input(void)
{
    nk_gl.mouse_btn[0] = nk_gl.mouse_btn[1] = nk_gl.mouse_btn[2] = -1;
    nk_gl.scroll_x = nk_gl.scroll_y = 0.0f;
    /* Nuklear 내부 입력 상태도 클리어 */
    nk_input_begin(&nk_gl.ctx);
    nk_input_end(&nk_gl.ctx);
}

void nk_impl_show_window(const char *name, int show)
{
    enum nk_show_states state = show ? NK_SHOWN : NK_HIDDEN;
    nk_window_show(&nk_gl.ctx, name, state);
}

void nk_impl_shutdown(void)
{
    nk_font_atlas_clear(&nk_gl.atlas);
    nk_buffer_free(&nk_gl.cmds);
    nk_free(&nk_gl.ctx);

    if (nk_gl.font_tex) glDeleteTextures(1, &nk_gl.font_tex);
    if (nk_gl.prog)     glDeleteProgram(nk_gl.prog);
    if (nk_gl.vert_shdr) glDeleteShader(nk_gl.vert_shdr);
    if (nk_gl.frag_shdr) glDeleteShader(nk_gl.frag_shdr);
    if (nk_gl.vao) glDeleteVertexArrays(1, &nk_gl.vao);
    if (nk_gl.vbo) glDeleteBuffers(1, &nk_gl.vbo);
    if (nk_gl.ebo) glDeleteBuffers(1, &nk_gl.ebo);

    memset(&nk_gl, 0, sizeof(nk_gl));
}
