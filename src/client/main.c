#define _GNU_SOURCE
/*
 * tessera client — GLFW + OpenGL 3.3 Core main loop
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
#include <locale.h>

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
#include "font_resolve.h"
#include "pane_store.h"
#include "text_select.h"
#include "client_util.h"
#include "ipc_client.h"
#include "ui/layout.h"
#include "ui/input.h"
#include "ui/nk_impl.h"
#include "ui/settings_ui.h"
#include "ui/confirm_dialog.h"
#include "ui/ui_overlay.h"
#include "ui/status_bar.h"

/* Nuklear 선언만 (NK_IMPLEMENTATION 없이) — 컨텍스트 메뉴에서 직접 사용 */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#include "nuklear.h"
#include "screen.h"
#include "../common/config.h"
#include "../common/utf8.h"
#include "../common/session_file.h"
#include "../platform/fs_watch.h"

/* ── Pane registry (→ pane_store.{c,h}) ──────────────────────────────────── */

typedef struct { ipc_client_t *c; uint32_t sid; uint32_t wid;
                 int fw; int fh; } resize_ctx_t;
static void resize_leaf_cb(layout_node_t *leaf, void *user);

/* ── Global state ────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static volatile int g_dirty   = 1;

static const tessera_theme_t *g_theme = NULL;

/*
 * 다중 window.
 *
 * g_window_id / g_active_pane / g_layout 은 "현재 활성 window" 의 라이브 상태다.
 * 이 셋을 그대로 두는 이유는 기존 100여 곳의 호출부가 활성 window 만 다루기
 * 때문 — window 전환 시 g_windows[] 슬롯에 저장했다가 되불러온다
 * (window_activate 참조).
 *
 * 비활성 window 의 pane 도 pane_slot 을 계속 유지하므로 PTY 출력을 놓치지
 * 않는다. 렌더링만 활성 window 의 layout 트리를 따른다.
 */
#define MAX_WINDOWS 32

typedef struct {
    uint32_t       id;
    char           name[64];
    layout_node_t *layout;
    uint32_t       active_pane;
    int            activity;     /* 비활성 상태에서 새 PTY 출력을 받았으면 1 */
} client_window_t;

static client_window_t g_windows[MAX_WINDOWS];
static int             g_window_count  = 0;
static int             g_active_window = 0;   /* g_windows 내 인덱스 */

static char           g_session_name[64] = {0};
static const char    *g_remote_target = NULL;   /* --remote 로 붙었으면 대상 문자열 */

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
static int      g_sel_sc = -1, g_sel_ec = -1;   /* start/end col (pane-local) */
static int64_t  g_sel_sr_li = -1, g_sel_er_li = -1; /* start/end 절대 행 인덱스(LI) */

/* 드래그 앵커 — 최초 클릭 시점에 확정된 단위 범위.
 * mode=0 cell, 1 word, 2 line. 드래그 중에는 이 앵커와 현재 마우스 위치의
 * 단위 범위를 union 하여 선택 영역을 결정한다. */
static int      g_sel_mode = 0;
static int      g_anchor_sc = 0, g_anchor_ec = 0;
static int64_t  g_anchor_sr_li = 0, g_anchor_er_li = 0;

/* 더블/트리플 클릭 감지 — 같은 셀에서 300ms 이내 반복 클릭 카운트 */
static long     g_last_click_ms = 0;
static uint32_t g_last_click_pane = 0;
static int      g_last_click_col = -1, g_last_click_row = -1;
static int      g_click_count = 0;

/* now_ms_mono / open_url → client_util.{c,h} */

/* 워드 문자 판정 — ASCII 알파넘+_ 이상, non-ASCII 는 단어로 간주 (CJK 등). */
/* 선택 지오메트리/텍스트 순수 헬퍼 → text_select.{c,h} */

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
static tessera_config_t *g_cfg_ptr = NULL;
static tessera_theme_t  *g_theme_mut_ptr = NULL;

/* key_callback에서 처리 완료 시 char_callback 무시 */
static int g_key_consumed = 0;

/* ── IME 인지 PTY 입력 스테이징 ───────────────────────────────────────────
 * 텍스트(IME 로 조합된 CJK 포함)는 GLFW CharCallback 으로 "커밋된" Unicode
 * codepoint 로 도착하고, 물리 키는 KeyCallback 으로 도착한다. 둘은 경합하면
 * 안 된다: X11/XIM IME(fcitx/ibus)가 한글 음절을 커밋할 때, 커밋을 유발한
 * 물리 키(대개 Enter)의 KeyCallback 이 CharCallback 보다 *먼저* 불린다.
 * Enter 의 "\r" 을 즉시 보내면 조합 문자보다 앞서 — 혹은 대신 — 셸에 닿는다.
 *
 * 전략: KeyCallback 은 *커밋 트리거* 키(Enter/Tab/Backspace)의 바이트를 곧장
 * 보내지 않고 스테이징한다. 이어서 CharCallback 이 같은 poll 사이클에서 비-ASCII
 * (>=U+0080) codepoint 를 주면 그것은 IME 커밋이므로 스테이징된 트리거를 버린다.
 * 아니면 glfwPollEvents() 직후 스테이징 바이트를 PTY 로 flush 한다. 모호하지 않은
 * 키(화살표/Home·End/기능키/Ctrl·Alt 조합 등)는 즉시 전송하며 스테이징하지 않아,
 * 내비게이션에 지연이 없고 절대 잘못 드롭되지 않는다.
 *
 * 알려진 한계(vanilla GLFW 3.4 가 preedit 상태를 노출하지 않아 해결 불가):
 * 진행 중인 조합을 편집/취소하는 Backspace·Escape 는 여전히 셸에 닿을 수 있다.
 * 완전한 정확성 + 조합중 표시는 IME 지원 GLFW 빌드나 XIM/IBus 브릿지가 필요하다
 * (ISSUES.md 참조). ASCII codepoint 는 트리거를 드롭하지 않으므로 "a<Enter>"
 * 같은 빠른 연타가 한 poll 에 묶여도 안전하다. */
static uint8_t g_staged_key[16];
static int     g_staged_key_len = 0;

static void flush_staged_key(void)
{
    if (g_staged_key_len > 0) {
        ipc_client_pty_input(g_client, g_active_pane,
                             g_staged_key, (size_t)g_staged_key_len);
        g_staged_key_len = 0;
    }
}

static int key_is_commit_trigger(int key)
{
    return key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER ||
           key == GLFW_KEY_TAB   || key == GLFW_KEY_BACKSPACE;
}

/* 커밋 트리거 키는 스테이징, 그 외 특수키는 즉시 PTY 전송. */
static void stage_or_send_key(const uint8_t *seq, int len, int commit_trigger)
{
    if (len <= 0) return;
    if (commit_trigger) {
        /* 한 poll 안에서 트리거가 둘 연속 들어오는 드문 경우: 앞엣것 먼저 flush. */
        if (g_staged_key_len > 0) flush_staged_key();
        if (len > (int)sizeof g_staged_key) len = (int)sizeof g_staged_key;
        memcpy(g_staged_key, seq, (size_t)len);
        g_staged_key_len = len;
    } else {
        ipc_client_pty_input(g_client, g_active_pane, seq, (size_t)len);
    }
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */

/* OSC 52 클립보드 콜백 — screen에서 호출됨 */
static void on_clipboard_set(const char *text, void *user)
{
    (void)user;
    if (g_window && text)
        glfwSetClipboardString(g_window, text);
}

/* DSR/CPR/DA 등 터미널 리플라이를 해당 pane 의 PTY stdin 으로 전달(키 입력과 동일
 * 경로). user = pane_id (uintptr_t 캐스트). */
static void on_screen_reply(const char *bytes, size_t len, void *user)
{
    uint32_t pane_id = (uint32_t)(uintptr_t)user;
    if (g_client && pane_id && bytes && len)
        ipc_client_pty_input(g_client, pane_id, (const uint8_t *)bytes, len);
}

/* 비활성 window 의 pane 이면 그 window 에 활동 표시를 세운다. */
static void mark_window_activity(uint32_t pane_id);

/* window 테이블 조작 (정의는 아래 "window 전환" 섹션). */
static int  window_index_of_pane(uint32_t pane_id);
static void window_save_active(void);
static void window_activate(int idx, int fw, int fh);
static void push_window_layout(uint32_t window_id, layout_node_t *root);

static void on_pty_output(uint32_t pane_id, const uint8_t *data, size_t len,
                           void *user)
{
    (void)user;
    pane_slot_t *s = pane_slot_find(pane_id);
    if (s) {
        screen_feed(&s->screen, data, len);
        if (!screen_sync_output(&s->screen))
            g_dirty = 1;
        /* 보고 있지 않은 window 의 출력은 상태바에 활동 표시(*)로 알린다. */
        mark_window_activity(pane_id);
    }
}

static void push_layout_to_daemon(void);
static void compute_layout_rect(int *x, int *y, int *w, int *h);
static int  status_bar_px(void);

/* 주어진 leaf 와 같은 split 의 형제 subtree 에서 첫 leaf 반환.
 * parent_pane_id 가 없을 때(세션 재접속 등)의 포커스 복귀 fallback. */
static layout_node_t *find_sibling_leaf(layout_node_t *leaf)
{
    if (!leaf || !leaf->parent) return NULL;
    layout_node_t *p   = leaf->parent;
    layout_node_t *sib = (p->children[0] == leaf) ? p->children[1]
                                                   : p->children[0];
    while (sib && sib->type != LAYOUT_LEAF) {
        sib = sib->children[0] ? sib->children[0] : sib->children[1];
    }
    return sib;
}

/* 같은 window 안에서 대체 포커스로 삼을 pane 을 고른다 (없으면 0). */
static void pick_next_pane_cb(layout_node_t *leaf, void *u)
{
    struct { uint32_t dying, pick; } *ctx = u;
    if (!ctx->pick && leaf->pane_id != ctx->dying) ctx->pick = leaf->pane_id;
}

static void on_pane_exited(uint32_t pane_id, void *user)
{
    (void)user;

    /* 죽은 pane 이 어느 window 소속인지 먼저 찾는다. 비활성 window 의 pane 도
     * 정리해야 하므로 g_layout 만 보면 안 된다(예전 버그: stale leaf 잔존). */
    int wi = window_index_of_pane(pane_id);
    if (wi < 0) {
        /* 우리가 모르는 pane — 슬롯만 정리하고 daemon 에 push 하지 않는다
         * (모르는 pane 에 대한 push 는 재전송 루프를 유발할 수 있다). */
        pane_slot_free(pane_id);
        g_dirty = 1;
        return;
    }

    window_save_active();   /* 아래에서 슬롯 트리를 직접 만지므로 동기화 */

    client_window_t *cw   = &g_windows[wi];
    int              is_active_win = (wi == g_active_window);

    /* ── 포커스 복귀 대상 선정 (같은 window 안에서만) ── */
    if (is_active_win && pane_id == g_active_pane) {
        uint32_t next = 0;
        /* 1순위: 닫힌 pane 을 만든 부모 pane 이 같은 window 에 살아있으면 그쪽 */
        pane_slot_t *dying = pane_slot_find(pane_id);
        if (dying && dying->parent_pane_id &&
            cw->layout && layout_find_pane(cw->layout, dying->parent_pane_id))
            next = dying->parent_pane_id;
        /* 2순위: layout 트리의 형제 subtree (재접속 등 parent 정보 유실 시) */
        if (!next) {
            layout_node_t *cur = layout_find_pane(cw->layout, pane_id);
            layout_node_t *sib = find_sibling_leaf(cur);
            if (sib && sib->pane_id != pane_id) next = sib->pane_id;
        }
        /* 3순위: 같은 window 의 아무 leaf. 예전엔 pane_slot 배열 전체를
         * 훑어 다른 window 의 pane 으로 포커스가 튈 수 있었다. */
        if (!next && cw->layout) {
            struct { uint32_t dying, pick; } pc = { pane_id, 0 };
            layout_each_leaf(cw->layout, pick_next_pane_cb, &pc);
            next = pc.pick;
        }
        g_active_pane = next;
        cw->active_pane = next;
    } else if (cw->active_pane == pane_id) {
        cw->active_pane = 0;   /* window_activate 가 첫 leaf 로 복구한다 */
    }

    /* 죽는 pane 에 선택 영역이 있었으면 초기화 (stale reference 제거) */
    if (g_sel_pane == pane_id) {
        g_selecting = 0;
        g_has_selection = 0;
        g_sel_sc = g_sel_ec = -1;
        g_sel_sr_li = g_sel_er_li = -1;
        g_sel_pane = 0;
    }

    /* 같은 pane_id 를 가진 모든 leaf 제거 (중복 상태 복구) */
    int removed = 0;
    for (;;) {
        layout_node_t *node = layout_find_pane(cw->layout, pane_id);
        if (!node) break;
        layout_remove(&cw->layout, node);
        removed++;
        if (!cw->layout) break;
        if (removed > 16) break;  /* 안전망 */
    }

    pane_slot_t *slot_before = pane_slot_find(pane_id);
    int slot_freed = slot_before != NULL;
    pane_slot_free(pane_id);

    int changed = (removed > 0) || slot_freed;
    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);

    if (cw->layout) {
        /* window 는 살아있다 — 이 window 만 재배치 */
        if (is_active_win) g_layout = cw->layout;
        if (changed && is_active_win) {
            resize_ctx_t rctx = { g_client, g_session_id, cw->id, fw, fh };
            layout_each_leaf(cw->layout, resize_leaf_cb, &rctx);
        }
        if (changed) push_window_layout(cw->id, cw->layout);
        g_dirty = 1;
        return;
    }

    /*
     * window 의 마지막 pane 이 죽었다 → 그 window 를 테이블에서 제거한다.
     * 예전에는 g_layout 이 비면 무조건 g_running=0 이라, 다른 window 가
     * 남아 있어도 셸에서 exit 한 번에 앱 전체가 닫혔다.
     */
    for (int i = wi; i < g_window_count - 1; i++)
        g_windows[i] = g_windows[i + 1];
    g_window_count--;

    if (g_window_count == 0) {
        /* 세션의 마지막 window 였다 — 이제서야 종료 */
        g_layout        = NULL;
        g_active_pane   = 0;
        g_active_window = 0;
        g_running       = 0;
        g_dirty         = 1;
        return;
    }

    /* 남은 window 로 이동. 활성 window 가 사라진 경우에만 전환한다. */
    if (is_active_win) {
        int next = (wi < g_window_count) ? wi : g_window_count - 1;
        g_layout        = NULL;   /* 방금 파괴된 트리 참조 제거 */
        g_active_pane   = 0;
        g_active_window = -1;     /* window_activate 가 저장 없이 새로 올리도록 */
        window_activate(next, fw, fh);
        if (g_client && g_session_id)
            ipc_client_window_focus(g_client, g_session_id, g_windows[next].id);
    } else if (g_active_window > wi) {
        g_active_window--;        /* 앞쪽이 당겨졌으므로 인덱스 보정 */
    }

    if (changed) push_layout_to_daemon();
    g_dirty = 1;
}

/* 현재 layout tree 를 직렬화해서 데몬에 업로드한다 (재접속 복원용). */
/* 지정한 window 의 트리를 데몬에 업로드한다. */
static void push_window_layout(uint32_t window_id, layout_node_t *root)
{
    if (!g_client || !root || g_session_id == 0 || window_id == 0) return;
    uint8_t blob[4096];
    int n = layout_serialize(root, blob, sizeof blob);
    if (n < 0) return;
    ipc_client_window_layout(g_client, g_session_id, window_id,
                              blob, (uint16_t)n);
}

static void push_layout_to_daemon(void)
{
    push_window_layout(g_window_id, g_layout);
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

    pane_slot_t *ns = pane_slot_alloc(new_pane_id, new_nc, new_nr, g_scrollback_lines);
    if (ns) {
        ns->parent_pane_id = parent_pane_id;
        if (g_theme) screen_apply_theme(&ns->screen, g_theme);
        screen_set_clipboard_cb(&ns->screen, on_clipboard_set, NULL);
        screen_set_reply_cb(&ns->screen, on_screen_reply, (void*)(uintptr_t)ns->pane_id);
    }

    /* 부모 pane의 클라이언트 측 screen도 새로운 크기로 리사이즈 */
    int par_nc = parent_leaf->rect.w / fw; if (par_nc < 1) par_nc = 1;
    int par_nr = parent_leaf->rect.h / fh; if (par_nr < 1) par_nr = 1;
    pane_slot_t *par_slot = pane_slot_find(parent_pane_id);
    if (par_slot) screen_resize(&par_slot->screen, par_nc, par_nr);

    g_dirty = 1;
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
        pane_slot_t *ns = pane_slot_alloc(new_pane_id, new_nc, new_nr, g_scrollback_lines);
        if (ns) {
            ns->parent_pane_id = g_active_pane;
            if (g_theme) screen_apply_theme(&ns->screen, g_theme);
            screen_set_clipboard_cb(&ns->screen, on_clipboard_set, NULL);
            screen_set_reply_cb(&ns->screen, on_screen_reply, (void*)(uintptr_t)ns->pane_id);
        }
    }
    g_active_pane = new_pane_id;
    g_dirty = 1;
    push_layout_to_daemon();
}

/* 실제 pane destroy 수행. 확인 팝업을 거치지 않으므로 직접 부르지 말고
 * do_close_pane() 을 거친다. */
static void close_pane_now(void)
{
    if (!g_layout || !g_client) return;

    layout_node_t *node = layout_find_pane(g_layout, g_active_pane);
    if (!node) return;

    uint32_t next_pane = 0;
    /* 1순위: 닫힌 pane 을 만든 부모 pane 이 살아있으면 그쪽으로 복귀 */
    pane_slot_t *dying = pane_slot_find(g_active_pane);
    if (dying && dying->parent_pane_id) {
        pane_slot_t *par = pane_slot_find(dying->parent_pane_id);
        if (par && par->pane_id != g_active_pane) next_pane = par->pane_id;
    }
    /* 2순위: layout 트리의 형제 subtree (세션 재접속 등 parent 정보 유실 시) */
    if (!next_pane) {
        layout_node_t *sib = find_sibling_leaf(node);
        if (sib && sib->pane_id != g_active_pane) next_pane = sib->pane_id;
    }
    /* 3순위: g_panes 배열 순회 fallback */
    if (!next_pane) {
        for (int i = 0; i < MAX_PANES; i++) {
            if (pane_slot_at(i)->used && pane_slot_at(i)->pane_id != g_active_pane) {
                next_pane = pane_slot_at(i)->pane_id;
                break;
            }
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

/* leaf 개수 카운트 헬퍼 */
static void count_leaf_cb(layout_node_t *leaf, void *u)
{
    (void)leaf;
    (*(int *)u)++;
}

static int layout_leaf_count(void)
{
    int n = 0;
    if (g_layout) layout_each_leaf(g_layout, count_leaf_cb, &n);
    return n;
}

/* ── window 전환 ─────────────────────────────────────────────────────────── */

/* 트리의 첫 leaf pane_id 를 집는다 (이미 값이 있으면 유지). */
static void first_leaf_cb(layout_node_t *leaf, void *u)
{
    uint32_t *out = (uint32_t *)u;
    if (*out == 0) *out = leaf->pane_id;
}

/* 트리의 모든 leaf pane_id 를 배열로 모은다. */
typedef struct { uint32_t *ids; int cap; int n; } collect_ctx_t;

static void collect_leaf_cb(layout_node_t *leaf, void *u)
{
    collect_ctx_t *c = (collect_ctx_t *)u;
    if (c->n < c->cap) c->ids[c->n++] = leaf->pane_id;
}

/* 활성 window 의 라이브 상태(g_layout/g_active_pane)를 슬롯에 되쓴다. */
static void window_save_active(void)
{
    if (g_active_window < 0 || g_active_window >= g_window_count) return;
    client_window_t *cw = &g_windows[g_active_window];
    cw->layout      = g_layout;
    cw->active_pane = g_active_pane;
}

/*
 * idx 번째 window 를 활성화한다. 슬롯의 layout 을 라이브 상태로 올린 뒤
 * 현재 창 크기에 맞춰 재배치하고 각 pane 의 PTY 크기를 갱신한다.
 */
static void window_activate(int idx, int fw, int fh)
{
    if (idx < 0 || idx >= g_window_count) return;

    client_window_t *cw = &g_windows[idx];
    g_active_window = idx;
    g_window_id     = cw->id;
    g_layout        = cw->layout;
    g_active_pane   = cw->active_pane;
    cw->activity    = 0;   /* 보고 있는 window 는 활동 표시를 지운다 */

    /* 활성 pane 이 유실됐으면 첫 leaf 로 되돌린다. */
    if (g_layout && !layout_find_pane(g_layout, g_active_pane)) {
        g_active_pane = 0;
        layout_each_leaf(g_layout, first_leaf_cb, &g_active_pane);
    }

    if (g_layout) {
        int lx, ly, lw, lh;
        compute_layout_rect(&lx, &ly, &lw, &lh);
        layout_resize_root(g_layout, lx, ly, lw, lh);
        resize_ctx_t rctx = { g_client, g_session_id, g_window_id, fw, fh };
        layout_each_leaf(g_layout, resize_leaf_cb, &rctx);
    }
    g_dirty = 1;
}

/* pane_id 가 속한 window 의 인덱스. 없으면 -1. */
static int window_index_of_pane(uint32_t pane_id)
{
    for (int i = 0; i < g_window_count; i++) {
        layout_node_t *root = (i == g_active_window) ? g_layout
                                                      : g_windows[i].layout;
        if (root && layout_find_pane(root, pane_id)) return i;
    }
    return -1;
}

static void mark_window_activity(uint32_t pane_id)
{
    int wi = window_index_of_pane(pane_id);
    if (wi >= 0 && wi != g_active_window && !g_windows[wi].activity) {
        g_windows[wi].activity = 1;
        g_dirty = 1;   /* 상태바 갱신 */
    }
}

/* 현재 window 를 저장하고 idx 번째로 전환한다. */
static void window_switch(int idx)
{
    if (idx < 0 || idx >= g_window_count || idx == g_active_window) return;
    window_save_active();
    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    window_activate(idx, fw, fh);
    if (g_client && g_session_id)
        ipc_client_window_focus(g_client, g_session_id, g_windows[idx].id);
}

static void do_window_cycle(int delta)
{
    if (g_window_count <= 1) return;
    int idx = (g_active_window + delta) % g_window_count;
    if (idx < 0) idx += g_window_count;
    window_switch(idx);
}

/* 새 window + 그 안의 첫 pane 을 만들고 즉시 전환한다. */
static void do_window_new(void)
{
    if (!g_client || g_session_id == 0) return;
    if (g_window_count >= MAX_WINDOWS) return;

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    int lx, ly, lw, lh;
    compute_layout_rect(&lx, &ly, &lw, &lh);
    int cols = lw / fw; if (cols < 1) cols = 1;
    int rows = lh / fh; if (rows < 1) rows = 1;

    char name[64];
    snprintf(name, sizeof name, "%d", g_window_count + 1);

    uint32_t wid = 0;
    if (ipc_client_window_create(g_client, g_session_id, name, &wid) != 0) return;

    uint32_t pid = 0;
    if (ipc_client_pane_create(g_client, g_session_id, wid,
                                (uint16_t)cols, (uint16_t)rows, &pid) != 0) {
        ipc_client_window_destroy(g_client, g_session_id, wid);
        return;
    }

    pane_slot_t *ps = pane_slot_alloc(pid, cols, rows, g_scrollback_lines);
    if (!ps) {
        /* pane_slot 이 없으면 화면을 못 그리므로 데몬 쪽도 되돌린다. */
        ipc_client_pane_destroy(g_client, g_session_id, wid, pid);
        ipc_client_window_destroy(g_client, g_session_id, wid);
        return;
    }
    screen_apply_theme(&ps->screen, g_theme);
    screen_set_clipboard_cb(&ps->screen, on_clipboard_set, NULL);
    screen_set_reply_cb(&ps->screen, on_screen_reply, (void*)(uintptr_t)pid);

    window_save_active();

    client_window_t *cw = &g_windows[g_window_count];
    cw->id          = wid;
    cw->layout      = layout_create_leaf(pid, lx, ly, lw, lh);
    cw->active_pane = pid;
    snprintf(cw->name, sizeof cw->name, "%s", name);
    g_window_count++;

    window_activate(g_window_count - 1, fw, fh);
    ipc_client_window_focus(g_client, g_session_id, wid);
    push_layout_to_daemon();
}

/* window 를 닫는다 (그 안의 pane 들도 함께 정리). 마지막 하나는 닫지 않는다. */
static void close_window_now(void)
{
    if (g_window_count <= 1) return;

    int idx = g_active_window;
    client_window_t *cw = &g_windows[idx];
    uint32_t wid = cw->id;

    /* 이 window 의 pane 들을 데몬에서 없애고 로컬 슬롯도 해제 */
    uint32_t dead[MAX_PANES];
    collect_ctx_t cc = { dead, MAX_PANES, 0 };
    if (cw->layout) {
        layout_each_leaf(cw->layout, collect_leaf_cb, &cc);
        for (int i = 0; i < cc.n; i++) {
            ipc_client_pane_destroy(g_client, g_session_id, wid, dead[i]);
            pane_slot_free(dead[i]);
        }
        layout_destroy(cw->layout);
        cw->layout = NULL;
    }
    ipc_client_window_destroy(g_client, g_session_id, wid);

    /* 테이블에서 제거 (뒤를 당김) */
    for (int i = idx; i < g_window_count - 1; i++)
        g_windows[i] = g_windows[i + 1];
    g_window_count--;

    /* 활성 상태를 무효화한 뒤 이웃 window 로 이동 */
    g_layout      = NULL;
    g_active_pane = 0;
    int next = (idx < g_window_count) ? idx : g_window_count - 1;
    g_active_window = -1;   /* window_activate 가 저장 없이 새로 올리도록 */

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    window_activate(next, fw, fh);
    if (g_client && g_session_id)
        ipc_client_window_focus(g_client, g_session_id, g_windows[next].id);
}

static void confirm_close_window_cb(void *user) { (void)user; close_window_now(); }

static void do_close_window(void)
{
    if (g_window_count <= 1) return;
    if (g_cfg_ptr && g_cfg_ptr->confirm_close_window) {
        char body[256];
        snprintf(body, sizeof body,
                 "window \"%s\" 의 모든 pane 이 종료됩니다.",
                 g_windows[g_active_window].name);
        confirm_dialog_open(CONFIRM_KIND_WINDOW,
            "Close Window?", body,
            &g_cfg_ptr->confirm_close_window,
            confirm_close_window_cb, NULL);
        g_dirty = 1;
        return;
    }
    close_window_now();
}

/* confirm 콜백 — 승인 시 실제 닫기 수행 */
static void confirm_close_pane_cb(void *user)    { (void)user; close_pane_now(); }
static void confirm_close_session_cb(void *user)
{
    (void)user;
    /* 세션 종료 = 앱 종료. 메인 루프가 내려오도록 플래그를 내린다. */
    if (g_window) glfwSetWindowShouldClose(g_window, 1);
    g_running = 0;
}

/* do_close_pane — 외부에서 호출되는 진입점. 확인 팝업이 필요하면 연다. */
static void do_close_pane(void)
{
    if (!g_layout || !g_client) return;
    if (!layout_find_pane(g_layout, g_active_pane)) return;
    if (confirm_dialog_is_open()) return;  /* 중첩 방지 */

    int leaves = layout_leaf_count();

    /* 이 pane 이 마지막이면 = 세션 종료 경로 */
    if (leaves <= 1) {
        if (g_cfg_ptr && g_cfg_ptr->confirm_close_session) {
            confirm_dialog_open(CONFIRM_KIND_SESSION,
                "Close Session?",
                "이것이 마지막 pane 입니다.\n"
                "닫으면 이 세션과 모든 pane 이 종료됩니다.",
                &g_cfg_ptr->confirm_close_session,
                confirm_close_session_cb, NULL);
            return;
        }
        close_pane_now();
        return;
    }

    if (g_cfg_ptr && g_cfg_ptr->confirm_close_pane) {
        pane_slot_t *dying = pane_slot_find(g_active_pane);
        const char *title = dying ? screen_get_title(&dying->screen) : "";
        char body[384];
        snprintf(body, sizeof body,
                 "현재 pane 을 닫습니다.\npane_id=%u%s%s",
                 g_active_pane,
                 (title && title[0]) ? "\n" : "",
                 (title && title[0]) ? title : "");
        confirm_dialog_open(CONFIRM_KIND_PANE,
            "Close Pane?", body,
            &g_cfg_ptr->confirm_close_pane,
            confirm_close_pane_cb, NULL);
        return;
    }
    close_pane_now();
}

/* 앱(세션) 종료 요청 — X 버튼 / 외부 close. 확인 팝업 분기. */
static void request_app_close(void)
{
    if (confirm_dialog_is_open()) return;
    /* 아직 세션이 만들어지지 않은 초기 단계(피커 취소 등)는 곧바로 종료. */
    if (g_session_id == 0 || g_window_id == 0 || !g_layout) {
        if (g_window) glfwSetWindowShouldClose(g_window, 1);
        return;
    }
    if (g_cfg_ptr && g_cfg_ptr->confirm_close_session) {
        int leaves = layout_leaf_count();
        char body[256];
        snprintf(body, sizeof body,
                 "이 세션과 모든 pane 이 종료됩니다.\n현재 pane 개수: %d",
                 leaves);
        confirm_dialog_open(CONFIRM_KIND_SESSION,
            "Close Session?", body,
            &g_cfg_ptr->confirm_close_session,
            confirm_close_session_cb, NULL);
        return;
    }
    if (g_window) glfwSetWindowShouldClose(g_window, 1);
    g_running = 0;
}

/* GLFW 창 닫기 콜백 — 확인 팝업이 필요하면 shouldClose 를 되돌린다. */
static void window_close_callback(GLFWwindow *win)
{
    /* 일단 확인을 거쳐야 하므로 close 플래그 취소. */
    glfwSetWindowShouldClose(win, 0);
    request_app_close();
}

/* ── Copy / Paste / Resize ──────────────────────────────────────────────── */

static void do_copy_selection(void)
{
    if (!g_has_selection) return;
    pane_slot_t *ss = pane_slot_find(g_sel_pane);
    if (!ss) return;
    char *text = selection_to_text(&ss->screen,
                                     g_sel_sc, g_sel_sr_li, g_sel_ec, g_sel_er_li);
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

/* keybind_matches → ui/input.{c,h} */

/* ── 선택 텍스트 추출 ────────────────────────────────────────────────────── */

/* ── Render pass ─────────────────────────────────────────────────────────── */

typedef struct { gl_renderer_t *render; font_face_t *font; int blink_on; } render_ctx_t;

static void render_leaf_cb(layout_node_t *leaf, void *user)
{
    render_ctx_t  *ctx = user;
    pane_slot_t   *slot = pane_slot_find(leaf->pane_id);
    if (!slot) return;

    pane_rect_t prect = { leaf->rect.x, leaf->rect.y,
                           leaf->rect.w, leaf->rect.h };

    /* 이 pane에 선택 영역이 있으면 LI -> 현재 프레임의 view row 로 변환해 전달.
     * 뷰포트 바깥이면 음수/rows 이상 값도 그대로 전달 — 렌더러가 일부 교차만
     * 그린다. 전체 off-view 면 sentinel(-1)로 꺼둔다. */
    int ssc = -1, ssr = -1, sec = -1, ser = -1;
    if ((g_selecting || g_has_selection) && g_sel_pane == leaf->pane_id) {
        int rows = slot->screen.rows;
        int vr_s = view_row_from_li(&slot->screen, g_sel_sr_li);
        int vr_e = view_row_from_li(&slot->screen, g_sel_er_li);
        int top = vr_s < vr_e ? vr_s : vr_e;
        int bot = vr_s < vr_e ? vr_e : vr_s;
        if (top < rows && bot >= 0) {   /* 뷰와 겹침 */
            ssc = g_sel_sc; ssr = vr_s;
            sec = g_sel_ec; ser = vr_e;
        }
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
/*
 * 특정 window 에 속한 pane 들만 추려 layout 트리를 만든다.
 * blob 이 있으면 그것을 복원하고, 없으면 flat split 으로 폴백한다.
 */
static layout_node_t *build_window_layout(const ipc_attach_result_t *ar,
                                           const ipc_client_win_t *win)
{
    layout_node_t *root = NULL;

    if (win->blob_len > 0) {
        root = layout_deserialize(ar->blobs + win->blob_off, win->blob_len);
        if (root) prune_dead_leaves(&root, ar->panes, ar->pane_count);
    }
    if (root) return root;

    /* 폴백: 이 window 의 pane 들을 순서대로 수평 분할 */
    int lx, ly, lw, lh;
    compute_layout_rect(&lx, &ly, &lw, &lh);
    uint32_t prev = 0;
    for (int i = 0; i < ar->pane_count; i++) {
        if (ar->panes[i].window_id != win->window_id) continue;
        uint32_t pid = ar->panes[i].pane_id;
        if (!root) {
            root = layout_create_leaf(pid, lx, ly, lw, lh);
        } else {
            layout_node_t *cur = layout_find_pane(root, prev);
            if (cur) {
                layout_split(cur, LAYOUT_SPLIT_H, 0.5f, pid);
                layout_node_t *r = cur;
                while (r->parent) r = r->parent;
                root = r;
            }
        }
        prev = pid;
    }
    return root;
}

static void session_attach_setup(uint32_t sid, int cols, int rows, int fw, int fh)
{
    ipc_attach_result_t ar;
    if (ipc_client_session_attach(g_client, sid, &ar) != 0) {
        fprintf(stderr, "Failed to attach to session\n");
        return;
    }
    g_session_id = sid;
    snprintf(g_session_name, sizeof g_session_name, "%s", ar.session_name);

    /* 1) pane_slot 생성 — 비활성 window 의 pane 도 PTY 출력을 계속 받아야
     *    하므로 세션의 모든 pane 에 대해 만든다. */
    for (int i = 0; i < ar.pane_count; i++) {
        pane_slot_t *ps = pane_slot_alloc(ar.panes[i].pane_id,
                                           ar.panes[i].cols, ar.panes[i].rows,
                                           g_scrollback_lines);
        if (ps) {
            screen_apply_theme(&ps->screen, g_theme);
            screen_set_clipboard_cb(&ps->screen, on_clipboard_set, NULL);
            screen_set_reply_cb(&ps->screen, on_screen_reply, (void*)(uintptr_t)ps->pane_id);
        }
    }

    /* 2) window 별 layout 트리 복원 */
    g_window_count  = 0;
    g_active_window = 0;
    for (int i = 0; i < ar.window_count && g_window_count < MAX_WINDOWS; i++) {
        layout_node_t *root = build_window_layout(&ar, &ar.windows[i]);
        if (!root) continue;   /* pane 이 하나도 없는 window 는 건너뛴다 */

        client_window_t *cw = &g_windows[g_window_count];
        cw->id = ar.windows[i].window_id;
        snprintf(cw->name, sizeof cw->name, "%s", ar.windows[i].name);
        cw->layout = root;
        /* 활성 pane 은 이 window 의 첫 leaf */
        cw->active_pane = 0;
        layout_each_leaf(root, first_leaf_cb, &cw->active_pane);

        if (ar.active_window_id && cw->id == ar.active_window_id)
            g_active_window = g_window_count;
        g_window_count++;
    }

    if (g_window_count == 0) {
        fprintf(stderr, "attach: session has no usable window\n");
        return;
    }

    window_activate(g_active_window, fw, fh);

    for (int i = 0; i < ar.pane_count; i++)
        ipc_client_pane_replay(g_client, ar.panes[i].pane_id);

    /* 데몬 blob 동기화 (죽은 leaf 가 제거되었을 수 있음) */
    push_layout_to_daemon();
    (void)cols; (void)rows;
}

/* 신규 세션 생성 + 첫 pane */
/* ── 하단 상태바 ─────────────────────────────────────────────────────────── */

/*
 * 상태바를 1행짜리 셀 그리드로 만들어 기존 cell 렌더러로 그린다.
 * 별도 셰이더 pass 없이 폰트/아틀라스 경로를 그대로 재사용한다.
 */
static void render_status_bar(void)
{
    int bar_h = status_bar_px();
    if (bar_h <= 0 || !g_renderer || !g_font) return;

    int fw = font_cell_width(g_font);
    if (fw < 1) return;
    int px   = g_cfg_ptr ? g_cfg_ptr->padding_x : 0;
    int cols = (g_win_w - 2 * px) / fw;
    if (cols < 1) return;
    if (cols > STATUS_BAR_MAX_COLS) cols = STATUS_BAR_MAX_COLS;

    status_bar_info_t info;
    memset(&info, 0, sizeof info);
    info.session_name  = g_session_name[0] ? g_session_name : NULL;
    info.active_window = g_active_window;
    info.remote        = g_remote_target ? 1 : 0;
    info.selecting     = (g_selecting || g_has_selection) ? 1 : 0;

    int wn = g_window_count;
    if (wn > STATUS_BAR_MAX_WINDOWS) wn = STATUS_BAR_MAX_WINDOWS;
    for (int i = 0; i < wn; i++) {
        info.windows[i].id       = g_windows[i].id;
        info.windows[i].activity = g_windows[i].activity;
        snprintf(info.windows[i].name, sizeof info.windows[i].name,
                 "%s", g_windows[i].name);
        int n = 0;
        layout_node_t *root = (i == g_active_window) ? g_layout
                                                      : g_windows[i].layout;
        if (root) layout_each_leaf(root, count_leaf_cb, &n);
        info.windows[i].pane_count = n;
    }
    info.window_count = wn;

    pane_slot_t *as = pane_slot_find(g_active_pane);
    if (as) {
        info.active_pane_id = g_active_pane;
        info.pane_cols      = as->screen.cols;
        info.pane_rows      = as->screen.rows;
        info.scrollback     = screen_scrollback_offset(&as->screen);
    }

    term_cell_t row[STATUS_BAR_MAX_COLS];
    status_bar_build(row, cols, &info, g_theme);

    pane_rect_t rect = { px, g_win_h - bar_h, cols * fw, bar_h };
    gl_renderer_draw_cells(g_renderer, row, cols, STATUS_BAR_ROWS, rect,
                            -1, -1, -1, -1);
}

static void session_new_setup(const char *name, int cols, int rows)
{
    if (!name || !name[0]) name = "default";
    if (ipc_client_session_create(g_client, name, &g_session_id) != 0) return;
    snprintf(g_session_name, sizeof g_session_name, "%s", name);
    ipc_client_window_create(g_client, g_session_id, "1", &g_window_id);
    uint32_t first_pane = 0;
    ipc_client_pane_create(g_client, g_session_id, g_window_id,
                            (uint16_t)cols, (uint16_t)rows, &first_pane);
    pane_slot_t *first_slot = pane_slot_alloc(first_pane, cols, rows, g_scrollback_lines);
    if (first_slot) {
        screen_apply_theme(&first_slot->screen, g_theme);
        screen_set_clipboard_cb(&first_slot->screen, on_clipboard_set, NULL);
        screen_set_reply_cb(&first_slot->screen, on_screen_reply, (void*)(uintptr_t)first_slot->pane_id);
    }
    g_active_pane = first_pane;
    int lx, ly, lw, lh;
    compute_layout_rect(&lx, &ly, &lw, &lh);
    g_layout = layout_create_leaf(first_pane, lx, ly, lw, lh);

    /* window 테이블 등록 — 이후 window 추가/전환의 기준점이 된다. */
    g_window_count  = 1;
    g_active_window = 0;
    g_windows[0].id          = g_window_id;
    g_windows[0].layout      = g_layout;
    g_windows[0].active_pane = g_active_pane;
    snprintf(g_windows[0].name, sizeof g_windows[0].name, "1");

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
        g_sel_sc = g_sel_ec = -1;
        g_sel_sr_li = g_sel_er_li = -1;
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
            if (action == GLFW_PRESS) {
                g_show_settings = !g_show_settings;
                if (g_show_settings) {
                    nk_impl_show_window("Settings", 1);
                    nk_impl_reset_input();
                }
                g_dirty = 1;
            }
            g_key_consumed = 1;
            return;
        }

        /* 설정 오버레이 열려있으면 터미널 키바인딩 무시 */
        if (g_show_settings) { g_key_consumed = 1; return; }

        pane_slot_t *as = pane_slot_find(g_active_pane);

        /* split/close/focus/copy/paste 는 edge-triggered — OS 키리피트로
         * 연속 발사되면 짧은 시간에 pane 이 쏟아져 "동기화된 것처럼" 보이므로
         * REPEAT 는 consume 만 하고 drop 한다. resize/scroll 은 아래에서 반복 허용. */
        int is_press = (action == GLFW_PRESS);
        if (keybind_matches(kb->split_vertical,   mod_flags, key))
            { if (is_press) do_split(LAYOUT_SPLIT_H); g_key_consumed = 1; return; }
        if (keybind_matches(kb->split_horizontal, mod_flags, key))
            { if (is_press) do_split(LAYOUT_SPLIT_V); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_left,       mod_flags, key))
            { if (is_press) do_focus(0); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_right,      mod_flags, key))
            { if (is_press) do_focus(1); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_up,         mod_flags, key))
            { if (is_press) do_focus(2); g_key_consumed = 1; return; }
        if (keybind_matches(kb->focus_down,       mod_flags, key))
            { if (is_press) do_focus(3); g_key_consumed = 1; return; }
        if (keybind_matches(kb->close_pane,       mod_flags, key))
            { if (is_press) do_close_pane(); g_key_consumed = 1; return; }
        if (keybind_matches(kb->copy,             mod_flags, key))
            { if (is_press) do_copy_selection(); g_key_consumed = 1; return; }
        if (keybind_matches(kb->paste,            mod_flags, key))
            { if (is_press) do_paste_clipboard(); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_left,      mod_flags, key))
            { do_resize_pane(0); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_right,     mod_flags, key))
            { do_resize_pane(1); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_down,      mod_flags, key))
            { do_resize_pane(2); g_key_consumed = 1; return; }
        if (keybind_matches(kb->resize_up,        mod_flags, key))
            { do_resize_pane(3); g_key_consumed = 1; return; }

        /* window(탭) — split 과 마찬가지로 edge-triggered */
        if (keybind_matches(kb->window_next,      mod_flags, key))
            { if (is_press) do_window_cycle(1);  g_key_consumed = 1; return; }
        if (keybind_matches(kb->window_prev,      mod_flags, key))
            { if (is_press) do_window_cycle(-1); g_key_consumed = 1; return; }
        if (keybind_matches(kb->window_new,       mod_flags, key))
            { if (is_press) do_window_new();     g_key_consumed = 1; return; }
        if (keybind_matches(kb->window_close,     mod_flags, key))
            { if (is_press) do_close_window();   g_key_consumed = 1; return; }

        /* Ctrl+Alt+1..9 → 해당 번호 window, Ctrl+Alt+0 → 10번째 (고정 바인딩) */
        if (mod_flags == (INPUT_MOD_CTRL | INPUT_MOD_ALT) &&
            key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
            if (is_press) {
                int n = (key == GLFW_KEY_0) ? 9 : (key - GLFW_KEY_1);
                window_switch(n);
            }
            g_key_consumed = 1;
            return;
        }

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

    /* ── PTY 입력 (특수키) ──
     * 커밋 트리거 키(Enter/Tab/Backspace)는 스테이징해 IME 조합 문자와의 순서
     * 경합을 피하고, 그 외 특수키는 즉시 전송한다. (위 g_staged_key 주석 참조) */
    uint8_t seq[16];
    int slen = input_key_to_bytes(key, mod_flags, seq, sizeof seq);
    if (slen > 0) {
        stage_or_send_key(seq, slen, key_is_commit_trigger(key));
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

    /* 비-ASCII codepoint = IME/CJK 커밋. key_callback 이 스테이징한 커밋 트리거
     * 키(Enter 등)는 이 텍스트를 확정하려고 IME 가 소비한 것이므로 버리고,
     * 조합된 문자만 PTY 로 보낸다. */
    if (codepoint >= 0x80) {
        g_staged_key_len = 0;  /* drop staged trigger */
        char utf8[4];
        int len = utf8_encode(codepoint, utf8);
        if (len > 0)
            ipc_client_pty_input(g_client, g_active_pane,
                                 (const uint8_t *)utf8, (size_t)len);
        return;
    }

    /* ASCII 텍스트: key_callback 이 이미 이 키의 바이트를 냈으면(Ctrl 조합 등)
     * 이중 전송 방지. 평범한 출력 가능 ASCII 는 g_key_consumed==0 이라 여기서 전송. */
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
        /* 앵커 좌표만 저장 — 화면 경계 clamp 는 렌더 시점(`ui_overlay_popup_at`)에서 수행.
         * 렌더 쪽으로 미루면 창 리사이즈/폰트 변경에도 메뉴가 자동 재위치한다. */
        g_context_menu_x = (float)mx;
        g_context_menu_y = (float)my;
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
        /* 좌클릭: 텍스트 선택. 좌표는 클릭한 pane 의 로컬 셀 좌표. */
        layout_node_t *leaf = layout_find_pane(g_layout, g_active_pane);
        int local_x = 0, local_y = 0;
        if (leaf) {
            local_x = ((int)mx - leaf->rect.x) / fw;
            local_y = ((int)my - leaf->rect.y) / fh;
            if (local_x < 0) local_x = 0;
            if (local_y < 0) local_y = 0;
            if (active_slot) {
                if (local_x >= active_slot->screen.cols) local_x = active_slot->screen.cols - 1;
                if (local_y >= active_slot->screen.rows) local_y = active_slot->screen.rows - 1;
            }
        }

        if (press) {
            /* 더블/트리플 클릭 감지 */
            long now = now_ms_mono();
            int same_cell = (g_last_click_pane == g_active_pane &&
                              g_last_click_col == local_x &&
                              g_last_click_row == local_y);
            if (same_cell && (now - g_last_click_ms) < 300) {
                g_click_count++;
                if (g_click_count > 3) g_click_count = 1;
            } else {
                g_click_count = 1;
            }
            g_last_click_ms   = now;
            g_last_click_pane = g_active_pane;
            g_last_click_col  = local_x;
            g_last_click_row  = local_y;

            g_sel_pane = g_active_pane;

            /* 클릭 시점의 LI (absolute row index) 계산 */
            int64_t li = active_slot ? li_from_view_row(&active_slot->screen, local_y) : (int64_t)local_y;

            /* 모드 결정: 1=cell, 2=word, 3=line → g_sel_mode 0/1/2 */
            int mode = (g_click_count == 3) ? 2 : (g_click_count == 2 ? 1 : 0);
            int anchor_sc = local_x, anchor_ec = local_x;
            if (active_slot)
                expand_range(&active_slot->screen, li, local_x, mode,
                              &anchor_sc, &anchor_ec);

            g_sel_mode = mode;
            g_anchor_sc = anchor_sc; g_anchor_ec = anchor_ec;
            g_anchor_sr_li = g_anchor_er_li = li;

            g_sel_sc = anchor_sc; g_sel_ec = anchor_ec;
            g_sel_sr_li = g_sel_er_li = li;

            /* word/line 모드도 드래그로 확장 가능하도록 selecting 유지.
             * 릴리즈 시점에 실제로 움직였는지 여부로 선택 확정 판단. */
            g_selecting = 1;
            g_has_selection = (mode != 0);  /* word/line 은 클릭 즉시 선택됨 */

            if (mode != 0 && active_slot) {
                /* word/line 은 클릭만으로 바로 클립보드 복사 */
                char *text = selection_to_text(&active_slot->screen,
                                                g_sel_sc, g_sel_sr_li,
                                                g_sel_ec, g_sel_er_li);
                if (text) { glfwSetClipboardString(win, text); free(text); }
            }
            g_dirty = 1;
        } else {
            /* 릴리즈: 드래그 확정. 실제 드래그는 cursor_pos_callback 에서
             * 이미 g_sel_* 에 반영됐다. 여기선 최종 상태에 따라 클립보드만 갱신. */
            if (g_selecting) {
                g_selecting = 0;
                /* cell 모드에서 같은 셀 클릭으로 끝났으면 선택 해제 */
                if (g_sel_mode == 0 &&
                    g_sel_sc == g_sel_ec && g_sel_sr_li == g_sel_er_li) {
                    g_has_selection = 0;
                } else {
                    g_has_selection = 1;
                    pane_slot_t *ss = pane_slot_find(g_sel_pane);
                    if (ss) {
                        char *text = selection_to_text(&ss->screen,
                            g_sel_sc, g_sel_sr_li, g_sel_ec, g_sel_er_li);
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
    if (!g_selecting) return;

    int fw = font_cell_width(g_font);
    int fh = font_cell_height(g_font);
    layout_node_t *leaf = layout_find_pane(g_layout, g_sel_pane);
    if (!leaf) return;
    pane_slot_t *ss = pane_slot_find(g_sel_pane);
    if (!ss) return;

    int lx = ((int)xpos - leaf->rect.x) / fw;
    int ly = ((int)ypos - leaf->rect.y) / fh;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= ss->screen.cols) lx = ss->screen.cols - 1;
    if (ly >= ss->screen.rows) ly = ss->screen.rows - 1;

    int64_t mouse_li = li_from_view_row(&ss->screen, ly);

    /* 마우스 위치의 단위 범위를 mode 에 맞춰 확장 */
    int mouse_sc, mouse_ec;
    expand_range(&ss->screen, mouse_li, lx, g_sel_mode, &mouse_sc, &mouse_ec);

    /* 앵커와 mouse 의 범위를 union — 시작은 더 이른 쪽, 끝은 더 늦은 쪽.
     * word/line 모드에서 마우스가 앵커를 지나치면 끝이 확장, 앵커 반대편으로
     * 넘어가면 반대 방향으로 확장. */
    int64_t sr_li, er_li;
    int sc, ec;
    if (pos_cmp(g_anchor_sr_li, g_anchor_sc, mouse_li, mouse_sc) <= 0) {
        sr_li = g_anchor_sr_li; sc = g_anchor_sc;
    } else {
        sr_li = mouse_li; sc = mouse_sc;
    }
    if (pos_cmp(g_anchor_er_li, g_anchor_ec, mouse_li, mouse_ec) >= 0) {
        er_li = g_anchor_er_li; ec = g_anchor_ec;
    } else {
        er_li = mouse_li; ec = mouse_ec;
    }

    g_sel_sc = sc; g_sel_sr_li = sr_li;
    g_sel_ec = ec; g_sel_er_li = er_li;
    g_has_selection = 1;
    g_dirty = 1;
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
/* 상태바가 차지하는 픽셀 높이 (숨김이면 0). */
static int status_bar_px(void)
{
    if (!g_cfg_ptr || !g_cfg_ptr->statusbar_show) return 0;
    if (!g_font) return 0;
    return font_cell_height(g_font) * STATUS_BAR_ROWS;
}

static void compute_layout_rect(int *x, int *y, int *w, int *h)
{
    int px = g_cfg_ptr ? g_cfg_ptr->padding_x : 0;
    int py = g_cfg_ptr ? g_cfg_ptr->padding_y : 0;
    *x = px; *y = py;
    *w = g_win_w - 2 * px;
    /* 하단 상태바 영역을 pane 에서 제외한다 — 겹치지 않고, PTY 크기도
     * 자연스럽게 줄어든 높이로 협상된다. */
    *h = g_win_h - 2 * py - status_bar_px();
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
                 "%s/.config/tessera/themes/%s.json",
                 home, g_cfg_ptr->theme_name);
        theme_load_file(g_theme_path, g_theme_mut_ptr);
        if (g_theme_mut_ptr->foreground == g_theme_mut_ptr->background)
            theme_defaults(g_theme_mut_ptr);
    }

    for (int i = 0; i < MAX_PANES; i++)
        if (pane_slot_at(i)->used)
            screen_update_palette(&pane_slot_at(i)->screen, g_theme);

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
            add_cjk_fallbacks(new_font, font_path);
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
    /* 프로세스 로케일을 환경(LANG/LC_*)에 맞춘다. X11 XIM(fcitx/ibus)은 이
     * 호출이 있어야 UTF-8 로케일을 인지해 GLFW 가 input context 를 만들 수 있다.
     * 이게 없으면 기본 "C" 로케일이라 IME 가 아예 물리지 않아 한/영 전환해도
     * ASCII 만 입력된다. glfwInit() 보다 먼저 호출해야 한다. */
    setlocale(LC_ALL, "");

    /* CLI 인자 파싱 */
    const char *remote_target = NULL;
    const char *attach_name   = NULL;
    const char *export_session = NULL;
    const char *export_path    = NULL;
    const char *import_path    = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc)
            remote_target = g_remote_target = argv[++i];
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

    tessera_config_t cfg = {0};
    config_defaults(&cfg);
    tessera_theme_t  theme = {0};
    theme_defaults(&theme);

    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_cfg_path, sizeof g_cfg_path, "%s/.config/tessera/config.json", home);
        config_load_file(g_cfg_path, &cfg);

        if (cfg.theme_name[0]) {
            snprintf(g_theme_path, sizeof g_theme_path,
                     "%s/.config/tessera/themes/%s.json", home, cfg.theme_name);
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

    g_window = glfwCreateWindow(g_win_w, g_win_h, "tessera", NULL, NULL);
    if (!g_window) {
        /* transparent framebuffer 미지원 시 폴백 */
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
        g_window = glfwCreateWindow(g_win_w, g_win_h, "tessera", NULL, NULL);
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
    fprintf(stderr, "[tessera] OpenGL %s\n", gl_version);
    glfwSwapInterval(1);  /* VSync */

    /* Opacity — transparent framebuffer 방식 (Wayland 호환) */

    /* ── GLFW callbacks ──────────────────────────────────────────────────── */
    glfwSetKeyCallback(g_window, key_callback);
    glfwSetCharCallback(g_window, char_callback);
    glfwSetMouseButtonCallback(g_window, mouse_button_callback);
    glfwSetScrollCallback(g_window, scroll_callback);
    glfwSetCursorPosCallback(g_window, cursor_pos_callback);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_callback);
    glfwSetWindowCloseCallback(g_window, window_close_callback);

    /* ── Font + atlas + renderer ─────────────────────────────────────────── */
    char font_resolved[512] = {0};
    const char *font_name = cfg.font_family[0] ? cfg.font_family : "monospace";
    const char *font_path = resolve_font_path(font_name, font_resolved,
                                               sizeof font_resolved);
    float font_size = cfg.font_size > 0.0f ? cfg.font_size : 12.0f;

    g_font = font_face_load(font_path, font_size, 96);
    if (!g_font) { fprintf(stderr, "Failed to load font: %s\n", font_path); return 1; }
    add_cjk_fallbacks(g_font, font_path);

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
        fprintf(stderr, "[tessera] connecting to %s via SSH...\n", remote_target);
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
        fprintf(stderr, "[tessera] importing session '%s' (%d windows)\n",
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

                pane_slot_t *ps = pane_slot_alloc(pid, pc, pr, g_scrollback_lines);
                if (ps) {
                    screen_apply_theme(&ps->screen, g_theme);
                    screen_set_clipboard_cb(&ps->screen, on_clipboard_set, NULL);
                    screen_set_reply_cb(&ps->screen, on_screen_reply, (void*)(uintptr_t)ps->pane_id);
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

        fprintf(stderr, "[tessera] %d session(s) found\n", sess_count);
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
            fprintf(stderr, "Session '%s' not found. Is the first tessera still running?\n",
                    attach_name);
            return 1;
        }

        fprintf(stderr, "[tessera] attaching to session_id=%u...\n", target_sid);
        session_attach_setup(target_sid, cols, rows, fw, fh);
        fprintf(stderr, "[tessera] attached to session '%s'\n", attach_name);
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

        /* key_callback 이 스테이징한 커밋 트리거 키 flush — 같은 poll 에서 IME
         * 커밋(비-ASCII char)이 뒤따르지 않았을 때만 실제 PTY 로 전송된다. */
        flush_staged_key();

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

                render_status_bar();

                /* Nuklear UI — 항상 렌더링 (메뉴 버튼 상시 표시) */
                if (g_nk_ctx) {
                    nk_impl_new_frame();

                    /* ── 우클릭 컨텍스트 메뉴 (복사/붙여넣기 + 분할 + 설정) ── */
                    if (g_show_context_menu) {
                        /* 버튼 수 × 행 높이 기반으로 메뉴 높이를 동적 계산.
                         * 폰트 크기/라벨 길이 변화 + 화면 경계 clamp 자동. */
                        const int   btn_count = 6;
                        const float btn_h     = 28.0f;
                        float spacing_y = g_nk_ctx->style.window.spacing.y;
                        float wp_y      = g_nk_ctx->style.window.padding.y;
                        float font_h    = (g_nk_ctx->style.font &&
                                            g_nk_ctx->style.font->height > 0)
                                           ? g_nk_ctx->style.font->height : 16.0f;
                        float header_h  = font_h
                                        + 2.0f * g_nk_ctx->style.window.header.label_padding.y
                                        + 2.0f * g_nk_ctx->style.window.header.padding.y;
                        if (header_h < 28.0f) header_h = 28.0f;
                        float menu_w = 220.0f;
                        float menu_h = header_h + 4.0f /* border */ + 2.0f * wp_y
                                     + (float)btn_count * btn_h
                                     + (float)(btn_count - 1) * spacing_y
                                     + 6.0f;

                        int fbw_m, fbh_m;
                        glfwGetFramebufferSize(g_window, &fbw_m, &fbh_m);
                        float cx, cy, cw, ch;
                        ui_overlay_popup_at(g_context_menu_x, g_context_menu_y,
                                             menu_w, menu_h,
                                             (float)fbw_m, (float)fbh_m,
                                             &cx, &cy, &cw, &ch);

                        int ctx_open = nk_begin(g_nk_ctx, "ctx_menu",
                                     nk_rect(cx, cy, cw, ch),
                                     NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR);
                        if (ctx_open) {
                            nk_layout_row_dynamic(g_nk_ctx, 28, 1);
                            if (nk_button_label(g_nk_ctx, "Copy")) {
                                /* 현재 선택 영역을 복사 */
                                if (g_has_selection) {
                                    pane_slot_t *ss = pane_slot_find(g_sel_pane);
                                    if (ss) {
                                        char *text = selection_to_text(&ss->screen,
                                            g_sel_sc, g_sel_sr_li, g_sel_ec, g_sel_er_li);
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
                        int fbw_s, fbh_s;
                        glfwGetFramebufferSize(g_window, &fbw_s, &fbh_s);
                        int result = settings_ui_draw(g_nk_ctx, &cfg, &theme,
                                                       g_cfg_path, g_theme_path,
                                                       (float)fbw_s, (float)fbh_s);
                        if (result == 1)
                            do_config_reload();
                        else if (result == -1)
                            g_show_settings = 0;
                    }

                    /* ── 닫기 확인 모달 ── */
                    {
                        int fbw, fbh;
                        glfwGetFramebufferSize(g_window, &fbw, &fbh);
                        confirm_dialog_draw(g_nk_ctx, g_window, fbw, fbh);
                    }

                    nk_impl_render();
                }

                glfwSwapBuffers(g_window);
            }
        }
    }

    /* ── Cleanup ─────────────────────────────────────────────────────────── */
    for (int i = 0; i < MAX_PANES; i++)
        if (pane_slot_at(i)->used) screen_destroy(&pane_slot_at(i)->screen);

    /* 세션은 데몬에서 수명 관리 (모든 클라이언트 detach 후 5분 idle → 자동 destroy).
     * 여기서 명시적으로 destroy 하지 않는다 — 재접속 가능하도록 유지. */
    ipc_client_disconnect(g_client);
    ipc_client_destroy(g_client);

    if (fs_watcher) fs_watch_destroy(fs_watcher);
    nk_impl_shutdown();
    gl_renderer_destroy(g_renderer);
    glyph_atlas_destroy(g_atlas);
    font_face_destroy(g_font);
    /* window 별 layout 트리를 모두 해제. 활성 window 의 트리는 g_layout 과
     * 같은 객체이므로 슬롯에 되쓴 뒤 한 번씩만 지운다. */
    window_save_active();
    for (int i = 0; i < g_window_count; i++)
        layout_destroy(g_windows[i].layout);
    if (g_window_count == 0) layout_destroy(g_layout);

    glfwDestroyWindow(g_window);
    glfwTerminate();

    return 0;
}
