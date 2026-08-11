/*
 * test_status_bar.c — 하단 상태바 행 빌더 테스트
 *
 * status_bar_build() 는 순수 함수(입력 → term_cell_t 배열)라 OpenGL 없이
 * 그대로 검증할 수 있다.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../src/client/ui/status_bar.h"
#include "../src/client/cell.h"
#include "../src/common/config.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

/* 셀 배열에서 ASCII 부분만 뽑아 문자열로 (비교 편의용). */
static void row_to_text(const term_cell_t *cells, int cols, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < cols && n + 1 < cap; i++) {
        uint32_t cp = cells[i].codepoint;
        out[n++] = (cp >= 0x20 && cp < 0x7F) ? (char)cp : ' ';
    }
    out[n] = '\0';
}

static int color_eq(const term_cell_t *c, uint32_t fg, uint32_t bg)
{
    return c->fg_r == ((fg >> 16) & 0xFF) && c->fg_g == ((fg >> 8) & 0xFF) &&
           c->fg_b == (fg & 0xFF) &&
           c->bg_r == ((bg >> 16) & 0xFF) && c->bg_g == ((bg >> 8) & 0xFF) &&
           c->bg_b == (bg & 0xFF);
}

static void fill_info(status_bar_info_t *info)
{
    memset(info, 0, sizeof *info);
    info->session_name  = "work";
    info->window_count  = 3;
    info->active_window = 1;
    snprintf(info->windows[0].name, sizeof info->windows[0].name, "shell");
    snprintf(info->windows[1].name, sizeof info->windows[1].name, "vim");
    snprintf(info->windows[2].name, sizeof info->windows[2].name, "logs");
    info->active_pane_id = 7;
    info->pane_cols = 80;
    info->pane_rows = 24;
}

static void test_basic_layout(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    fill_info(&info);

    term_cell_t row[STATUS_BAR_MAX_COLS];
    const int cols = 100;
    status_bar_build(row, cols, &info, &th);

    char text[256];
    row_to_text(row, cols, text, sizeof text);

    CHECK(strstr(text, "work")  != NULL, "세션명 표시");
    CHECK(strstr(text, "1:shell") != NULL, "window 1 표시");
    CHECK(strstr(text, "2:vim")   != NULL, "window 2 표시");
    CHECK(strstr(text, "3:logs")  != NULL, "window 3 표시");
    CHECK(strstr(text, "pane 7")  != NULL, "활성 pane id 표시");
    CHECK(strstr(text, "80x24")   != NULL, "pane 크기 표시");

    /* 모든 칸이 채워져야 한다 (배경이 끊기지 않도록) */
    int all_filled = 1;
    for (int i = 0; i < cols; i++)
        if (row[i].codepoint == 0) all_filled = 0;
    CHECK(all_filled, "빈 칸 없이 전부 채워짐");

    printf("[PASS] test_basic_layout\n");
}

static void test_active_highlight(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    fill_info(&info);

    term_cell_t row[STATUS_BAR_MAX_COLS];
    const int cols = 100;
    status_bar_build(row, cols, &info, &th);

    char text[256];
    row_to_text(row, cols, text, sizeof text);

    /* "2:vim" 이 활성 → 활성 색상 + BOLD 여야 한다 */
    const char *p = strstr(text, "2:vim");
    CHECK(p != NULL, "활성 window 세그먼트 존재");
    if (p) {
        int idx = (int)(p - text);
        CHECK(color_eq(&row[idx], th.statusbar_active_fg, th.statusbar_active_bg),
              "활성 window 는 active 색상");
        CHECK(row[idx].attrs & CELL_ATTR_BOLD, "활성 window 는 BOLD");
    }

    /* "1:shell" 은 비활성 → 기본 색상 */
    const char *q = strstr(text, "1:shell");
    CHECK(q != NULL, "비활성 window 세그먼트 존재");
    if (q) {
        int idx = (int)(q - text);
        CHECK(color_eq(&row[idx], th.statusbar_fg, th.statusbar_bg),
              "비활성 window 는 기본 색상");
    }

    printf("[PASS] test_active_highlight\n");
}

static void test_activity_marker(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    fill_info(&info);
    info.windows[0].activity = 1;   /* 비활성 window 에 활동 */
    info.windows[1].activity = 1;   /* 활성 window — 표시되면 안 됨 */

    term_cell_t row[STATUS_BAR_MAX_COLS];
    const int cols = 100;
    status_bar_build(row, cols, &info, &th);

    char text[256];
    row_to_text(row, cols, text, sizeof text);

    CHECK(strstr(text, "1:shell*") != NULL, "비활성 window 활동에 * 표시");
    CHECK(strstr(text, "2:vim*")   == NULL, "활성 window 에는 * 없음");

    printf("[PASS] test_activity_marker\n");
}

static void test_status_flags(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    fill_info(&info);
    info.remote     = 1;
    info.scrollback = 42;
    info.selecting  = 1;

    term_cell_t row[STATUS_BAR_MAX_COLS];
    const int cols = 120;
    status_bar_build(row, cols, &info, &th);

    char text[256];
    row_to_text(row, cols, text, sizeof text);

    CHECK(strstr(text, "[SCROLL 42]") != NULL, "스크롤백 오프셋 표시");
    CHECK(strstr(text, "[SELECT]")    != NULL, "선택 중 표시");
    CHECK(strstr(text, "[REMOTE]")    != NULL, "원격 연결 표시");

    printf("[PASS] test_status_flags\n");
}

/* 좁은 폭에서 오버런/깨짐이 없어야 한다. */
static void test_narrow_widths(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    fill_info(&info);

    for (int cols = 1; cols <= 60; cols++) {
        /* 가드 바이트로 오버런 검출 */
        term_cell_t row[STATUS_BAR_MAX_COLS];
        memset(row, 0, sizeof row);
        row[cols].codepoint = 0xDEAD;

        status_bar_build(row, cols, &info, &th);

        char msg[64];
        snprintf(msg, sizeof msg, "cols=%d: 경계 밖 미침범", cols);
        CHECK(row[cols].codepoint == 0xDEAD, msg);

        snprintf(msg, sizeof msg, "cols=%d: 전 칸 채움", cols);
        int filled = 1;
        for (int i = 0; i < cols; i++)
            if (row[i].codepoint == 0) filled = 0;
        CHECK(filled, msg);
    }

    printf("[PASS] test_narrow_widths\n");
}

/* 오른쪽 블록이 폭의 절반을 넘으면 생략되고 왼쪽이 살아야 한다. */
static void test_right_block_dropped(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    fill_info(&info);
    info.remote = 1;
    info.scrollback = 999;
    info.selecting = 1;

    term_cell_t row[STATUS_BAR_MAX_COLS];
    const int cols = 30;
    status_bar_build(row, cols, &info, &th);

    char text[64];
    row_to_text(row, cols, text, sizeof text);

    CHECK(strstr(text, "[REMOTE]") == NULL, "좁으면 오른쪽 블록 생략");
    CHECK(strstr(text, "work")     != NULL, "좁아도 세션명은 유지");

    printf("[PASS] test_right_block_dropped\n");
}

/* UTF-8 이름이 바이트 단위로 쪼개지지 않아야 한다. */
static void test_utf8_name(void)
{
    tessera_theme_t th;
    theme_defaults(&th);

    status_bar_info_t info;
    memset(&info, 0, sizeof info);
    info.session_name  = "한글";
    info.window_count  = 1;
    info.active_window = 0;
    snprintf(info.windows[0].name, sizeof info.windows[0].name, "가");

    term_cell_t row[STATUS_BAR_MAX_COLS];
    const int cols = 40;
    status_bar_build(row, cols, &info, &th);

    /* U+D55C(한) U+AE00(글) 이 각각 한 셀에 들어가야 한다 */
    int found_han = 0, found_ga = 0;
    for (int i = 0; i < cols; i++) {
        if (row[i].codepoint == 0xD55C) found_han = 1;
        if (row[i].codepoint == 0xAC00) found_ga  = 1;
    }
    CHECK(found_han, "UTF-8 세션명이 코드포인트로 디코딩됨");
    CHECK(found_ga,  "UTF-8 window 이름이 코드포인트로 디코딩됨");

    printf("[PASS] test_utf8_name\n");
}

int main(void)
{
    test_basic_layout();
    test_active_highlight();
    test_activity_marker();
    test_status_flags();
    test_narrow_widths();
    test_right_block_dropped();
    test_utf8_name();

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
