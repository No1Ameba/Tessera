#include "status_bar.h"

#include <stdio.h>

/* ── 색상 헬퍼 ───────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t attrs;
} sb_style_t;

static sb_style_t style_of(uint32_t fg, uint32_t bg, uint8_t attrs)
{
    sb_style_t s;
    s.fg_r = (uint8_t)((fg >> 16) & 0xFF);
    s.fg_g = (uint8_t)((fg >>  8) & 0xFF);
    s.fg_b = (uint8_t)( fg        & 0xFF);
    s.bg_r = (uint8_t)((bg >> 16) & 0xFF);
    s.bg_g = (uint8_t)((bg >>  8) & 0xFF);
    s.bg_b = (uint8_t)( bg        & 0xFF);
    s.attrs = attrs;
    return s;
}

/* ── 행 빌더 ─────────────────────────────────────────────────────────────── */

/*
 * 한 행을 왼쪽부터 채워 나가는 커서.
 * 항상 cap 을 넘지 않으므로 폭이 좁아도 오버런하지 않는다.
 */
typedef struct {
    term_cell_t *cells;
    int          cap;
    int          n;      /* 다음 쓸 열 */
} sb_writer_t;

static void sb_put(sb_writer_t *w, uint32_t cp, sb_style_t st)
{
    if (w->n >= w->cap) return;
    term_cell_t *c = &w->cells[w->n++];
    c->codepoint = cp;
    c->fg_r = st.fg_r; c->fg_g = st.fg_g; c->fg_b = st.fg_b;
    c->bg_r = st.bg_r; c->bg_g = st.bg_g; c->bg_b = st.bg_b;
    c->attrs   = st.attrs;
    c->link_id = 0;
}

/*
 * ASCII 문자열을 그대로 기록한다.
 * 상태바 텍스트는 세션/window 이름을 제외하면 전부 ASCII 이고, 이름에 들어온
 * 멀티바이트는 바이트 단위로 쪼개면 깨지므로 UTF-8 을 디코딩해 코드포인트로 넣는다.
 */
static void sb_puts(sb_writer_t *w, const char *s, sb_style_t st)
{
    if (!s) return;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && w->n < w->cap) {
        uint32_t cp = *p;
        int extra = 0;
        if      (cp < 0x80) { extra = 0; }
        else if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; extra = 1; }
        else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; extra = 2; }
        else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; extra = 3; }
        else { cp = 0xFFFD; extra = 0; }
        p++;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        sb_put(w, cp, st);
    }
}

/* 남은 칸을 배경색 공백으로 채운다. */
static void sb_fill_rest(sb_writer_t *w, sb_style_t st)
{
    while (w->n < w->cap) sb_put(w, ' ', st);
}

/* 문자열이 차지할 열 수 (UTF-8 코드포인트 개수 기준, wide 는 고려하지 않음). */
static int sb_width(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

/* ── 공개 API ────────────────────────────────────────────────────────────── */

void status_bar_build(term_cell_t *cells, int cols,
                       const status_bar_info_t *info,
                       const tessera_theme_t *theme)
{
    if (!cells || !info || !theme || cols <= 0) return;
    if (cols > STATUS_BAR_MAX_COLS) cols = STATUS_BAR_MAX_COLS;

    sb_style_t base   = style_of(theme->statusbar_fg, theme->statusbar_bg, 0);
    sb_style_t active = style_of(theme->statusbar_active_fg,
                                 theme->statusbar_active_bg, CELL_ATTR_BOLD);

    /* 우선 전체를 기본 배경으로 깔아둔다 (짧게 끝나도 배경이 이어지도록). */
    sb_writer_t w = { cells, cols, 0 };
    sb_fill_rest(&w, base);
    w.n = 0;

    /* ── 오른쪽 블록을 먼저 만들어 폭을 확보한다 ── */
    char right[128];
    int  rn = 0;
    right[0] = '\0';
    if (info->scrollback > 0)
        rn += snprintf(right + rn, sizeof(right) - (size_t)rn,
                       "[SCROLL %d] ", info->scrollback);
    if (rn < (int)sizeof(right) && info->selecting)
        rn += snprintf(right + rn, sizeof(right) - (size_t)rn, "[SELECT] ");
    if (rn < (int)sizeof(right) && info->remote)
        rn += snprintf(right + rn, sizeof(right) - (size_t)rn, "[REMOTE] ");
    if (rn < (int)sizeof(right) && info->active_pane_id)
        snprintf(right + rn, sizeof(right) - (size_t)rn,
                 "pane %u  %dx%d ", info->active_pane_id,
                 info->pane_cols, info->pane_rows);

    int right_w = sb_width(right);
    /* 오른쪽 블록은 전체의 절반을 넘지 않을 때만 표시한다. */
    if (right_w > cols / 2) { right[0] = '\0'; right_w = 0; }

    int left_cap = cols - right_w;
    if (left_cap < 0) left_cap = 0;

    /* ── 왼쪽: 세션명 + window 목록 ── */
    w.cap = left_cap;

    if (info->session_name && info->session_name[0]) {
        sb_put(&w, ' ', active);
        sb_puts(&w, info->session_name, active);
        sb_put(&w, ' ', active);
        sb_put(&w, ' ', base);
    }

    for (int i = 0; i < info->window_count; i++) {
        const status_bar_window_t *win = &info->windows[i];
        int is_active = (i == info->active_window);
        sb_style_t st = is_active ? active : base;

        char seg[96];
        /* "1:name" — 이름이 없으면 번호만. 비활성 window 의 새 출력은 '*'. */
        if (win->name[0])
            snprintf(seg, sizeof(seg), " %d:%s%s ", i + 1, win->name,
                     (!is_active && win->activity) ? "*" : "");
        else
            snprintf(seg, sizeof(seg), " %d%s ", i + 1,
                     (!is_active && win->activity) ? "*" : "");

        /* 잘려서 반쪽만 보이느니 통째로 생략한다. */
        if (w.n + sb_width(seg) > w.cap) break;
        sb_puts(&w, seg, st);
    }

    /* 왼쪽 블록 나머지를 기본 배경으로 */
    sb_fill_rest(&w, base);

    /* ── 오른쪽 블록을 끝에 붙인다 ── */
    if (right_w > 0) {
        w.cap = cols;
        w.n   = cols - right_w;
        sb_puts(&w, right, base);
        sb_fill_rest(&w, base);
    }
}
