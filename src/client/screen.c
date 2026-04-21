#include "screen.h"
#include "../common/utf8.h"
#include "../common/config.h"
#include <stdlib.h>
#include <string.h>

/* ── 색상 테이블 ─────────────────────────────────────────────────────────── */

/* xterm 호환 16-color ANSI 팔레트 */
static const uint8_t ANSI16[16][3] = {
    {  0,  0,  0}, {170,  0,  0}, {  0,170,  0}, {170,170,  0},
    {  0,  0,170}, {170,  0,170}, {  0,170,170}, {170,170,170},
    { 85, 85, 85}, {255, 85, 85}, { 85,255, 85}, {255,255, 85},
    { 85, 85,255}, {255, 85,255}, { 85,255,255}, {255,255,255},
};

static void color256(const screen_t *s, int n, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (n < 0)   n = 0;
    if (n > 255) n = 255;
    if (n < 16) {
        *r = s->ansi_palette[n][0]; *g = s->ansi_palette[n][1]; *b = s->ansi_palette[n][2];
    } else if (n < 232) {
        int v = n - 16;
        *r = (uint8_t)((v / 36)      * 51);
        *g = (uint8_t)(((v / 6) % 6) * 51);
        *b = (uint8_t)((v % 6)       * 51);
    } else {
        uint8_t g8 = (uint8_t)(8 + (n - 232) * 10);
        *r = *g = *b = g8;
    }
}

/* ── 내부 헬퍼 ───────────────────────────────────────────────────────────── */

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return imin(imax(v,lo),hi); }

/* 현재 SGR 상태로 빈 셀 하나를 반환 */
static term_cell_t blank_cell(const screen_t *s)
{
    term_cell_t c = {0};
    c.fg_r = s->fg_r; c.fg_g = s->fg_g; c.fg_b = s->fg_b;
    c.bg_r = s->bg_r; c.bg_g = s->bg_g; c.bg_b = s->bg_b;
    c.attrs = s->attrs;
    return c;
}

/* 지정 행을 현재 배경색 공백으로 지운다 */
static void erase_row(screen_t *s, int row)
{
    term_cell_t blank = blank_cell(s);
    term_cell_t *base = s->cells + row * s->cols;
    for (int c = 0; c < s->cols; c++) base[c] = blank;
}

/* ── 스크롤 ──────────────────────────────────────────────────────────────── */

/* 스크롤 영역을 n 줄 위로 밀어올린다 */
static void scroll_up_n(screen_t *s, int n)
{
    if (n <= 0) return;
    int top = s->scroll_top, bot = s->scroll_bot;
    int region = bot - top + 1;
    if (n > region) n = region;

    /* 메인 화면에서만 스크롤백 저장 */
    if (!s->use_alt && s->scrollback_max > 0) {
        for (int i = 0; i < n; i++) {
            memcpy(s->scrollback + s->sb_head * s->cols,
                   s->cells + (top + i) * s->cols,
                   (size_t)s->cols * sizeof(term_cell_t));
            s->sb_head = (s->sb_head + 1) % s->scrollback_max;
            if (s->sb_count < s->scrollback_max) s->sb_count++;
        }
    }

    if (n < region) {
        memmove(s->cells + top * s->cols,
                s->cells + (top + n) * s->cols,
                (size_t)(region - n) * s->cols * sizeof(term_cell_t));
    }
    for (int r = bot - n + 1; r <= bot; r++) erase_row(s, r);
}

/* 스크롤 영역을 n 줄 아래로 밀어내린다 (reverse scroll) */
static void scroll_down_n(screen_t *s, int n)
{
    if (n <= 0) return;
    int top = s->scroll_top, bot = s->scroll_bot;
    int region = bot - top + 1;
    if (n > region) n = region;

    if (n < region) {
        memmove(s->cells + (top + n) * s->cols,
                s->cells + top * s->cols,
                (size_t)(region - n) * s->cols * sizeof(term_cell_t));
    }
    for (int r = top; r < top + n; r++) erase_row(s, r);
}

/* ── 커서 이동 ───────────────────────────────────────────────────────────── */

static void cur_up(screen_t *s, int n)
{
    s->cy = imax(s->scroll_top, s->cy - n);
}
static void cur_down(screen_t *s, int n)
{
    s->cy = imin(s->scroll_bot, s->cy + n);
}
static void cur_left(screen_t *s, int n)
{
    s->cx = imax(0, s->cx - n);
}
static void cur_right(screen_t *s, int n)
{
    s->cx = imin(s->cols - 1, s->cx + n);
}

/* 커서 y가 스크롤 영역 아래이면 스크롤 */
static void newline(screen_t *s)
{
    if (s->cy < s->scroll_top || s->cy > s->scroll_bot) {
        /* 스크롤 영역 밖: 그냥 이동 */
        if (s->cy < s->rows - 1) s->cy++;
    } else if (s->cy < s->scroll_bot) {
        s->cy++;
    } else {
        scroll_up_n(s, 1);
        /* cy는 scroll_bot 그대로 */
    }
}

/* ── 문자 출력 ───────────────────────────────────────────────────────────── */

static void print_char(screen_t *s, uint32_t cp)
{
    /* pending_wrap: 이전에 줄 끝에서 출력하여 다음 문자 대기 상태 */
    if (s->pending_wrap) {
        s->cx = 0;
        s->pending_wrap = 0;
        newline(s);
    }

    int w = utf8_char_width(cp);
    if (w <= 0) w = 1;  /* combining/control → 1칸 처리 */

    /* wide char가 줄 끝 한 칸에 들어갈 수 없으면 그 칸을 공백으로 하고 줄바꿈 */
    if (w == 2 && s->cx == s->cols - 1) {
        term_cell_t bl = blank_cell(s);
        s->cells[s->cy * s->cols + s->cx] = bl;
        s->cx = 0;
        newline(s);
    }

    /* 선두 셀 기록 */
    term_cell_t *cell = &s->cells[s->cy * s->cols + s->cx];
    uint8_t link_attr = s->active_link_id ? CELL_ATTR_UNDERLINE : 0;
    cell->codepoint = cp;
    cell->fg_r = s->fg_r; cell->fg_g = s->fg_g; cell->fg_b = s->fg_b;
    cell->bg_r = s->bg_r; cell->bg_g = s->bg_g; cell->bg_b = s->bg_b;
    cell->attrs = s->attrs | (w == 2 ? CELL_ATTR_WIDE : 0) | link_attr;
    cell->link_id = s->active_link_id;

    /* wide char: 다음 칸을 continuation 셀로 표시 */
    if (w == 2 && s->cx + 1 < s->cols) {
        term_cell_t *cont = &s->cells[s->cy * s->cols + s->cx + 1];
        cont->codepoint = 0;
        cont->fg_r = s->fg_r; cont->fg_g = s->fg_g; cont->fg_b = s->fg_b;
        cont->bg_r = s->bg_r; cont->bg_g = s->bg_g; cont->bg_b = s->bg_b;
        cont->attrs = CELL_ATTR_WIDE_CONT;
        cont->link_id = s->active_link_id;
    }

    int next = s->cx + w;
    if (next < s->cols) {
        s->cx = next;
    } else {
        s->pending_wrap = 1;
        s->cx = s->cols - 1;
    }
}

/* ── 지우기 ──────────────────────────────────────────────────────────────── */

static void erase_display(screen_t *s, int mode)
{
    term_cell_t blank = blank_cell(s);
    switch (mode) {
    case 0: /* 커서 이하 지우기 */
        for (int c = s->cx; c < s->cols; c++)
            s->cells[s->cy * s->cols + c] = blank;
        for (int r = s->cy + 1; r < s->rows; r++)
            for (int c = 0; c < s->cols; c++)
                s->cells[r * s->cols + c] = blank;
        break;
    case 1: /* 커서 이상 지우기 */
        for (int r = 0; r < s->cy; r++)
            for (int c = 0; c < s->cols; c++)
                s->cells[r * s->cols + c] = blank;
        for (int c = 0; c <= s->cx; c++)
            s->cells[s->cy * s->cols + c] = blank;
        break;
    case 2: /* 전체 지우기 */
    case 3:
        for (int i = 0; i < s->rows * s->cols; i++)
            s->cells[i] = blank;
        break;
    }
}

static void erase_line(screen_t *s, int mode)
{
    term_cell_t blank = blank_cell(s);
    switch (mode) {
    case 0: /* 커서 오른쪽 */
        for (int c = s->cx; c < s->cols; c++)
            s->cells[s->cy * s->cols + c] = blank;
        break;
    case 1: /* 커서 왼쪽 */
        for (int c = 0; c <= s->cx; c++)
            s->cells[s->cy * s->cols + c] = blank;
        break;
    case 2: /* 줄 전체 */
        for (int c = 0; c < s->cols; c++)
            s->cells[s->cy * s->cols + c] = blank;
        break;
    }
}

static void erase_chars(screen_t *s, int n)
{
    term_cell_t blank = blank_cell(s);
    int end = imin(s->cx + n, s->cols);
    for (int c = s->cx; c < end; c++)
        s->cells[s->cy * s->cols + c] = blank;
}

/* ── 줄 삽입/삭제 ────────────────────────────────────────────────────────── */

static void insert_lines(screen_t *s, int n)
{
    /* 커서 위치부터 scroll_bot까지 n줄 아래로 밀기 */
    int saved_top = s->scroll_top;
    s->scroll_top = s->cy;
    scroll_down_n(s, n);
    s->scroll_top = saved_top;
}

static void delete_lines(screen_t *s, int n)
{
    int saved_top = s->scroll_top;
    s->scroll_top = s->cy;
    scroll_up_n(s, n);
    s->scroll_top = saved_top;
}

static void delete_chars(screen_t *s, int n)
{
    int row = s->cy;
    int remaining = s->cols - s->cx - n;
    if (remaining > 0)
        memmove(&s->cells[row * s->cols + s->cx],
                &s->cells[row * s->cols + s->cx + n],
                (size_t)remaining * sizeof(term_cell_t));
    term_cell_t blank = blank_cell(s);
    int fill_start = imax(s->cx, s->cols - n);
    for (int c = fill_start; c < s->cols; c++)
        s->cells[row * s->cols + c] = blank;
}

/* ── SGR 파서 ────────────────────────────────────────────────────────────── */

static void reset_sgr(screen_t *s)
{
    s->fg_r = s->default_fg_r;
    s->fg_g = s->default_fg_g;
    s->fg_b = s->default_fg_b;
    s->bg_r = s->default_bg_r;
    s->bg_g = s->default_bg_g;
    s->bg_b = s->default_bg_b;
    s->attrs = 0;
}

static void handle_sgr(screen_t *s, const int *p, size_t n)
{
    if (n == 0) { reset_sgr(s); return; }

    size_t i = 0;
    while (i < n) {
        int v = (p[i] < 0) ? 0 : p[i];

        if (v == 0) {
            reset_sgr(s);
        } else if (v == 1) {
            s->attrs |= CELL_ATTR_BOLD;
        } else if (v == 3) {
            s->attrs |= CELL_ATTR_ITALIC;
        } else if (v == 4) {
            s->attrs |= CELL_ATTR_UNDERLINE;
        } else if (v == 5) {
            s->attrs |= CELL_ATTR_BLINK;
        } else if (v == 7) {
            s->attrs |= CELL_ATTR_REVERSE;
        } else if (v == 22) {
            s->attrs &= ~CELL_ATTR_BOLD;
        } else if (v == 23) {
            s->attrs &= ~CELL_ATTR_ITALIC;
        } else if (v == 24) {
            s->attrs &= ~CELL_ATTR_UNDERLINE;
        } else if (v == 25) {
            s->attrs &= ~CELL_ATTR_BLINK;
        } else if (v == 27) {
            s->attrs &= ~CELL_ATTR_REVERSE;
        } else if (v >= 30 && v <= 37) {
            s->fg_r = s->ansi_palette[v-30][0];
            s->fg_g = s->ansi_palette[v-30][1];
            s->fg_b = s->ansi_palette[v-30][2];
        } else if (v == 38) {
            /* 확장 전경색 */
            if (i+1 < n && p[i+1] == 5 && i+2 < n) {
                color256(s, p[i+2], &s->fg_r, &s->fg_g, &s->fg_b);
                i += 2;
            } else if (i+1 < n && p[i+1] == 2 && i+4 < n) {
                s->fg_r = (uint8_t)(p[i+2] < 0 ? 0 : p[i+2]);
                s->fg_g = (uint8_t)(p[i+3] < 0 ? 0 : p[i+3]);
                s->fg_b = (uint8_t)(p[i+4] < 0 ? 0 : p[i+4]);
                i += 4;
            }
        } else if (v == 39) {
            s->fg_r = s->default_fg_r;
            s->fg_g = s->default_fg_g;
            s->fg_b = s->default_fg_b;
        } else if (v >= 40 && v <= 47) {
            s->bg_r = s->ansi_palette[v-40][0];
            s->bg_g = s->ansi_palette[v-40][1];
            s->bg_b = s->ansi_palette[v-40][2];
        } else if (v == 48) {
            /* 확장 배경색 */
            if (i+1 < n && p[i+1] == 5 && i+2 < n) {
                color256(s, p[i+2], &s->bg_r, &s->bg_g, &s->bg_b);
                i += 2;
            } else if (i+1 < n && p[i+1] == 2 && i+4 < n) {
                s->bg_r = (uint8_t)(p[i+2] < 0 ? 0 : p[i+2]);
                s->bg_g = (uint8_t)(p[i+3] < 0 ? 0 : p[i+3]);
                s->bg_b = (uint8_t)(p[i+4] < 0 ? 0 : p[i+4]);
                i += 4;
            }
        } else if (v == 49) {
            s->bg_r = s->default_bg_r;
            s->bg_g = s->default_bg_g;
            s->bg_b = s->default_bg_b;
        } else if (v >= 90 && v <= 97) {
            s->fg_r = s->ansi_palette[v-90+8][0];
            s->fg_g = s->ansi_palette[v-90+8][1];
            s->fg_b = s->ansi_palette[v-90+8][2];
        } else if (v >= 100 && v <= 107) {
            s->bg_r = s->ansi_palette[v-100+8][0];
            s->bg_g = s->ansi_palette[v-100+8][1];
            s->bg_b = s->ansi_palette[v-100+8][2];
        }

        i++;
    }
}

/* ── 모드 설정/해제 ──────────────────────────────────────────────────────── */

static void handle_mode(screen_t *s, const int *p, size_t n,
                         const char *inter, int set)
{
    int priv = (inter && inter[0] == '?');
    for (size_t i = 0; i < n; i++) {
        int m = (p[i] < 0) ? 0 : p[i];
        if (priv) {
            if (m == 25) {
                s->cursor_hidden = set ? 0 : 1;
            } else if (m == 1000) {
                s->mouse_mode = set ? SCREEN_MOUSE_X10 : SCREEN_MOUSE_NONE;
            } else if (m == 1006) {
                s->mouse_mode = set ? SCREEN_MOUSE_SGR : SCREEN_MOUSE_NONE;
            } else if (m == 2004) {
                s->bracketed_paste = set ? 1 : 0;
            } else if (m == 2026) {
                s->sync_output = set ? 1 : 0;
            } else if (m == 1049) {
                /* 대체 화면 전환 */
                if (set && !s->use_alt) {
                    memcpy(s->main_cells, s->cells,
                           (size_t)s->rows * s->cols * sizeof(term_cell_t));
                    s->main_cx = s->cx;
                    s->main_cy = s->cy;
                    memset(s->alt_cells, 0,
                           (size_t)s->rows * s->cols * sizeof(term_cell_t));
                    /* alt_cells를 기본 배경으로 초기화 */
                    term_cell_t blank = blank_cell(s);
                    for (int j = 0; j < s->rows * s->cols; j++)
                        s->alt_cells[j] = blank;
                    s->cells = s->alt_cells;
                    s->use_alt = 1;
                    s->cx = 0; s->cy = 0;
                } else if (!set && s->use_alt) {
                    s->cells = s->main_cells;
                    s->use_alt = 0;
                    s->cx = s->main_cx;
                    s->cy = s->main_cy;
                }
            }
        }
    }
}

/* ── VT 콜백 ─────────────────────────────────────────────────────────────── */

static void cb_print(void *ctx, uint32_t cp)
{
    print_char((screen_t*)ctx, cp);
}

static void cb_execute(void *ctx, uint8_t byte)
{
    screen_t *s = (screen_t*)ctx;
    switch (byte) {
    case 0x07: break;  /* BEL — 무시 */
    case 0x08:         /* BS */
        s->pending_wrap = 0;
        if (s->cx > 0) s->cx--;
        break;
    case 0x09: {       /* HT — 다음 탭 정지 (8칸 단위) */
        s->pending_wrap = 0;
        int next = ((s->cx / 8) + 1) * 8;
        s->cx = imin(next, s->cols - 1);
        break;
    }
    case 0x0A: /* LF */
    case 0x0B: /* VT */
    case 0x0C: /* FF */
        newline(s);
        break;
    case 0x0D: /* CR */
        s->cx = 0;
        s->pending_wrap = 0;
        break;
    default:
        break;
    }
}

static void cb_csi(void *ctx,
                    const int  *params,    size_t params_len,
                    const char *inter,     size_t inter_len,
                    uint8_t     final)
{
    screen_t *s = (screen_t*)ctx;
    (void)inter_len;

    /* 파라미터 0번/1번 편의 변수 */
    int p0 = (params_len > 0 && params[0] >= 0) ? params[0] : 0;
    int p1 = (params_len > 1 && params[1] >= 0) ? params[1] : 0;

    s->pending_wrap = 0;

    switch (final) {
    case 'A': cur_up(s,   p0 ? p0 : 1); break;  /* CUU */
    case 'B': cur_down(s, p0 ? p0 : 1); break;  /* CUD */
    case 'C': cur_right(s,p0 ? p0 : 1); break;  /* CUF */
    case 'D': cur_left(s, p0 ? p0 : 1); break;  /* CUB */
    case 'E':                                     /* CNL */
        s->cy = imin(s->cy + (p0 ? p0 : 1), s->rows - 1);
        s->cx = 0;
        break;
    case 'F':                                     /* CPL */
        s->cy = imax(s->cy - (p0 ? p0 : 1), 0);
        s->cx = 0;
        break;
    case 'G':                                     /* CHA */
        s->cx = iclamp((p0 ? p0 : 1) - 1, 0, s->cols - 1);
        break;
    case 'H':                                     /* CUP */
    case 'f':                                     /* HVP */
        s->cy = iclamp((p0 ? p0 : 1) - 1, 0, s->rows - 1);
        s->cx = iclamp((p1 ? p1 : 1) - 1, 0, s->cols - 1);
        break;
    case 'J': erase_display(s, p0); break;        /* ED */
    case 'K': erase_line(s, p0);    break;        /* EL */
    case 'L': insert_lines(s, p0 ? p0 : 1); break; /* IL */
    case 'M': delete_lines(s, p0 ? p0 : 1); break; /* DL */
    case 'P': delete_chars(s, p0 ? p0 : 1); break; /* DCH */
    case 'S': scroll_up_n(s,   p0 ? p0 : 1); break; /* SU */
    case 'T': scroll_down_n(s, p0 ? p0 : 1); break; /* SD */
    case 'X': erase_chars(s, p0 ? p0 : 1); break;   /* ECH */
    case 'd':                                     /* VPA */
        s->cy = iclamp((p0 ? p0 : 1) - 1, 0, s->rows - 1);
        break;
    case 'h': handle_mode(s, params, params_len, inter, 1); break;
    case 'l': handle_mode(s, params, params_len, inter, 0); break;
    case 'm': handle_sgr(s, params, params_len);  break;  /* SGR */
    case 'r': {                                   /* DECSTBM */
        int top = iclamp((p0 ? p0 : 1) - 1, 0, s->rows - 1);
        int bot = iclamp((p1 ? p1 : s->rows) - 1, 0, s->rows - 1);
        if (top < bot) { s->scroll_top = top; s->scroll_bot = bot; }
        s->cx = 0; s->cy = 0;
        break;
    }
    case 's': /* SCOSC — 커서 저장 */
        s->saved_cx = s->cx; s->saved_cy = s->cy;
        break;
    case 'u': /* SCORC — 커서 복원 */
        s->cx = iclamp(s->saved_cx, 0, s->cols - 1);
        s->cy = iclamp(s->saved_cy, 0, s->rows - 1);
        break;
    case 'q': /* DECSCUSR — CSI Ps SP q (intermediate = space) */
        if (inter && inter[0] == ' ')
            s->cursor_style = iclamp(p0, 0, 6);
        break;
    default:
        break;
    }
}

static void cb_esc(void *ctx,
                    const char *inter, size_t inter_len,
                    uint8_t final)
{
    screen_t *s = (screen_t*)ctx;
    (void)inter_len;
    if (!inter || inter[0] == '\0') {
        switch (final) {
        case '7': /* DECSC */
            s->saved_cx = s->cx; s->saved_cy = s->cy;
            break;
        case '8': /* DECRC */
            s->cx = iclamp(s->saved_cx, 0, s->cols - 1);
            s->cy = iclamp(s->saved_cy, 0, s->rows - 1);
            break;
        case 'M': /* RI — reverse index (scroll down if at top) */
            if (s->cy == s->scroll_top)
                scroll_down_n(s, 1);
            else if (s->cy > 0)
                s->cy--;
            break;
        case 'c': /* RIS — full reset */
            reset_sgr(s);
            s->cx = s->cy = 0;
            s->scroll_top = 0; s->scroll_bot = s->rows - 1;
            erase_display(s, 2);
            break;
        default:
            break;
        }
    }
}

/* ── Base64 디코딩 (OSC 52 용) ───────────────────────────────────────────── */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* out must be at least (len*3/4+1) bytes. Returns decoded length, -1 on error. */
static int b64_decode(const char *in, size_t len, char *out)
{
    int pos = 0;
    size_t i = 0;
    while (i < len) {
        int a = -1, b = -1, c = -1, d = -1;

        while (i < len && (a = b64_val(in[i])) < 0 && in[i] != '=')
            i++;
        if (i >= len || in[i] == '=') break;
        i++;

        while (i < len && (b = b64_val(in[i])) < 0 && in[i] != '=')
            i++;
        if (b < 0) break;
        i++;
        out[pos++] = (char)((a << 2) | (b >> 4));

        while (i < len && (c = b64_val(in[i])) < 0 && in[i] != '=')
            i++;
        if (i >= len || in[i] == '=') break;
        i++;
        out[pos++] = (char)(((b & 0xF) << 4) | (c >> 2));

        while (i < len && (d = b64_val(in[i])) < 0 && in[i] != '=')
            i++;
        if (i >= len || in[i] == '=') break;
        i++;
        out[pos++] = (char)(((c & 0x3) << 6) | d);
    }
    out[pos] = '\0';
    return pos;
}

/* ── OSC 콜백 ───────────────────────────────────────────────────────────── */

static void cb_osc(void *ctx, const char *payload, size_t len)
{
    screen_t *s = (screen_t*)ctx;
    if (!payload || len < 2) return;

    /* payload 형식: "N;text" */
    int code = 0;
    size_t i = 0;
    while (i < len && payload[i] >= '0' && payload[i] <= '9')
        code = code * 10 + (payload[i++] - '0');
    if (i >= len || payload[i] != ';') return;
    i++;  /* ';' 건너뜀 */

    if (code == 0 || code == 2) {
        /* OSC 0/2: 창 타이틀 */
        size_t tlen = len - i;
        if (tlen >= sizeof s->title) tlen = sizeof s->title - 1;
        memcpy(s->title, payload + i, tlen);
        s->title[tlen] = '\0';
    } else if (code == 8) {
        /* OSC 8: 하이퍼링크 — "8;params;URI".
         * URI 가 비어있으면 활성 링크 해제, 아니면 테이블에 저장 후 활성화. */
        const char *rest = payload + i;
        size_t rlen = len - i;
        const char *semi = memchr(rest, ';', rlen);
        if (!semi) return;
        size_t skip = (size_t)(semi - rest) + 1;
        const char *uri = rest + skip;
        size_t uri_len = rlen - skip;
        if (uri_len == 0) {
            s->active_link_id = 0;
            return;
        }
        /* 기존 테이블에서 같은 URL 검색 (중복 저장 방지) */
        for (int k = 0; k < s->link_count; k++) {
            if (strncmp(s->links[k].url, uri, uri_len) == 0 &&
                s->links[k].url[uri_len] == '\0') {
                s->active_link_id = s->links[k].id;
                return;
            }
        }
        /* 새 링크 등록 */
        int slot = s->link_count;
        if (slot >= (int)(sizeof(s->links) / sizeof(s->links[0]))) {
            /* 오래된 엔트리를 덮어쓰기 (ring replacement) */
            slot = s->next_link_id % (int)(sizeof(s->links) / sizeof(s->links[0]));
        } else {
            s->link_count++;
        }
        size_t cpy = uri_len;
        if (cpy >= sizeof(s->links[0].url)) cpy = sizeof(s->links[0].url) - 1;
        memcpy(s->links[slot].url, uri, cpy);
        s->links[slot].url[cpy] = '\0';
        s->links[slot].id = ++s->next_link_id;
        if (s->next_link_id == 0) s->next_link_id = 1;  /* 0 은 "링크 없음" 예약 */
        s->active_link_id = s->links[slot].id;
    } else if (code == 52) {
        /* OSC 52: 클립보드 — "52;c;BASE64" 또는 "52;pc;BASE64" */
        /* 'c' 또는 'pc' 등의 selection parameter 건너뛰기 */
        const char *rest = payload + i;
        size_t rlen = len - i;
        const char *semi = memchr(rest, ';', rlen);
        if (!semi) return;
        size_t skip = (size_t)(semi - rest) + 1;
        const char *b64 = rest + skip;
        size_t b64len = rlen - skip;

        if (b64len == 0) return;
        /* "?" = 클립보드 읽기 요청 — 보안상 무시 */
        if (b64len == 1 && b64[0] == '?') return;

        /* base64 디코딩 */
        size_t out_max = b64len * 3 / 4 + 4;
        char *decoded = malloc(out_max);
        if (!decoded) return;
        int dlen = b64_decode(b64, b64len, decoded);
        if (dlen > 0 && s->clipboard_cb) {
            s->clipboard_cb(decoded, s->clipboard_user);
        }
        free(decoded);
    }
}

/* ── 공개 API ────────────────────────────────────────────────────────────── */

int screen_init(screen_t *s, int cols, int rows, int scrollback_lines)
{
    memset(s, 0, sizeof *s);
    s->cols          = cols;
    s->rows          = rows;
    s->scrollback_max = scrollback_lines;
    s->scroll_top    = 0;
    s->scroll_bot    = rows - 1;

    s->main_cells = calloc((size_t)rows * cols, sizeof(term_cell_t));
    s->alt_cells  = calloc((size_t)rows * cols, sizeof(term_cell_t));
    if (!s->main_cells || !s->alt_cells) goto oom;

    if (scrollback_lines > 0) {
        s->scrollback = calloc((size_t)scrollback_lines * cols,
                                sizeof(term_cell_t));
        s->view_cells = calloc((size_t)rows * cols, sizeof(term_cell_t));
        if (!s->scrollback || !s->view_cells) goto oom;
    }

    s->cells = s->main_cells;

    /* 기본 팔레트 초기화 (하드코딩 fallback — 테마 로드 시 교체됨) */
    s->default_fg_r = CELL_DEFAULT_FG_R;
    s->default_fg_g = CELL_DEFAULT_FG_G;
    s->default_fg_b = CELL_DEFAULT_FG_B;
    s->default_bg_r = CELL_DEFAULT_BG_R;
    s->default_bg_g = CELL_DEFAULT_BG_G;
    s->default_bg_b = CELL_DEFAULT_BG_B;
    for (int i = 0; i < 16; i++) {
        s->ansi_palette[i][0] = ANSI16[i][0];
        s->ansi_palette[i][1] = ANSI16[i][1];
        s->ansi_palette[i][2] = ANSI16[i][2];
    }

    /* 기본 색 설정 */
    reset_sgr(s);

    /* 초기 셀을 기본 배경으로 채우기 */
    {
        term_cell_t blank = blank_cell(s);
        for (int i = 0; i < rows * cols; i++) s->main_cells[i] = blank;
        for (int i = 0; i < rows * cols; i++) s->alt_cells[i]  = blank;
    }

    /* VT 파서 초기화 */
    static const vt_callbacks_t cbs = {
        .print        = cb_print,
        .execute      = cb_execute,
        .csi_dispatch = cb_csi,
        .esc_dispatch = cb_esc,
        .osc_dispatch = cb_osc,
    };
    vt_parser_init(&s->parser, &cbs, s);

    return 0;

oom:
    free(s->main_cells);
    free(s->alt_cells);
    free(s->scrollback);
    return -1;
}

void screen_destroy(screen_t *s)
{
    if (!s) return;
    free(s->main_cells);
    free(s->alt_cells);
    free(s->scrollback);
    free(s->view_cells);
    memset(s, 0, sizeof *s);
}

void screen_feed(screen_t *s, const uint8_t *data, size_t len)
{
    vt_parser_feed(&s->parser, data, len);
}

void screen_resize(screen_t *s, int new_cols, int new_rows)
{
    if (new_cols == s->cols && new_rows == s->rows) return;

    /* 새 그리드 할당 */
    term_cell_t *new_main = calloc((size_t)new_rows * new_cols, sizeof(term_cell_t));
    term_cell_t *new_alt  = calloc((size_t)new_rows * new_cols, sizeof(term_cell_t));
    if (!new_main || !new_alt) { free(new_main); free(new_alt); return; }

    /* 기본 배경으로 초기화 */
    term_cell_t blank = blank_cell(s);
    for (int i = 0; i < new_rows * new_cols; i++) new_main[i] = blank;
    for (int i = 0; i < new_rows * new_cols; i++) new_alt[i]  = blank;

    /* 기존 내용 복사 (겹치는 범위) */
    int copy_rows = imin(s->rows, new_rows);
    int copy_cols = imin(s->cols, new_cols);
    for (int r = 0; r < copy_rows; r++) {
        memcpy(new_main + r * new_cols,
               s->main_cells + r * s->cols,
               (size_t)copy_cols * sizeof(term_cell_t));
    }

    free(s->main_cells);
    free(s->alt_cells);
    s->main_cells = new_main;
    s->alt_cells  = new_alt;
    s->cells      = s->use_alt ? s->alt_cells : s->main_cells;

    s->cols = new_cols;
    s->rows = new_rows;
    s->scroll_top = 0;
    s->scroll_bot = new_rows - 1;
    s->cx = iclamp(s->cx, 0, new_cols - 1);
    s->cy = iclamp(s->cy, 0, new_rows - 1);
    s->pending_wrap = 0;

    /* 스크롤백/뷰 버퍼는 크기가 바뀌면 재할당 */
    if (s->scrollback && new_cols != copy_cols) {
        free(s->scrollback);
        s->scrollback = calloc((size_t)s->scrollback_max * new_cols,
                                sizeof(term_cell_t));
        if (!s->scrollback) s->scrollback_max = 0;
        s->sb_head = s->sb_count = 0;
    }
    if (s->view_cells) {
        free(s->view_cells);
        s->view_cells = calloc((size_t)new_rows * new_cols, sizeof(term_cell_t));
    }
    s->sb_offset = 0;
}

/* ── 스크롤백 뷰 합성 ────────────────────────────────────────────────────── */

/*
 * view_cells 를 scrollback 과 현재 화면으로 합성한다.
 * sb_offset 줄 과거에서 시작해 rows 줄을 채운다.
 *
 * 레이아웃:
 *   view[0 .. from_sb-1]        → scrollback 의 최근 offset 줄 중 앞부분
 *   view[from_sb .. rows-1]     → 현재 화면의 앞부분 (rows - offset 줄)
 */
static void compose_view(screen_t *s)
{
    int offset   = s->sb_offset;
    int from_sb  = (offset >= s->rows) ? s->rows : offset;
    int from_sc  = s->rows - from_sb;

    /* scrollback 에서 채우기 */
    int sb_base = s->sb_count - offset;  /* 뷰 첫 줄의 ring-buffer 논리 인덱스 */
    for (int r = 0; r < from_sb; r++) {
        int logic = sb_base + r;
        if (logic < 0 || logic >= s->sb_count) {
            /* 범위 밖 → 빈 행 */
            term_cell_t blank = blank_cell(s);
            for (int c = 0; c < s->cols; c++)
                s->view_cells[r * s->cols + c] = blank;
        } else {
            int ring = (s->sb_head - s->sb_count + logic + s->scrollback_max)
                       % s->scrollback_max;
            memcpy(s->view_cells + r * s->cols,
                   s->scrollback + ring * s->cols,
                   (size_t)s->cols * sizeof(term_cell_t));
        }
    }

    /* 현재 화면에서 채우기 */
    for (int r = 0; r < from_sc; r++) {
        memcpy(s->view_cells + (from_sb + r) * s->cols,
               s->cells + r * s->cols,
               (size_t)s->cols * sizeof(term_cell_t));
    }
}

const term_cell_t *screen_get_cells(screen_t *s)
{
    if (s->sb_offset > 0 && s->view_cells) {
        compose_view(s);
        return s->view_cells;
    }
    return s->cells;
}

void screen_scroll_view(screen_t *s, int delta)
{
    if (!s->scrollback || !s->view_cells || s->use_alt) return;

    int new_off = s->sb_offset + delta;
    if (new_off < 0) new_off = 0;
    if (new_off > s->sb_count) new_off = s->sb_count;
    s->sb_offset = new_off;
}

void screen_scroll_view_reset(screen_t *s)
{
    s->sb_offset = 0;
}

int screen_scrollback_offset(const screen_t *s)
{
    return s->sb_offset;
}
int screen_cursor_x(const screen_t *s) { return s->cx; }
int screen_cursor_y(const screen_t *s) { return s->cy; }
int screen_cursor_hidden(const screen_t *s)    { return s->cursor_hidden; }
int screen_cursor_style(const screen_t *s)     { return s->cursor_style; }
int screen_mouse_mode(const screen_t *s)       { return s->mouse_mode; }
int screen_bracketed_paste(const screen_t *s)  { return s->bracketed_paste; }
int screen_sync_output(const screen_t *s)      { return s->sync_output; }
const char *screen_get_title(const screen_t *s){ return s->title; }

void screen_set_clipboard_cb(screen_t *s,
                              void (*cb)(const char *text, void *user),
                              void *user)
{
    if (!s) return;
    s->clipboard_cb   = cb;
    s->clipboard_user = user;
}

const char *screen_link_url(const screen_t *s, uint16_t link_id)
{
    if (!s || link_id == 0) return NULL;
    for (int k = 0; k < s->link_count; k++)
        if (s->links[k].id == link_id) return s->links[k].url;
    return NULL;
}

/* 팔레트 + 기본 fg/bg 갱신 공통 로직 */
static void apply_palette(screen_t *s, const termemu_theme_t *t)
{
    for (int i = 0; i < 16; i++) {
        uint32_t c = t->ansi[i];
        s->ansi_palette[i][0] = (uint8_t)((c >> 16) & 0xFF);
        s->ansi_palette[i][1] = (uint8_t)((c >>  8) & 0xFF);
        s->ansi_palette[i][2] = (uint8_t)( c        & 0xFF);
    }
    s->default_fg_r = (uint8_t)((t->foreground >> 16) & 0xFF);
    s->default_fg_g = (uint8_t)((t->foreground >>  8) & 0xFF);
    s->default_fg_b = (uint8_t)( t->foreground        & 0xFF);
    s->default_bg_r = (uint8_t)((t->background >> 16) & 0xFF);
    s->default_bg_g = (uint8_t)((t->background >>  8) & 0xFF);
    s->default_bg_b = (uint8_t)( t->background        & 0xFF);
}

void screen_update_palette(screen_t *s, const termemu_theme_t *t)
{
    if (!s || !t) return;

    /* 이전 기본색 기억 (baked 셀 색상 매핑용) */
    uint8_t old_fg_r = s->default_fg_r, old_fg_g = s->default_fg_g, old_fg_b = s->default_fg_b;
    uint8_t old_bg_r = s->default_bg_r, old_bg_g = s->default_bg_g, old_bg_b = s->default_bg_b;
    uint8_t old_ansi[16][3];
    memcpy(old_ansi, s->ansi_palette, sizeof old_ansi);

    apply_palette(s, t);
    reset_sgr(s);

    /* 기존 셀: 이전 기본색이었던 셀만 새 기본색으로 교체.
     * 이전 ANSI 팔레트 색이었던 셀은 새 ANSI 색으로 매핑.
     * 명시적 24-bit 색상은 건드리지 않는다. */
    int total = s->rows * s->cols;
    term_cell_t *bufs[] = { s->main_cells, s->alt_cells };
    for (int b = 0; b < 2; b++) {
        term_cell_t *cells = bufs[b];
        for (int i = 0; i < total; i++) {
            /* 전경색 매핑 */
            if (cells[i].fg_r == old_fg_r && cells[i].fg_g == old_fg_g && cells[i].fg_b == old_fg_b) {
                cells[i].fg_r = s->default_fg_r; cells[i].fg_g = s->default_fg_g; cells[i].fg_b = s->default_fg_b;
            } else {
                for (int c = 0; c < 16; c++) {
                    if (cells[i].fg_r == old_ansi[c][0] && cells[i].fg_g == old_ansi[c][1] && cells[i].fg_b == old_ansi[c][2]) {
                        cells[i].fg_r = s->ansi_palette[c][0]; cells[i].fg_g = s->ansi_palette[c][1]; cells[i].fg_b = s->ansi_palette[c][2];
                        break;
                    }
                }
            }
            /* 배경색 매핑 */
            if (cells[i].bg_r == old_bg_r && cells[i].bg_g == old_bg_g && cells[i].bg_b == old_bg_b) {
                cells[i].bg_r = s->default_bg_r; cells[i].bg_g = s->default_bg_g; cells[i].bg_b = s->default_bg_b;
            } else {
                for (int c = 0; c < 16; c++) {
                    if (cells[i].bg_r == old_ansi[c][0] && cells[i].bg_g == old_ansi[c][1] && cells[i].bg_b == old_ansi[c][2]) {
                        cells[i].bg_r = s->ansi_palette[c][0]; cells[i].bg_g = s->ansi_palette[c][1]; cells[i].bg_b = s->ansi_palette[c][2];
                        break;
                    }
                }
            }
        }
    }
}

void screen_apply_theme(screen_t *s, const termemu_theme_t *t)
{
    if (!s || !t) return;
    apply_palette(s, t);

    /* 현재 SGR 상태를 새 기본값으로 리셋 */
    reset_sgr(s);

    /* 모든 셀의 fg/bg를 새 테마 기본색으로 교체한다.
     * 셀 색상은 쓰기 시점에 baking되므로 테마가 바뀌면 전체를 재칠해야 한다. */
    uint8_t dfg_r = s->default_fg_r, dfg_g = s->default_fg_g, dfg_b = s->default_fg_b;
    uint8_t dbg_r = s->default_bg_r, dbg_g = s->default_bg_g, dbg_b = s->default_bg_b;
    int total = s->rows * s->cols;
    for (int i = 0; i < total; i++) {
        s->main_cells[i].fg_r = dfg_r; s->main_cells[i].fg_g = dfg_g; s->main_cells[i].fg_b = dfg_b;
        s->main_cells[i].bg_r = dbg_r; s->main_cells[i].bg_g = dbg_g; s->main_cells[i].bg_b = dbg_b;
        s->alt_cells[i].fg_r = dfg_r; s->alt_cells[i].fg_g = dfg_g; s->alt_cells[i].fg_b = dfg_b;
        s->alt_cells[i].bg_r = dbg_r; s->alt_cells[i].bg_g = dbg_g; s->alt_cells[i].bg_b = dbg_b;
    }
}
