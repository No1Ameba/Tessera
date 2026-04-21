#define _GNU_SOURCE
/*
 * termemu client — GLFW + OpenGL 3.3 Core main loop
 *
 * Cross-platform: Linux, macOS, Windows
 *
 * Shortcuts (configurable):
 *   Alt+-        수직 분할
 *   Alt+=        수평 분할
 *   Alt+h/j/k/l 포커스 이동
 *   Ctrl+W       현재 pane 닫기
 *   Ctrl+,       설정 오버레이
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "renderer/font.h"
#include "renderer/glyph_atlas.h"
#include "renderer/gl_renderer.h"
#include "ipc_client.h"
#include "ui/layout.h"
#include "ui/input.h"
#include "ui/nk_impl.h"
#include "ui/settings_ui.h"

/* Nuklear 선언만 (NK_IMPLEMENTATION 없이) — 컨텍스트 메뉴에서 직접 사용 */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#include "nuklear.h"
#include "screen.h"
#include "../common/config.h"
#include "../common/utf8.h"
#include "../common/session_file.h"
#include "../platform/fs_watch.h"

/* ── Pane registry ───────────────────────────────────────────────────────── */

#define MAX_PANES   16

typedef struct {
    uint32_t  pane_id;
    screen_t  screen;
    int       used;
} pane_slot_t;

static pane_slot_t g_panes[MAX_PANES];

typedef struct { ipc_client_t *c; uint32_t sid; uint32_t wid;
                 int fw; int fh; } resize_ctx_t;
static void resize_leaf_cb(layout_node_t *leaf, void *user);

static pane_slot_t *pane_slot_find(uint32_t pane_id)
{
    for (int i = 0; i < MAX_PANES; i++)
        if (g_panes[i].used && g_panes[i].pane_id == pane_id)
            return &g_panes[i];
    return NULL;
}

static pane_slot_t *pane_slot_alloc(uint32_t pane_id, int cols, int rows)
{
    /* 같은 pane_id 가 이미 있으면 재사용 — 중복 할당 방지 */
    pane_slot_t *existing = pane_slot_find(pane_id);
    if (existing) return existing;

    extern int g_scrollback_lines;  /* 아래 전역 선언 */
    int sb = g_scrollback_lines > 0 ? g_scrollback_lines : 1000;
    for (int i = 0; i < MAX_PANES; i++) {
        if (!g_panes[i].used) {
            g_panes[i].pane_id = pane_id;
            g_panes[i].used    = 1;
            screen_init(&g_panes[i].screen, cols, rows, sb);
            return &g_panes[i];
        }
    }
    return NULL;
}

static void pane_slot_free(uint32_t pane_id)
{
    pane_slot_t *s = pane_slot_find(pane_id);
    if (!s) return;
    screen_destroy(&s->screen);
    s->used = 0;
}

/* ── Global state ────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static volatile int g_dirty   = 1;

static const termemu_theme_t *g_theme = NULL;

static uint32_t       g_session_id    = 0;
static uint32_t       g_window_id     = 0;
static uint32_t       g_active_pane   = 0;
static layout_node_t *g_layout        = NULL;

static ipc_client_t  *g_client        = NULL;
static font_face_t   *g_font          = NULL;
static glyph_atlas_t *g_atlas         = NULL;
int                   g_scrollback_lines = 1000;
static GLFWwindow    *g_window        = NULL;
static gl_renderer_t *g_renderer      = NULL;

/* 설정 오버레이 */
static int g_show_settings = 0;
static struct nk_context *g_nk_ctx = NULL;

/* 세션 선택 다이얼로그 */
static int                 g_show_session_picker = 0;
static ipc_session_info_t  g_picker_sessions[64];
static int                 g_picker_count = 0;
static char                g_new_session_name[64] = "default";

/* 우클릭 컨텍스트 메뉴 */
static int g_show_context_menu = 0;
static float g_context_menu_x = 0, g_context_menu_y = 0;

/* 텍스트 선택 */
static int      g_selecting = 0;      /* 드래그 중 */
static int      g_has_selection = 0;  /* 선택 확정됨 */
static uint32_t g_sel_pane = 0;
static int      g_sel_sc = -1, g_sel_sr = -1; /* start col/row */
static int      g_sel_ec = -1, g_sel_er = -1; /* end col/row */

/* 설정 재로드 플래그 */
static volatile sig_atomic_t g_sighup = 0;

#ifndef _WIN32
static void sighup_handler(int sig) { (void)sig; g_sighup = 1; }
#endif

/* 윈도우 크기 */
static int g_win_w = 1280, g_win_h = 720;

/* 타이틀 중복 방지 */
static char g_last_title[256] = {0};

/* config/theme 경로 (reload 용) */
static char g_cfg_path[512]   = {0};
static char g_theme_path[512] = {0};
static termemu_config_t *g_cfg_ptr = NULL;
static termemu_theme_t  *g_theme_mut_ptr = NULL;

/* key_callback에서 처리 완료 시 char_callback 무시 */
static int g_key_consumed = 0;

/* ── Callbacks ───────────────────────────────────────────────────────────── */

/* OSC 52 클립보드 콜백 — screen에서 호출됨 */
static void on_clipboard_set(const char *text, void *user)
{
    (void)user;
    if (g_window && text)
        glfwSetClipboardString(g_window, text);
}

static void on_pty_output(uint32_t pane_id, const uint8_t *data, size_t len,
                           void *user)
{
    (void)user;
    pane_slot_t *s = pane_slot_find(pane_id);
    if (s) {
        screen_feed(&s->screen, data, len);
        if (!screen_sync_output(&s->screen))
            g_dirty = 1;
    }
}

static void push_layout_to_daemon(void);
static void compute_layout_rect(int *x, int *y, int *w, int *h);

static void on_pane_exited(uint32_t pane_id, void *user)
{
    (void)user;

    if (pane_id == g_active_pane) {
        for (int i = 0; i < MAX_PANES; i++) {
            if (g_panes[i].used && g_panes[i].pane_id != pane_id) {
                g_active_pane = g_panes[i].pane_id;
                break;
            }
        }
    }

    /* 같은 pane_id 를 가진 모든 leaf 제거 (중복 상태 복구) */
    int removed = 0;
    for (;;) {
        layout_node_t *node = layout_find_pane(g_layout, pane_id);
        if (!node) break;
        layout_remove(&g_layout, node);
        removed++;
        if (!g_layout) break;
        if (removed > 16) break;  /* 안전망 */
    }

    /* pane_slot 도 함께 정리. slot 이 실제로 존재해서 free 됐는지 여부를 반환받는다. */
    pane_slot_t *slot_before = pane_slot_find(pane_id);
    int slot_freed = slot_before != NULL;
    pane_slot_free(pane_id);

    /* 이번 이벤트가 실제로 상태를 바꿨을 때만 daemon 에 새 layout 을 전송한다.
     * 이벤트와 무관한 pane_id (우리가 모르는 pane) 에 대해 push 를 보내면
     * daemon 과의 재전송 루프를 유발할 수 있다. */
    int changed = (removed > 0) || slot_freed;

    if (g_layout) {
        if (changed) {
            int fw = font_cell_width(g_font);
            int fh = font_cell_height(g_font);
            resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
            layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
        }
    } else {
        g_running = 0;
    }
    g_dirty = 1;
    if (changed) push_layout_to_daemon();
}

/* 현재 layout tree 를 직렬화해서 데몬에 업로드한다 (재접속 복원용). */
static void push_layout_to_daemon(void)
{
    if (!g_client || !g_layout || g_session_id == 0 || g_window_id == 0) return;
    uint8_t blob[4096];
    int n = layout_serialize(g_layout, blob, sizeof blob);
    if (n < 0) return;
    ipc_client_window_layout(g_client, g_session_id, g_window_id,
                              blob, (uint16_t)n);
}

/* 다른 클라이언트의 split을 우리 layout 트리에 반영 */
static void on_pane_split(uint32_t session_id, uint32_t window_id,
                           uint32_t parent_pane_id, uint32_t new_pane_id,
                           uint16_t cols, uint16_t rows,
                           uint8_t direction, float ratio,
                           void *user)
{
    (void)user; (void)cols; (void)rows;

    if (session_id != g_session_id || window_id != g_window_id) return;
    if (!g_layout) return;
    if (pane_slot_find(new_pane_id)) return;  /* 이미 처리됨 */

    layout_node_t *parent_leaf = layout_find_pane(g_layout, parent_pane_id);
    if (!parent_leaf) return;

    layout_node_type_t dir = (direction == 1) ? LAYOUT_SPLIT_V : LAYOUT_SPLIT_H;
    if (ratio <= 0.0f || ratio >= 1.0f) ratio = 0.5f;

    layout_node_t *new_leaf = layout_split(parent_leaf, dir, ratio, new_pane_id);
    if (!new_leaf) return;

    layout_node_t *r = parent_leaf;
    while (r->parent) r = r->parent;
    g_layout = r;

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    int new_nc = new_leaf->rect.w / fw; if (new_nc < 1) new_nc = 1;
    int new_nr = new_leaf->rect.h / fh; if (new_nr < 1) new_nr = 1;

    pane_slot_t *ns = pane_slot_alloc(new_pane_id, new_nc, new_nr);
    if (ns) {
        if (g_theme) screen_apply_theme(&ns->screen, g_theme);
        screen_set_clipboard_cb(&ns->screen, on_clipboard_set, NULL);
    }

    /* 부모 pane의 클라이언트 측 screen도 새로운 크기로 리사이즈 */
    int par_nc = parent_leaf->rect.w / fw; if (par_nc < 1) par_nc = 1;
    int par_nr = parent_leaf->rect.h / fh; if (par_nr < 1) par_nr = 1;
    pane_slot_t *par_slot = pane_slot_find(parent_pane_id);
    if (par_slot) screen_resize(&par_slot->screen, par_nc, par_nr);

    g_dirty = 1;
}

/* ── 폰트 경로 해석 ──────────────────────────────────────────────────────── */

static const char *resolve_font_path(const char *name, char *buf, size_t bufsz)
{
    if (name && name[0] == '/') {
        FILE *f = fopen(name, "rb");
        if (f) { fclose(f); return name; }
    }
#ifndef _WIN32
    if (name && name[0]) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "fc-match --format='%%{file}' '%s' 2>/dev/null", name);
        FILE *p = popen(cmd, "r");
        if (p) {
            size_t n = fread(buf, 1, bufsz - 1, p);
            pclose(p);
            if (n > 0) {
                buf[n] = '\0';
                if (buf[n-1] == '\n') buf[--n] = '\0';
                FILE *f = fopen(buf, "rb");
                if (f) { fclose(f); return buf; }
            }
        }
    }
#endif
    static const char *fallbacks[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
        NULL
    };
    for (int i = 0; fallbacks[i]; i++) {
        FILE *f = fopen(fallbacks[i], "rb");
        if (f) { fclose(f); return fallbacks[i]; }
    }
    return name;
}

/* ── Split helpers ───────────────────────────────────────────────────────── */

static void do_split(layout_node_type_t dir)
{
    if (!g_layout || !g_client) return;

    layout_node_t *cur_leaf = layout_find_pane(g_layout, g_active_pane);
    if (!cur_leaf) return;

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);

    int new_w, new_h;
    if (dir == LAYOUT_SPLIT_H) {
        new_w = cur_leaf->rect.w / 2;
        new_h = cur_leaf->rect.h;
    } else {
        new_w = cur_leaf->rect.w;
        new_h = cur_leaf->rect.h / 2;
    }
    int nc = new_w / fw; if (nc < 1) nc = 1;
    int nr = new_h / fh; if (nr < 1) nr = 1;

    uint32_t new_pane_id = 0;
    uint8_t  notify_dir  = (dir == LAYOUT_SPLIT_V) ? 1 : 0;
    if (ipc_client_pane_create_split(g_client, g_session_id, g_window_id,
                                      (uint16_t)nc, (uint16_t)nr,
                                      g_active_pane, notify_dir, 0.5f,
                                      &new_pane_id) != 0)
        return;

    layout_node_t *new_leaf = layout_split(cur_leaf, dir, 0.5f, new_pane_id);
    if (!new_leaf) {
        ipc_client_pane_destroy(g_client, g_session_id, g_window_id, new_pane_id);
        return;
    }

    {
        layout_node_t *r = cur_leaf;
        while (r->parent) r = r->parent;
        g_layout = r;
    }

    int old_nc = cur_leaf->rect.w / fw; if (old_nc < 1) old_nc = 1;
    int old_nr = cur_leaf->rect.h / fh; if (old_nr < 1) old_nr = 1;
    ipc_client_pane_resize(g_client, g_session_id, g_window_id,
                            g_active_pane, (uint16_t)old_nc, (uint16_t)old_nr);
    pane_slot_t *cur_slot = pane_slot_find(g_active_pane);
    if (cur_slot) screen_resize(&cur_slot->screen, old_nc, old_nr);

    int new_nc = new_leaf->rect.w / fw; if (new_nc < 1) new_nc = 1;
    int new_nr = new_leaf->rect.h / fh; if (new_nr < 1) new_nr = 1;
    {
        pane_slot_t *ns = pane_slot_alloc(new_pane_id, new_nc, new_nr);
        if (ns) {
            if (g_theme) screen_apply_theme(&ns->screen, g_theme);
            screen_set_clipboard_cb(&ns->screen, on_clipboard_set, NULL);
        }
    }
    g_active_pane = new_pane_id;
    g_dirty = 1;
    push_layout_to_daemon();
}

static void do_close_pane(void)
{
    if (!g_layout || !g_client) return;

    layout_node_t *node = layout_find_pane(g_layout, g_active_pane);
    if (!node) return;

    uint32_t next_pane = 0;
    for (int i = 0; i < MAX_PANES; i++) {
        if (g_panes[i].used && g_panes[i].pane_id != g_active_pane) {
            next_pane = g_panes[i].pane_id;
            break;
        }
    }

    ipc_client_pane_destroy(g_client, g_session_id, g_window_id, g_active_pane);
    /* 같은 pane_id 를 가진 모든 leaf 제거 (중복 상태 복구) */
    uint32_t closing_id = g_active_pane;
    for (;;) {
        layout_node_t *n = layout_find_pane(g_layout, closing_id);
        if (!n) break;
        layout_remove(&g_layout, n);
        if (!g_layout) break;
    }
    pane_slot_free(closing_id);
    (void)node;

    if (next_pane) {
        g_active_pane = next_pane;
        int fw = font_cell_width(g_font);
        int fh = font_cell_height(g_font);
        resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
        layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
    } else {
        g_running = 0;
    }
    g_dirty = 1;
    push_layout_to_daemon();
}

/* ── Copy / Paste / Resize ──────────────────────────────────────────────── */

static char *selection_to_text(const screen_t *scr,
                                int sc, int sr, int ec, int er);

static void do_copy_selection(void)
{
    if (!g_has_selection) return;
    pane_slot_t *ss = pane_slot_find(g_sel_pane);
    if (!ss) return;
    char *text = selection_to_text(&ss->screen,
                                     g_sel_sc, g_sel_sr, g_sel_ec, g_sel_er);
    if (text) {
        glfwSetClipboardString(g_window, text);
        free(text);
    }
}

static void do_paste_clipboard(void)
{
    const char *clip = glfwGetClipboardString(g_window);
    if (!clip) return;
    pane_slot_t *s = pane_slot_find(g_active_pane);
    if (!s) return;
    int bracketed = screen_bracketed_paste(&s->screen);
    if (bracketed)
        ipc_client_pty_input(g_client, g_active_pane,
                              (const uint8_t*)"\x1b[200~", 6);
    ipc_client_pty_input(g_client, g_active_pane,
                          (const uint8_t*)clip, strlen(clip));
    if (bracketed)
        ipc_client_pty_input(g_client, g_active_pane,
                              (const uint8_t*)"\x1b[201~", 6);
}

/* dir: 0=left(H), 1=right(L), 2=down(J), 3=up(K)
 * 활성 pane 위치와 무관하게 경계선을 해당 방향으로 이동. */
static void do_resize_pane(int dir)
{
    if (!g_layout) return;
    layout_node_t *leaf = layout_find_pane(g_layout, g_active_pane);
    if (!leaf) return;

    layout_node_type_t want = (dir < 2) ? LAYOUT_SPLIT_H : LAYOUT_SPLIT_V;
    int positive = (dir == 1 || dir == 2);  /* right / down */

    layout_node_t *anc = leaf->parent;
    while (anc && anc->type != want) anc = anc->parent;
    if (!anc) return;

    float delta = 0.03f;
    float r     = anc->split_ratio + (positive ? delta : -delta);
    if (r < 0.05f) r = 0.05f;
    if (r > 0.95f) r = 0.95f;
    if (r == anc->split_ratio) return;
    anc->split_ratio = r;

    layout_resize_root(g_layout,
                        g_layout->rect.x, g_layout->rect.y,
                        g_layout->rect.w, g_layout->rect.h);

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
    layout_each_leaf(g_layout, resize_leaf_cb, &rctx);

    push_layout_to_daemon();
    g_dirty = 1;
}

/* URL 을 OS 기본 핸들러로 연다 (Linux: xdg-open, macOS: open). 비차단. */
static void open_url(const char *url)
{
    if (!url || !*url) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
            close(devnull);
        }
#ifdef __APPLE__
        execlp("open", "open", url, (char*)NULL);
#else
        execlp("xdg-open", "xdg-open", url, (char*)NULL);
#endif
        _exit(127);
    }
    /* SIGCHLD는 부모의 기존 핸들러가 reap (없으면 좀비; 짧은 수명이므로 OK) */
}

/* ── Focus navigation ────────────────────────────────────────────────────── */

typedef struct { int px, py; uint32_t found_id; } click_ctx_t;

static void click_focus_cb(layout_node_t *leaf, void *user)
{
    click_ctx_t *ctx = user;
    if (ctx->px >= leaf->rect.x && ctx->px < leaf->rect.x + leaf->rect.w &&
        ctx->py >= leaf->rect.y && ctx->py < leaf->rect.y + leaf->rect.h)
        ctx->found_id = leaf->pane_id;
}

typedef struct { int dir; uint32_t cur_id; uint32_t best_id; int best_dist; layout_rect_t src; } focus_ctx_t;

static void focus_pick_cb(layout_node_t *leaf, void *user)
{
    focus_ctx_t *ctx = user;
    if (leaf->pane_id == ctx->cur_id) return;

    int sx = ctx->src.x + ctx->src.w / 2;
    int sy = ctx->src.y + ctx->src.h / 2;
    int dx = leaf->rect.x + leaf->rect.w / 2;
    int dy = leaf->rect.y + leaf->rect.h / 2;

    int in_dir = 0;
    switch (ctx->dir) {
    case 0: in_dir = (dx < sx); break;
    case 1: in_dir = (dx > sx); break;
    case 2: in_dir = (dy < sy); break;
    case 3: in_dir = (dy > sy); break;
    }
    if (!in_dir) return;

    int dist = (dx-sx)*(dx-sx) + (dy-sy)*(dy-sy);
    if (ctx->best_dist < 0 || dist < ctx->best_dist) {
        ctx->best_dist = dist;
        ctx->best_id   = leaf->pane_id;
    }
}

static void do_focus(int dir)
{
    layout_node_t *cur = layout_find_pane(g_layout, g_active_pane);
    if (!cur) return;
    focus_ctx_t ctx = { dir, g_active_pane, 0, -1, cur->rect };
    layout_each_leaf(g_layout, focus_pick_cb, &ctx);
    if (ctx.best_id && ctx.best_id != g_active_pane) {
        g_active_pane = ctx.best_id;
        g_dirty = 1;
    }
}

/* ── 키바인딩 매칭 ───────────────────────────────────────────────────────── */

static int keybind_matches(const char *binding, unsigned int mods, int glfw_key)
{
    if (!binding || !binding[0]) return 0;

    char buf[64];
    strncpy(buf, binding, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    unsigned int req_mods = 0;
    char *p = buf;
    char *plus;

    while ((plus = strchr(p, '+')) != NULL) {
        *plus = '\0';
        if      (strcmp(p, "Alt")   == 0) req_mods |= INPUT_MOD_ALT;
        else if (strcmp(p, "Ctrl")  == 0) req_mods |= INPUT_MOD_CTRL;
        else if (strcmp(p, "Shift") == 0) req_mods |= INPUT_MOD_SHIFT;
        p = plus + 1;
    }

    int req_key = input_glfw_key_from_name(p);
    if (req_key < 0) return 0;

    return (mods == req_mods) && (glfw_key == req_key);
}

/* ── 선택 텍스트 추출 ────────────────────────────────────────────────────── */

static char *selection_to_text(const screen_t *scr,
                                int sc, int sr, int ec, int er)
{
    /* 정규화 */
    int r0, c0, r1, c1;
    if (sr < er || (sr == er && sc <= ec)) {
        r0 = sr; c0 = sc; r1 = er; c1 = ec;
    } else {
        r0 = er; c0 = ec; r1 = sr; c1 = sc;
    }

    const term_cell_t *cells = screen_get_cells((screen_t *)scr);
    int cols = scr->cols;

    /* 최대 크기 추정: 각 셀 최대 4바이트 UTF-8 + 행당 1바이트 \n */
    size_t max_sz = (size_t)(r1 - r0 + 1) * (size_t)(cols * 4 + 1) + 1;
    char *buf = malloc(max_sz);
    if (!buf) return NULL;
    size_t pos = 0;

    for (int row = r0; row <= r1; row++) {
        int col_start = (row == r0) ? c0 : 0;
        int col_end   = (row == r1) ? c1 : cols - 1;

        /* trailing whitespace trim을 위해 마지막 non-space 찾기 */
        int last_nonspace = col_start - 1;
        for (int col = col_start; col <= col_end; col++) {
            const term_cell_t *cell = &cells[row * cols + col];
            if (cell->attrs & CELL_ATTR_WIDE_CONT) continue;
            uint32_t cp = cell->codepoint;
            if (cp && cp != ' ') last_nonspace = col;
        }

        for (int col = col_start; col <= last_nonspace; col++) {
            const term_cell_t *cell = &cells[row * cols + col];
            if (cell->attrs & CELL_ATTR_WIDE_CONT) continue;
            uint32_t cp = cell->codepoint;
            if (!cp) cp = ' ';
            char u8[4];
            int len = utf8_encode(cp, u8);
            if (len > 0 && pos + (size_t)len < max_sz) {
                memcpy(buf + pos, u8, (size_t)len);
                pos += (size_t)len;
            }
        }

        /* 행 구분 (마지막 행 제외) */
        if (row < r1 && pos + 1 < max_sz)
            buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return buf;
}

/* ── Render pass ─────────────────────────────────────────────────────────── */

typedef struct { gl_renderer_t *render; font_face_t *font; int blink_on; } render_ctx_t;

static void render_leaf_cb(layout_node_t *leaf, void *user)
{
    render_ctx_t  *ctx = user;
    pane_slot_t   *slot = pane_slot_find(leaf->pane_id);
    if (!slot) return;

    pane_rect_t prect = { leaf->rect.x, leaf->rect.y,
                           leaf->rect.w, leaf->rect.h };

    /* 이 pane에 선택 영역이 있으면 전달, 없으면 -1 */
    int ssc = -1, ssr = -1, sec = -1, ser = -1;
    if ((g_selecting || g_has_selection) && g_sel_pane == leaf->pane_id) {
        ssc = g_sel_sc; ssr = g_sel_sr;
        sec = g_sel_ec; ser = g_sel_er;
    }
    gl_renderer_draw_cells(ctx->render, screen_get_cells(&slot->screen),
                            slot->screen.cols, slot->screen.rows, prect,
                            ssc, ssr, sec, ser);

    /* 커서 렌더링 (active pane만, 스크롤백 뷰가 아닐 때) */
    if (leaf->pane_id == g_active_pane &&
        !screen_cursor_hidden(&slot->screen) &&
        screen_scrollback_offset(&slot->screen) == 0)
    {
        int style = screen_cursor_style(&slot->screen);
        /* DECSCUSR 미설정(0)이면 config 설정을 사용.
         * 렌더러 스타일 인코딩:
         *   0/1/2 = solid block, 3/4 = underline, 5/6 = bar,
         *   7/8   = hollow block (blink/solid)
         *   홀수 = blink, 짝수 = solid */
        if (style == 0 && g_cfg_ptr) {
            int blink = g_cfg_ptr->cursor_blink;
            switch (g_cfg_ptr->cursor_style) {
            case CURSOR_BLOCK:        style = blink ? 1 : 2; break;
            case CURSOR_UNDERLINE:    style = blink ? 3 : 4; break;
            case CURSOR_BAR:          style = blink ? 5 : 6; break;
            case CURSOR_BLOCK_HOLLOW: style = blink ? 7 : 8; break;
            }
        }
        int is_blink = (style == 1 || style == 3 || style == 5 || style == 7);
        int vis = (!is_blink || ctx->blink_on) ? 1 : 0;

        /* 커서 색: 전경색 기본 */
        float cr = slot->screen.default_fg_r / 255.0f;
        float cg = slot->screen.default_fg_g / 255.0f;
        float cb = slot->screen.default_fg_b / 255.0f;

        gl_renderer_draw_cursor(ctx->render,
                                 screen_get_cells(&slot->screen),
                                 slot->screen.cols,
                                 screen_cursor_x(&slot->screen),
                                 screen_cursor_y(&slot->screen),
                                 style, vis, cr, cg, cb, prect);
    }
}

/* ── Resize all panes ────────────────────────────────────────────────────── */

/* blob 에서 복원한 트리 중 실제 존재하지 않는 pane 을 제거한다.
 * (데몬이 블롭을 업데이트하지 않은 상태에서 셸이 죽은 경우 등.) */
static int pane_in_list(uint32_t pid, const ipc_attach_pane_info_t *panes, int n) {
    for (int i = 0; i < n; i++) if (panes[i].pane_id == pid) return 1;
    return 0;
}

static void prune_dead_leaves(layout_node_t **root,
                               const ipc_attach_pane_info_t *panes, int n)
{
    if (!*root) return;
    /* leaf 를 수집 (트리 변형 중 안전) */
    uint32_t dead[64];
    int dead_n = 0;
    /* 재귀 수집 */
    layout_node_t *stack[128];
    int top = 0;
    stack[top++] = *root;
    while (top > 0 && dead_n < 64) {
        layout_node_t *node = stack[--top];
        if (!node) continue;
        if (node->type == LAYOUT_LEAF) {
            if (!pane_in_list(node->pane_id, panes, n))
                dead[dead_n++] = node->pane_id;
        } else {
            if (top < 126) {
                stack[top++] = node->children[0];
                stack[top++] = node->children[1];
            }
        }
    }
    for (int i = 0; i < dead_n; i++) {
        layout_node_t *leaf = layout_find_pane(*root, dead[i]);
        if (leaf) layout_remove(root, leaf);
    }
}

/* 세션 attach: 기존 세션에 연결 + pane_slot + layout 생성 */
static void session_attach_setup(uint32_t sid, int cols, int rows, int fw, int fh)
{
    ipc_attach_pane_info_t panes[64];
    int pane_count = 0;
    uint8_t blob[4096];
    uint16_t blob_len = 0;
    if (ipc_client_session_attach_ex(g_client, sid, panes, 64, &pane_count,
                                      blob, sizeof blob, &blob_len) != 0) {
        fprintf(stderr, "Failed to attach to session\n");
        return;
    }
    g_session_id = sid;
    if (pane_count > 0) g_window_id = panes[0].window_id;

    /* 1) pane_slot 생성 (모든 pane 에 대해) */
    for (int i = 0; i < pane_count; i++) {
        pane_slot_t *ps = pane_slot_alloc(panes[i].pane_id,
                                           panes[i].cols, panes[i].rows);
        if (ps) {
            screen_apply_theme(&ps->screen, g_theme);
            screen_set_clipboard_cb(&ps->screen, on_clipboard_set, NULL);
        }
    }

    /* 2) layout 트리 복원: blob 있으면 deserialize, 없으면 flat split fallback */
    g_layout = NULL;
    if (blob_len > 0) {
        g_layout = layout_deserialize(blob, blob_len);
        if (g_layout) prune_dead_leaves(&g_layout, panes, pane_count);
    }
    if (!g_layout && pane_count > 0) {
        int lx, ly, lw, lh;
        compute_layout_rect(&lx, &ly, &lw, &lh);
        g_layout = layout_create_leaf(panes[0].pane_id, lx, ly, lw, lh);
        for (int i = 1; i < pane_count; i++) {
            layout_node_t *cur = layout_find_pane(g_layout, panes[i-1].pane_id);
            if (cur) {
                layout_split(cur, LAYOUT_SPLIT_H, 0.5f, panes[i].pane_id);
                layout_node_t *r = cur;
                while (r->parent) r = r->parent;
                g_layout = r;
            }
        }
    }
    if (pane_count > 0) g_active_pane = panes[0].pane_id;

    if (g_layout) {
        int lx, ly, lw, lh;
        compute_layout_rect(&lx, &ly, &lw, &lh);
        layout_resize_root(g_layout, lx, ly, lw, lh);
        resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
        layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
    }
    for (int i = 0; i < pane_count; i++)
        ipc_client_pane_replay(g_client, panes[i].pane_id);

    /* 데몬 blob 동기화 (죽은 leaf 가 제거되었을 수 있음) */
    push_layout_to_daemon();
    (void)cols; (void)rows;
}

/* 신규 세션 생성 + 첫 pane */
static void session_new_setup(const char *name, int cols, int rows)
{
    if (!name || !name[0]) name = "default";
    if (ipc_client_session_create(g_client, name, &g_session_id) != 0) return;
    ipc_client_window_create(g_client, g_session_id, "1", &g_window_id);
    uint32_t first_pane = 0;
    ipc_client_pane_create(g_client, g_session_id, g_window_id,
                            (uint16_t)cols, (uint16_t)rows, &first_pane);
    pane_slot_t *first_slot = pane_slot_alloc(first_pane, cols, rows);
    if (first_slot) {
        screen_apply_theme(&first_slot->screen, g_theme);
        screen_set_clipboard_cb(&first_slot->screen, on_clipboard_set, NULL);
    }
    g_active_pane = first_pane;
    int lx, ly, lw, lh;
    compute_layout_rect(&lx, &ly, &lw, &lh);
    g_layout = layout_create_leaf(first_pane, lx, ly, lw, lh);
    push_layout_to_daemon();
}

static void resize_leaf_cb(layout_node_t *leaf, void *user)
{
    resize_ctx_t *ctx = user;
    int nc = leaf->rect.w / ctx->fw; if (nc < 1) nc = 1;
    int nr = leaf->rect.h / ctx->fh; if (nr < 1) nr = 1;
    ipc_client_pane_resize(ctx->c, ctx->sid, ctx->wid,
                            leaf->pane_id, (uint16_t)nc, (uint16_t)nr);
    pane_slot_t *slot = pane_slot_find(leaf->pane_id);
    if (slot) screen_resize(&slot->screen, nc, nr);
}

/* ── GLFW callbacks ──────────────────────────────────────────────────────── */

static void key_callback(GLFWwindow *win, int key, int scancode,
                          int action, int mods)
{
    (void)win;
    g_key_consumed = 0;

    if (action == GLFW_RELEASE) return;

    /* ESC → 설정창/컨텍스트 메뉴 닫기 */
    if (key == GLFW_KEY_ESCAPE) {
        if (g_show_context_menu) { g_show_context_menu = 0; g_key_consumed = 1; g_dirty = 1; return; }
        if (g_show_settings) { g_show_settings = 0; g_key_consumed = 1; g_dirty = 1; return; }
    }

    /* 키 입력 시 선택 해제 */
    if (g_has_selection) {
        g_has_selection = 0;
        g_sel_sc = g_sel_sr = g_sel_ec = g_sel_er = -1;
        g_dirty = 1;
    }

    /* Nuklear 오버레이 활성 시 키 입력 전달 */
    if (g_show_settings || g_show_session_picker || g_show_context_menu) {
        nk_impl_handle_key(key, scancode, action, mods);
        g_key_consumed = 1;
        return;
    }

    unsigned int mod_flags = input_glfw_mods(mods);

    /* ── 상시 alias (Ctrl+Insert / Shift+Insert for copy/paste) ── */
    if (!g_show_settings) {
        if (mod_flags == INPUT_MOD_CTRL  && key == GLFW_KEY_INSERT) {
            do_copy_selection();  g_key_consumed = 1; return;
        }
        if (mod_flags == INPUT_MOD_SHIFT && key == GLFW_KEY_INSERT) {
            do_paste_clipboard(); g_key_consumed = 1; return;
        }
    }

    /* ── 설정 토글 ── */
    if (g_cfg_ptr) {
        const keybindings_t *kb = &g_cfg_ptr->keybindings;

        if (keybind_matches(kb->preferences, mod_flags, key)) {
            g_show_settings = !g_show_settings;
            if (g_show_settings) {
                nk_impl_show_window("Settings", 1);
                nk_impl_reset_input();
            }
            g_key_consumed = 1;
            g_dirty = 1;
            return;
        }

        /* 설정 오버레이 열려있으면 터미널 키바인딩 무시 */
        if (g_show_settings) { g_key_consumed = 1; return; }

        pane_slot_t *as = pane_slot_find(g_active_pane);

        if (keybind_matches(kb->split_vertical,   mod_flags, key))
            { do_split(LAYOUT_SPLIT_H); g_key_consumed = 1; return; }
        if (keybind_matches(kb->split_horizontal, mod_flags, key))
            { do_split(LAYOUT_SPLIT_V); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_left,       mod_flags, key))
            { do_focus(0); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_right,      mod_flags, key))
            { do_focus(1); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_up,         mod_flags, key))
            { do_focus(2); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_down,       mod_flags, key))
            { do_focus(3); g_key_consumed = 1; return; }
        if (keybind_matches(kb->close_pane,       mod_flags, key))
            { do_close_pane(); g_key_consumed = 1; return; }
        if (keybind_matches(kb->copy,             mod_flags, key))
            { do_copy_selection(); g_key_consumed = 1; return; }
        if (keybind_matches(kb->paste,            mod_flags, key))
            { do_paste_clipboard(); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_left,      mod_flags, key))
            { do_resize_pane(0); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_right,     mod_flags, key))
            { do_resize_pane(1); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_down,      mod_flags, key))
            { do_resize_pane(2); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_up,        mod_flags, key))
            { do_resize_pane(3); g_key_consumed = 1; return; }

        if (as) {
            if (keybind_matches(kb->scroll_up,   mod_flags, key)) {
                screen_scroll_view(&as->screen, as->screen.rows);
                g_dirty = 1; g_key_consumed = 1; return;
            }
            if (keybind_matches(kb->scroll_down, mod_flags, key)) {
                screen_scroll_view(&as->screen, -as->screen.rows);
                g_dirty = 1; g_key_consumed = 1; return;
            }
        }
    }

    /* ── 키 입력 시 스크롤백 뷰에서 복귀 ── */
    {
        pane_slot_t *as = pane_slot_find(g_active_pane);
        if (as && screen_scrollback_offset(&as->screen) > 0) {
            screen_scroll_view_reset(&as->screen);
            g_dirty = 1;
        }
    }

    /* ── PTY 입력 (특수키) ── */
    uint8_t seq[16];
    int slen = input_key_to_bytes(key, mod_flags, seq, sizeof seq);
    if (slen > 0) {
        ipc_client_pty_input(g_client, g_active_pane, seq, (size_t)slen);
        g_key_consumed = 1;
    }
}

static void char_callback(GLFWwindow *win, unsigned int codepoint)
{
    (void)win;

    /* Nuklear 오버레이 활성 시 우선 전달 (g_key_consumed 무시) */
    if (g_show_settings || g_show_session_picker || g_show_context_menu) {
        nk_impl_handle_char(codepoint);
        return;
    }

    /* key_callback에서 이미 처리된 경우 무시 */
    if (g_key_consumed) return;

    /* UTF-8로 인코딩해서 PTY에 전송 */
    char utf8[4];
    int len = utf8_encode(codepoint, utf8);
    if (len > 0)
        ipc_client_pty_input(g_client, g_active_pane,
                             (const uint8_t *)utf8, (size_t)len);
}

static void mouse_button_callback(GLFWwindow *win, int button, int action,
                                   int mods)
{
    /* Nuklear에 항상 마우스 이벤트 전달 (메뉴 버튼 상시 활성) */
    nk_impl_handle_mouse_button(button, action, mods);

    /* Nuklear 오버레이가 열려있으면 터미널에 전달 안 함 */
    if (g_show_settings || g_show_context_menu) {
        return;
    }

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    double mx, my;
    glfwGetCursorPos(win, &mx, &my);
    unsigned int mod_flags = input_glfw_mods(mods);
    int press = (action == GLFW_PRESS);

    /* 우클릭 → 컨텍스트 메뉴 (마우스 모드가 아닐 때) */
    pane_slot_t *active_slot_pre = pane_slot_find(g_active_pane);
    int mode_pre = active_slot_pre ? screen_mouse_mode(&active_slot_pre->screen) : 0;
    if (press && button == GLFW_MOUSE_BUTTON_RIGHT && mode_pre == SCREEN_MOUSE_NONE) {
        g_show_context_menu = 1;
        /* 창 가장자리에 걸려 가려지지 않도록 flip/clamp. 메뉴 크기 = 180x250. */
        const float mw = 180.0f, mh = 250.0f;
        float x = (float)mx, y = (float)my;
        if (x + mw > (float)g_win_w) x -= mw;          /* 오른쪽 모서리 → 왼쪽으로 */
        if (y + mh > (float)g_win_h) y -= mh;          /* 아래쪽 모서리 → 위로   */
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        g_context_menu_x = x;
        g_context_menu_y = y;
        g_dirty = 1;
        nk_impl_reset_input();
        return;
    }

    /* 클릭 위치로 포커스 이동 */
    if (press && button == GLFW_MOUSE_BUTTON_LEFT) {
        click_ctx_t cctx = { (int)mx, (int)my, 0 };
        layout_each_leaf(g_layout, click_focus_cb, &cctx);
        if (cctx.found_id) { g_active_pane = cctx.found_id; g_dirty = 1; }
    }

    pane_slot_t *active_slot = pane_slot_find(g_active_pane);
    int mode = active_slot ? screen_mouse_mode(&active_slot->screen) : 0;
    int cell_x = (int)mx / fw;  /* 0-based */
    int cell_y = (int)my / fh;

    /* Ctrl + 좌클릭 누름 → OSC 8 하이퍼링크 열기 (선택/마우스 모드보다 우선).
     * 링크 조회는 pane 로컬 좌표가 필요하므로 leaf rect 로 보정. */
    if (press && button == GLFW_MOUSE_BUTTON_LEFT &&
        (mod_flags & INPUT_MOD_CTRL) && active_slot && g_layout) {
        layout_node_t *leaf = layout_find_pane(g_layout, g_active_pane);
        if (leaf) {
            int lx = ((int)mx - leaf->rect.x) / fw;
            int ly = ((int)my - leaf->rect.y) / fh;
            if (lx >= 0 && ly >= 0 &&
                lx < active_slot->screen.cols &&
                ly < active_slot->screen.rows) {
                const term_cell_t *cells = screen_get_cells(&active_slot->screen);
                uint16_t lid = cells[ly * active_slot->screen.cols + lx].link_id;
                if (lid) {
                    const char *url = screen_link_url(&active_slot->screen, lid);
                    if (url) { open_url(url); return; }
                }
            }
        }
    }

    if (mode != SCREEN_MOUSE_NONE) {
        /* 마우스 모드: PTY에 전달 (1-based) */
        uint8_t seq[32];
        int btn_vt = button;
        if (button == GLFW_MOUSE_BUTTON_RIGHT)  btn_vt = 2;
        if (button == GLFW_MOUSE_BUTTON_MIDDLE) btn_vt = 1;
        int slen = input_mouse_to_bytes(cell_x + 1, cell_y + 1, btn_vt, press,
                                         mod_flags, seq, (int)sizeof seq);
        if (slen > 0)
            ipc_client_pty_input(g_client, g_active_pane, seq, (size_t)slen);
    } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
        /* 좌클릭: 텍스트 선택 */
        if (press) {
            g_selecting = 1;
            g_has_selection = 0;
            g_sel_pane = g_active_pane;
            g_sel_sc = cell_x; g_sel_sr = cell_y;
            g_sel_ec = cell_x; g_sel_er = cell_y;
            g_dirty = 1;
        } else {
            /* 릴리즈: 선택 확정 + 클립보드 복사 */
            if (g_selecting) {
                g_selecting = 0;
                g_sel_ec = cell_x; g_sel_er = cell_y;
                /* 같은 위치 클릭이면 선택 해제 */
                if (g_sel_sc == g_sel_ec && g_sel_sr == g_sel_er) {
                    g_has_selection = 0;
                } else {
                    g_has_selection = 1;
                    pane_slot_t *ss = pane_slot_find(g_sel_pane);
                    if (ss) {
                        char *text = selection_to_text(&ss->screen,
                            g_sel_sc, g_sel_sr, g_sel_ec, g_sel_er);
                        if (text) {
                            glfwSetClipboardString(win, text);
                            free(text);
                        }
                    }
                }
                g_dirty = 1;
            }
        }
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE && press) {
        /* 중클릭 붙여넣기 */
        const char *clip = glfwGetClipboardString(win);
        pane_slot_t *s = pane_slot_find(g_active_pane);
        if (clip && s) {
            if (screen_bracketed_paste(&s->screen))
                ipc_client_pty_input(g_client, g_active_pane,
                                     (const uint8_t*)"\x1b[200~", 6);
            ipc_client_pty_input(g_client, g_active_pane,
                                 (const uint8_t*)clip, strlen(clip));
            if (screen_bracketed_paste(&s->screen))
                ipc_client_pty_input(g_client, g_active_pane,
                                     (const uint8_t*)"\x1b[201~", 6);
        }
    }
}

static void scroll_callback(GLFWwindow *win, double xoff, double yoff)
{
    (void)win;

    if (g_show_settings) {
        nk_impl_handle_scroll(xoff, yoff);
        return;
    }

    pane_slot_t *active_slot = pane_slot_find(g_active_pane);
    if (!active_slot) return;
    int mode = screen_mouse_mode(&active_slot->screen);

    if (mode != SCREEN_MOUSE_NONE) {
        int fw = font_cell_width(g_font);
        int fh = font_cell_height(g_font);
        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        int cell_x = (int)mx / fw + 1;
        int cell_y = (int)my / fh + 1;
        unsigned int mod_flags = input_glfw_mods(0);

        uint8_t seq[32];
        int btn_vt = (yoff > 0) ? 64 : 65;
        int slen = input_mouse_to_bytes(cell_x, cell_y, btn_vt, 1, mod_flags,
                                         seq, (int)sizeof seq);
        if (slen > 0)
            ipc_client_pty_input(g_client, g_active_pane, seq, (size_t)slen);
    } else {
        /* 스크롤백 */
        int delta = (yoff > 0) ? 3 : -3;
        screen_scroll_view(&active_slot->screen, delta);
        g_dirty = 1;
    }
}

static void cursor_pos_callback(GLFWwindow *win, double xpos, double ypos)
{
    (void)win;
    /* 드래그 중이면 선택 끝점 업데이트 */
    if (g_selecting) {
        int fw = font_cell_width(g_font);
        int fh = font_cell_height(g_font);
        g_sel_ec = (int)xpos / fw;
        g_sel_er = (int)ypos / fh;
        g_dirty = 1;
    }
}

static void framebuffer_size_callback(GLFWwindow *win, int width, int height)
{
    (void)win;
    if (width == g_win_w && height == g_win_h) return;
    g_win_w = width;
    g_win_h = height;
    gl_renderer_resize(g_renderer, g_win_w, g_win_h);
    if (g_layout) {
        int lx, ly, lw, lh;
        compute_layout_rect(&lx, &ly, &lw, &lh);
        layout_resize_root(g_layout, lx, ly, lw, lh);
        int fw = font_cell_width(g_font);
        int fh = font_cell_height(g_font);
        resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
        layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
    }
    g_dirty = 1;
}

static void glfw_error_callback(int error, const char *description)
{
    /* Wayland에서 opacity/icon 등 미지원 기능은 무시 (65548 = GLFW_FEATURE_UNAVAILABLE) */
    if (error == 0x1000C) return;
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

/* ── Config reload ───────────────────────────────────────────────────────── */

/* 현재 설정의 패딩을 반영한 레이아웃 루트 영역을 반환한다. */
static void compute_layout_rect(int *x, int *y, int *w, int *h)
{
    int px = g_cfg_ptr ? g_cfg_ptr->padding_x : 0;
    int py = g_cfg_ptr ? g_cfg_ptr->padding_y : 0;
    *x = px; *y = py;
    *w = g_win_w - 2 * px;
    *h = g_win_h - 2 * py;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* 현재 적용된 폰트/패딩 기억 (hot-reload 시 변경 여부 판별). */
static char  g_applied_font_family[256] = {0};
static float g_applied_font_size        = 0.0f;
static int   g_applied_padding_x        = 0;
static int   g_applied_padding_y        = 0;

static void do_config_reload(void)
{
    if (!g_cfg_path[0]) return;

    config_load_file(g_cfg_path, g_cfg_ptr);
    const char *home = getenv("HOME");
    if (g_cfg_ptr->theme_name[0] && home) {
        snprintf(g_theme_path, sizeof(g_theme_path),
                 "%s/.config/termemu/themes/%s.json",
                 home, g_cfg_ptr->theme_name);
        theme_load_file(g_theme_path, g_theme_mut_ptr);
        if (g_theme_mut_ptr->foreground == g_theme_mut_ptr->background)
            theme_defaults(g_theme_mut_ptr);
    }

    for (int i = 0; i < MAX_PANES; i++)
        if (g_panes[i].used)
            screen_update_palette(&g_panes[i].screen, g_theme);

    {
        float br = ((g_theme_mut_ptr->background >> 16) & 0xFF) / 255.0f;
        float bg = ((g_theme_mut_ptr->background >>  8) & 0xFF) / 255.0f;
        float bb = ( g_theme_mut_ptr->background        & 0xFF) / 255.0f;
        gl_renderer_set_clear_color(g_renderer, br, bg, bb);
    }

    gl_renderer_set_opacity(g_renderer, g_cfg_ptr->opacity);

    /* scrollback_lines 는 새로 생성되는 pane 에만 적용 (기존 화면은 유지). */
    if (g_cfg_ptr->scrollback_lines > 0)
        g_scrollback_lines = g_cfg_ptr->scrollback_lines;

    /* ── 폰트 핫 리로드: family 또는 size 변경 시 ────────────────────────── */
    const char *new_family = g_cfg_ptr->font_family[0] ? g_cfg_ptr->font_family
                                                         : "monospace";
    float new_size = g_cfg_ptr->font_size > 0.0f ? g_cfg_ptr->font_size : 12.0f;
    int font_changed = (strcmp(new_family, g_applied_font_family) != 0) ||
                        (new_size != g_applied_font_size);
    if (font_changed) {
        char resolved[512] = {0};
        const char *font_path = resolve_font_path(new_family, resolved,
                                                   sizeof resolved);
        font_face_t *new_font = font_face_load(font_path, new_size, 96);
        if (new_font) {
            glyph_atlas_t *new_atlas = glyph_atlas_create(1024, 1024);
            if (new_atlas) {
                gl_renderer_set_font(g_renderer, new_font, new_atlas);
                /* 이전 자원 해제 (renderer 는 이제 새 포인터 사용) */
                glyph_atlas_destroy(g_atlas);
                font_face_destroy(g_font);
                g_atlas = new_atlas;
                g_font  = new_font;
                strncpy(g_applied_font_family, new_family,
                        sizeof g_applied_font_family - 1);
                g_applied_font_size = new_size;
            } else {
                font_face_destroy(new_font);
            }
        }
    }

    /* ── 패딩 변경 시 layout 재배치 ─────────────────────────────────────── */
    int padding_changed = (g_cfg_ptr->padding_x != g_applied_padding_x) ||
                           (g_cfg_ptr->padding_y != g_applied_padding_y);
    if (font_changed || padding_changed) {
        if (g_layout) {
            int lx, ly, lw, lh;
            compute_layout_rect(&lx, &ly, &lw, &lh);
            layout_resize_root(g_layout, lx, ly, lw, lh);
            int fw = font_cell_width(g_font);
            int fh = font_cell_height(g_font);
            resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
            layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
        }
        g_applied_padding_x = g_cfg_ptr->padding_x;
        g_applied_padding_y = g_cfg_ptr->padding_y;
    }

    g_dirty = 1;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* CLI 인자 파싱 */
    const char *remote_target = NULL;
    const char *attach_name   = NULL;
    const char *export_session = NULL;
    const char *export_path    = NULL;
    const char *import_path    = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc)
            remote_target = argv[++i];
        else if (strcmp(argv[i], "--attach") == 0 && i + 1 < argc)
            attach_name = argv[++i];
        else if (strcmp(argv[i], "--export") == 0 && i + 2 < argc) {
            export_session = argv[++i];
            export_path = argv[++i];
        }
        else if (strcmp(argv[i], "--import") == 0 && i + 1 < argc)
            import_path = argv[++i];
    }

    /* --export: GUI 없이 세션 저장 후 종료 */
    if (export_session && export_path) {
        ipc_client_t *ec = ipc_client_create(NULL, NULL);
        if (ipc_client_connect(ec) != 0) {
            fprintf(stderr, "Failed to connect to daemon\n"); return 1;
        }
        ipc_session_info_t sessions[64];
        int sc = 0;
        ipc_client_session_list(ec, sessions, 64, &sc);
        uint32_t sid = 0;
        for (int i = 0; i < sc; i++)
            if (strcmp(sessions[i].name, export_session) == 0)
                { sid = sessions[i].session_id; break; }
        if (!sid) {
            fprintf(stderr, "Session '%s' not found\n", export_session);
            return 1;
        }
        session_snapshot_t snap;
        if (ipc_client_session_save(ec, sid, &snap) != 0) {
            fprintf(stderr, "Failed to get session snapshot\n"); return 1;
        }
        if (session_snapshot_save(export_path, &snap) != 0) {
            fprintf(stderr, "Failed to write %s\n", export_path); return 1;
        }
        fprintf(stderr, "Exported session '%s' to %s\n", export_session, export_path);
        ipc_client_disconnect(ec);
        ipc_client_destroy(ec);
        return 0;
    }

    termemu_config_t cfg = {0};
    config_defaults(&cfg);
    termemu_theme_t  theme = {0};
    theme_defaults(&theme);

    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_cfg_path, sizeof g_cfg_path, "%s/.config/termemu/config.json", home);
        config_load_file(g_cfg_path, &cfg);

        if (cfg.theme_name[0]) {
            snprintf(g_theme_path, sizeof g_theme_path,
                     "%s/.config/termemu/themes/%s.json", home, cfg.theme_name);
            theme_load_file(g_theme_path, &theme);
        }
    }
    if (theme.foreground == theme.background) {
        fprintf(stderr, "Warning: theme fg==bg, reverting to defaults\n");
        theme_defaults(&theme);
    }
    g_theme = &theme;
    g_cfg_ptr = &cfg;
    g_theme_mut_ptr = &theme;
    if (cfg.scrollback_lines > 0) g_scrollback_lines = cfg.scrollback_lines;

#ifndef _WIN32
    signal(SIGHUP, sighup_handler);
    signal(SIGPIPE, SIG_IGN);  /* daemon 소켓 끊김 시 kill 방지 */
#endif

    /* ── GLFW 초기화 ─────────────────────────────────────────────────────── */
    glfwSetErrorCallback(glfw_error_callback);

#ifdef __linux__
    /* WSLg 등에서 Wayland EGL 실패를 방지하기 위해 X11 우선 */
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

    if (!glfwInit()) {
        fprintf(stderr, "GLFW init failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    g_window = glfwCreateWindow(g_win_w, g_win_h, "termemu", NULL, NULL);
    if (!g_window) {
        /* transparent framebuffer 미지원 시 폴백 */
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
        g_window = glfwCreateWindow(g_win_w, g_win_h, "termemu", NULL, NULL);
    }
    if (!g_window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(g_window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "Failed to load OpenGL functions\n");
        glfwTerminate();
        return 1;
    }

    /* GL 컨텍스트 검증 */
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    if (!gl_version) {
        fprintf(stderr, "OpenGL context is invalid\n");
        glfwTerminate();
        return 1;
    }
    fprintf(stderr, "[termemu] OpenGL %s\n", gl_version);
    glfwSwapInterval(1);  /* VSync */

    /* Opacity — transparent framebuffer 방식 (Wayland 호환) */

    /* ── GLFW callbacks ──────────────────────────────────────────────────── */
    glfwSetKeyCallback(g_window, key_callback);
    glfwSetCharCallback(g_window, char_callback);
    glfwSetMouseButtonCallback(g_window, mouse_button_callback);
    glfwSetScrollCallback(g_window, scroll_callback);
    glfwSetCursorPosCallback(g_window, cursor_pos_callback);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_callback);

    /* ── Font + atlas + renderer ─────────────────────────────────────────── */
    char font_resolved[512] = {0};
    const char *font_name = cfg.font_family[0] ? cfg.font_family : "monospace";
    const char *font_path = resolve_font_path(font_name, font_resolved,
                                               sizeof font_resolved);
    float font_size = cfg.font_size > 0.0f ? cfg.font_size : 12.0f;

    g_font = font_face_load(font_path, font_size, 96);
    if (!g_font) { fprintf(stderr, "Failed to load font: %s\n", font_path); return 1; }

    g_atlas    = glyph_atlas_create(1024, 1024);
    g_renderer = gl_renderer_create(g_win_w, g_win_h, g_font, g_atlas);
    if (!g_atlas || !g_renderer) { fprintf(stderr, "Renderer init failed\n"); return 1; }

    /* 테마 배경색 + 투명도를 렌더러에 적용 */
    {
        float br = ((theme.background >> 16) & 0xFF) / 255.0f;
        float bg = ((theme.background >>  8) & 0xFF) / 255.0f;
        float bb = ( theme.background        & 0xFF) / 255.0f;
        gl_renderer_set_clear_color(g_renderer, br, bg, bb);
        gl_renderer_set_opacity(g_renderer, cfg.opacity);
    }

    /* ── Nuklear 초기화 ──────────────────────────────────────────────────── */
    g_nk_ctx = nk_impl_init(g_window, font_path, 16.0f);

    /* ── IPC connect ─────────────────────────────────────────────────────── */
    g_client = ipc_client_create(on_pty_output, NULL);
    ipc_client_set_pane_exited_cb(g_client, on_pane_exited, NULL);
    ipc_client_set_pane_split_cb(g_client, on_pane_split, NULL);

    int connect_ok;
    if (remote_target) {
        fprintf(stderr, "[termemu] connecting to %s via SSH...\n", remote_target);
        connect_ok = ipc_client_connect_remote(g_client, remote_target);
    } else {
        connect_ok = ipc_client_connect(g_client);
    }
    if (connect_ok != 0) {
        fprintf(stderr, "Failed to connect to daemon\n"); return 1;
    }

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    int cols = g_win_w / fw; if (cols < 1) cols = 80;
    int rows = g_win_h / fh; if (rows < 1) rows = 24;

    if (import_path) {
        /* ── Import 모드: JSON 파일에서 세션 복원 ── */
        session_snapshot_t snap;
        if (session_snapshot_load(import_path, &snap) != 0) {
            fprintf(stderr, "Failed to load %s\n", import_path); return 1;
        }
        fprintf(stderr, "[termemu] importing session '%s' (%d windows)\n",
                snap.name, snap.window_count);

        ipc_client_session_create(g_client, snap.name, &g_session_id);

        int total_panes = 0;
        for (int wi = 0; wi < snap.window_count; wi++) {
            snap_window_t *sw = &snap.windows[wi];
            uint32_t wid = 0;
            ipc_client_window_create(g_client, g_session_id, sw->name, &wid);
            if (wi == 0) g_window_id = wid;

            for (int pi = 0; pi < sw->pane_count; pi++) {
                snap_pane_t *sp = &sw->panes[pi];
                uint16_t pc = sp->cols > 0 ? sp->cols : (uint16_t)cols;
                uint16_t pr = sp->rows > 0 ? sp->rows : (uint16_t)rows;
                uint32_t pid = 0;
                ipc_client_pane_create(g_client, g_session_id, wid, pc, pr, &pid);

                pane_slot_t *ps = pane_slot_alloc(pid, pc, pr);
                if (ps) {
                    screen_apply_theme(&ps->screen, g_theme);
                    screen_set_clipboard_cb(&ps->screen, on_clipboard_set, NULL);
                }

                /* cwd가 있으면 cd 명령 전송 */
                if (sp->cwd[0]) {
                    char cd_cmd[SNAP_CWD_MAX + 8];
                    int cd_len = snprintf(cd_cmd, sizeof cd_cmd, "cd %s\n", sp->cwd);
                    if (cd_len > 0)
                        ipc_client_pty_input(g_client, pid,
                                             (const uint8_t *)cd_cmd, (size_t)cd_len);
                    /* clear 전송 — cd 출력 정리 */
                    ipc_client_pty_input(g_client, pid,
                                         (const uint8_t *)"clear\n", 6);
                }

                if (total_panes == 0) {
                    g_active_pane = pid;
                    int lx, ly, lw, lh;
                    compute_layout_rect(&lx, &ly, &lw, &lh);
                    g_layout = layout_create_leaf(pid, lx, ly, lw, lh);
                } else if (g_layout) {
                    layout_node_t *cur = layout_find_pane(g_layout, g_active_pane);
                    if (cur) {
                        layout_split(cur, LAYOUT_SPLIT_H, 0.5f, pid);
                        layout_node_t *r = cur;
                        while (r->parent) r = r->parent;
                        g_layout = r;
                    }
                }
                total_panes++;
            }
        }

        if (g_layout) {
            int lx, ly, lw, lh;
            compute_layout_rect(&lx, &ly, &lw, &lh);
            layout_resize_root(g_layout, lx, ly, lw, lh);
            resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
            layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
        }

        /* 초기 출력 수신 */
        ipc_client_poll(g_client, 300);
    } else if (attach_name) {
        /* ── Attach 모드: 기존 세션에 연결 ── */
        ipc_session_info_t sessions[64];
        int sess_count = 0;
        ipc_client_session_list(g_client, sessions, 64, &sess_count);

        fprintf(stderr, "[termemu] %d session(s) found\n", sess_count);
        for (int i = 0; i < sess_count; i++)
            fprintf(stderr, "  [%d] id=%u name='%s'\n",
                    i, sessions[i].session_id, sessions[i].name);

        uint32_t target_sid = 0;
        for (int i = 0; i < sess_count; i++) {
            if (strcmp(sessions[i].name, attach_name) == 0) {
                target_sid = sessions[i].session_id;
                break;
            }
        }
        if (!target_sid) {
            fprintf(stderr, "Session '%s' not found. Is the first termemu still running?\n",
                    attach_name);
            return 1;
        }

        fprintf(stderr, "[termemu] attaching to session_id=%u...\n", target_sid);
        session_attach_setup(target_sid, cols, rows, fw, fh);
        fprintf(stderr, "[termemu] attached to session '%s'\n", attach_name);
    } else {
        /* 세션 목록 조회 + 선택 다이얼로그 항상 표시 (원격도 동일) */
        ipc_client_session_list(g_client, g_picker_sessions, 64, &g_picker_count);
        g_show_session_picker = 1;
    }

    /* ── 설정 핫 리로드 감시 ────────────────────────────────────────────── */
    void *fs_watcher = fs_watch_create();
    if (fs_watcher) {
        if (g_cfg_path[0])   fs_watch_add(fs_watcher, g_cfg_path);
        if (g_theme_path[0]) fs_watch_add(fs_watcher, g_theme_path);
    }

    /* ── Event loop ──────────────────────────────────────────────────────── */
    while (!glfwWindowShouldClose(g_window) && g_running) {
        glfwPollEvents();

        /* SIGHUP 또는 inotify → config + theme 재로드 */
        if (g_sighup || (fs_watcher && fs_watch_poll(fs_watcher) > 0)) {
            g_sighup = 0;
            do_config_reload();
        }

        int poll_ms = 8;
        ipc_client_poll(g_client, poll_ms);

        /* OSC 타이틀 업데이트 */
        pane_slot_t *active_slot = pane_slot_find(g_active_pane);
        if (active_slot) {
            const char *title = screen_get_title(&active_slot->screen);
            if (title[0] && strcmp(title, g_last_title) != 0) {
                glfwSetWindowTitle(g_window, title);
                strncpy(g_last_title, title, sizeof(g_last_title) - 1);
                g_last_title[sizeof(g_last_title) - 1] = '\0';
            }
        }

        /* 렌더링 (커서 블링크를 위해 항상 렌더 시도, 16ms 캡이 제한) */
        {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            static long last_render_ms = 0;
            long now_ms = ts_now.tv_sec * 1000L + ts_now.tv_nsec / 1000000L;
            if (now_ms - last_render_ms >= 16) {
                last_render_ms = now_ms;
                g_dirty = 0;

                gl_renderer_begin_frame(g_renderer);

                if (g_layout) {
                    /* 블링크 상태: 500ms 주기 토글 */
                    int blink_on = ((now_ms / 500) % 2 == 0) ? 1 : 0;
                    render_ctx_t rctx = { g_renderer, g_font, blink_on };
                    layout_each_leaf(g_layout, render_leaf_cb, &rctx);
                }

                /* Nuklear UI — 항상 렌더링 (메뉴 버튼 상시 표시) */
                if (g_nk_ctx) {
                    nk_impl_new_frame();

                    /* ── 우클릭 컨텍스트 메뉴 (복사/붙여넣기 + 분할 + 설정) ── */
                    if (g_show_context_menu) {
                        int ctx_open = nk_begin(g_nk_ctx, "ctx_menu",
                                     nk_rect(g_context_menu_x, g_context_menu_y, 180, 250),
                                     NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR);
                        if (ctx_open) {
                            nk_layout_row_dynamic(g_nk_ctx, 28, 1);
                            if (nk_button_label(g_nk_ctx, "Copy")) {
                                /* 현재 선택 영역을 복사 */
                                if (g_has_selection) {
                                    pane_slot_t *ss = pane_slot_find(g_sel_pane);
                                    if (ss) {
                                        char *text = selection_to_text(&ss->screen,
                                            g_sel_sc, g_sel_sr, g_sel_ec, g_sel_er);
                                        if (text) {
                                            glfwSetClipboardString(g_window, text);
                                            free(text);
                                        }
                                    }
                                }
                                g_show_context_menu = 0;
                            }
                            if (nk_button_label(g_nk_ctx, "Paste")) {
                                const char *clip = glfwGetClipboardString(g_window);
                                pane_slot_t *ps = pane_slot_find(g_active_pane);
                                if (clip && ps) {
                                    if (screen_bracketed_paste(&ps->screen))
                                        ipc_client_pty_input(g_client, g_active_pane,
                                            (const uint8_t*)"\x1b[200~", 6);
                                    ipc_client_pty_input(g_client, g_active_pane,
                                        (const uint8_t*)clip, strlen(clip));
                                    if (screen_bracketed_paste(&ps->screen))
                                        ipc_client_pty_input(g_client, g_active_pane,
                                            (const uint8_t*)"\x1b[201~", 6);
                                }
                                g_show_context_menu = 0;
                            }
                            if (nk_button_label(g_nk_ctx, "Settings")) {
                                g_show_settings = 1;
                                g_show_context_menu = 0;
                                nk_impl_reset_input();
                                nk_impl_show_window("Settings", 1);
                            }
                            if (nk_button_label(g_nk_ctx, "Split Vertical")) {
                                do_split(LAYOUT_SPLIT_H);
                                g_show_context_menu = 0;
                            }
                            if (nk_button_label(g_nk_ctx, "Split Horizontal")) {
                                do_split(LAYOUT_SPLIT_V);
                                g_show_context_menu = 0;
                            }
                            if (nk_button_label(g_nk_ctx, "Close Pane")) {
                                do_close_pane();
                                g_show_context_menu = 0;
                            }
                            /* 메뉴 밖 클릭 시 닫기 — nk_begin 성공 블록 내에서만 hovered 체크 */
                            {
                                static int prev_left = 0;
                                int now_left = glfwGetMouseButton(g_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                                int edge = (!prev_left && now_left);
                                prev_left = now_left;
                                if (edge && !nk_window_is_hovered(g_nk_ctx))
                                    g_show_context_menu = 0;
                            }
                        } else {
                            g_show_context_menu = 0;  /* 윈도우 숨겨짐 */
                        }
                        nk_end(g_nk_ctx);
                    }

                    /* ── 세션 선택 다이얼로그 ── */
                    if (g_show_session_picker) {
                        float dw = 420, dh = 350;
                        float dx = ((float)g_win_w - dw) * 0.5f;
                        float dy = ((float)g_win_h - dh) * 0.5f;
                        const char *title = remote_target
                            ? "Select Session (Remote)" : "Select Session";
                        if (nk_begin(g_nk_ctx, title,
                                     nk_rect(dx, dy, dw, dh),
                                     NK_WINDOW_BORDER | NK_WINDOW_TITLE |
                                     NK_WINDOW_NO_SCROLLBAR))
                        {
                            if (remote_target) {
                                nk_layout_row_dynamic(g_nk_ctx, 20, 1);
                                char info[128];
                                snprintf(info, sizeof info, "Connected to: %s", remote_target);
                                nk_label_colored(g_nk_ctx, info, NK_TEXT_LEFT,
                                                 nk_rgb(120, 200, 255));
                            }

                            nk_layout_row_dynamic(g_nk_ctx, 25, 1);
                            if (g_picker_count == 0)
                                nk_label(g_nk_ctx, "No existing sessions.", NK_TEXT_LEFT);
                            else
                                nk_label(g_nk_ctx, "Existing sessions:", NK_TEXT_LEFT);

                            for (int i = 0; i < g_picker_count; i++) {
                                nk_layout_row_dynamic(g_nk_ctx, 28, 1);
                                char btn[128];
                                snprintf(btn, sizeof btn, "%s (%u windows)",
                                         g_picker_sessions[i].name,
                                         g_picker_sessions[i].window_count);
                                if (nk_button_label(g_nk_ctx, btn)) {
                                    uint32_t sid = g_picker_sessions[i].session_id;
                                    g_show_session_picker = 0;
                                    session_attach_setup(sid, cols, rows, fw, fh);
                                }
                            }

                            nk_layout_row_dynamic(g_nk_ctx, 20, 1);
                            nk_spacing(g_nk_ctx, 1);
                            nk_layout_row_dynamic(g_nk_ctx, 25, 1);
                            nk_label(g_nk_ctx, "New session name:", NK_TEXT_LEFT);
                            nk_layout_row_dynamic(g_nk_ctx, 30, 1);
                            nk_edit_string_zero_terminated(g_nk_ctx, NK_EDIT_FIELD,
                                g_new_session_name, sizeof g_new_session_name,
                                nk_filter_default);
                            nk_layout_row_dynamic(g_nk_ctx, 35, 2);
                            if (nk_button_label(g_nk_ctx, "Create New")) {
                                g_show_session_picker = 0;
                                session_new_setup(g_new_session_name, cols, rows);
                            }
                            if (nk_button_label(g_nk_ctx, "Cancel")) {
                                glfwSetWindowShouldClose(g_window, 1);
                            }
                        }
                        nk_end(g_nk_ctx);
                    }

                    /* ── 설정 오버레이 ── */
                    if (g_show_settings) {
                        int result = settings_ui_draw(g_nk_ctx, &cfg, &theme,
                                                       g_cfg_path, g_theme_path);
                        if (result == 1)
                            do_config_reload();
                        else if (result == -1)
                            g_show_settings = 0;
                    }

                    nk_impl_render();
                }

                glfwSwapBuffers(g_window);
            }
        }
    }

    /* ── Cleanup ─────────────────────────────────────────────────────────── */
    for (int i = 0; i < MAX_PANES; i++)
        if (g_panes[i].used) screen_destroy(&g_panes[i].screen);

    /* 세션은 데몬에서 수명 관리 (모든 클라이언트 detach 후 5분 idle → 자동 destroy).
     * 여기서 명시적으로 destroy 하지 않는다 — 재접속 가능하도록 유지. */
    ipc_client_disconnect(g_client);
    ipc_client_destroy(g_client);

    if (fs_watcher) fs_watch_destroy(fs_watcher);
    nk_impl_shutdown();
    gl_renderer_destroy(g_renderer);
    glyph_atlas_destroy(g_atlas);
    font_face_destroy(g_font);
    layout_destroy(g_layout);

    glfwDestroyWindow(g_window);
    glfwTerminate();

    return 0;
}
