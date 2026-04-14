/*
 * test_screen.c — VT 파서 ↔ screen buffer 통합 테스트
 *
 * screen_feed() 에 이스케이프 시퀀스를 주입하고 cell grid 상태를 검증.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "../src/client/screen.h"
#include "../src/client/cell.h"

/* ── 작은 헬퍼 ───────────────────────────────────────────────────────────── */

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

static void feed(screen_t *s, const char *str)
{
    screen_feed(s, (const uint8_t *)str, strlen(str));
}

static const term_cell_t *cell_at(screen_t *s, int col, int row)
{
    return &screen_get_cells(s)[row * s->cols + col];
}

/* ══════════════════════════════════════════════════════════════════════════ */

static void test_basic_print(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    feed(&s, "Hello");

    CHECK(cell_at(&s, 0, 0)->codepoint == 'H', "col0 = 'H'");
    CHECK(cell_at(&s, 1, 0)->codepoint == 'e', "col1 = 'e'");
    CHECK(cell_at(&s, 2, 0)->codepoint == 'l', "col2 = 'l'");
    CHECK(cell_at(&s, 3, 0)->codepoint == 'l', "col3 = 'l'");
    CHECK(cell_at(&s, 4, 0)->codepoint == 'o', "col4 = 'o'");
    CHECK(screen_cursor_x(&s) == 5, "cursor x=5 after 'Hello'");
    CHECK(screen_cursor_y(&s) == 0, "cursor y=0");

    screen_destroy(&s);
    printf("[PASS] test_basic_print\n");
}

static void test_newline(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    feed(&s, "AB\r\nCD");

    CHECK(cell_at(&s, 0, 0)->codepoint == 'A', "row0 col0 = 'A'");
    CHECK(cell_at(&s, 1, 0)->codepoint == 'B', "row0 col1 = 'B'");
    CHECK(cell_at(&s, 0, 1)->codepoint == 'C', "row1 col0 = 'C'");
    CHECK(cell_at(&s, 1, 1)->codepoint == 'D', "row1 col1 = 'D'");
    CHECK(screen_cursor_x(&s) == 2, "cursor x=2");
    CHECK(screen_cursor_y(&s) == 1, "cursor y=1");

    screen_destroy(&s);
    printf("[PASS] test_newline\n");
}

static void test_sgr_bold_color(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    /* ESC[1;32m = bold + green fg */
    feed(&s, "\x1b[1;32mX\x1b[0m");

    const term_cell_t *c = cell_at(&s, 0, 0);
    CHECK(c->codepoint == 'X', "codepoint 'X'");
    CHECK(c->attrs & CELL_ATTR_BOLD, "bold flag set");
    /* ANSI16[2] = green (0, 170, 0) */
    CHECK(c->fg_r ==   0, "fg_r=0 (ANSI green)");
    CHECK(c->fg_g == 170, "fg_g=170 (ANSI green)");
    CHECK(c->fg_b ==   0, "fg_b=0 (ANSI green)");

    /* after reset, next char should have no attrs */
    feed(&s, "Y");
    const term_cell_t *c2 = cell_at(&s, 1, 0);
    CHECK(c2->attrs == 0, "attrs=0 after SGR reset");

    screen_destroy(&s);
    printf("[PASS] test_sgr_bold_color\n");
}

static void test_sgr_256color(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    /* ESC[38;5;196m = 256-color fg index 196 */
    /* 196 = 16 + (5*36 + 0*6 + 0) = 16+180 = 196 → R=5*51=255 G=0 B=0 */
    feed(&s, "\x1b[38;5;196mR\x1b[0m");

    const term_cell_t *c = cell_at(&s, 0, 0);
    CHECK(c->codepoint == 'R', "codepoint 'R'");
    CHECK(c->fg_r == 255, "fg_r=255 (256-color red)");
    CHECK(c->fg_g ==   0, "fg_g=0");
    CHECK(c->fg_b ==   0, "fg_b=0");

    screen_destroy(&s);
    printf("[PASS] test_sgr_256color\n");
}

static void test_sgr_truecolor(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    /* ESC[38;2;100;150;200m = truecolor fg */
    feed(&s, "\x1b[38;2;100;150;200mT\x1b[0m");

    const term_cell_t *c = cell_at(&s, 0, 0);
    CHECK(c->codepoint == 'T', "codepoint 'T'");
    CHECK(c->fg_r == 100, "fg_r=100");
    CHECK(c->fg_g == 150, "fg_g=150");
    CHECK(c->fg_b == 200, "fg_b=200");

    screen_destroy(&s);
    printf("[PASS] test_sgr_truecolor\n");
}

static void test_cursor_movement(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    /* CUP (ESC[row;colH) is 1-based */
    feed(&s, "\x1b[5;10H");
    CHECK(screen_cursor_x(&s) == 9, "CUP: cursor x=9 (col 10 → 0-based 9)");
    CHECK(screen_cursor_y(&s) == 4, "CUP: cursor y=4 (row 5 → 0-based 4)");

    /* CUU — cursor up 2 */
    feed(&s, "\x1b[2A");
    CHECK(screen_cursor_y(&s) == 2, "CUU: cursor y=2");

    /* CUD — cursor down 1 */
    feed(&s, "\x1b[B");
    CHECK(screen_cursor_y(&s) == 3, "CUD: cursor y=3");

    /* CUF — cursor forward 3 */
    feed(&s, "\x1b[3C");
    CHECK(screen_cursor_x(&s) == 12, "CUF: cursor x=12");

    /* CUB — cursor back 1 */
    feed(&s, "\x1b[D");
    CHECK(screen_cursor_x(&s) == 11, "CUB: cursor x=11");

    screen_destroy(&s);
    printf("[PASS] test_cursor_movement\n");
}

static void test_erase_display(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    /* 화면에 글자 채우기 */
    feed(&s, "AAAA\r\nBBBB\r\nCCCC");

    /* ESC[2J — erase all */
    feed(&s, "\x1b[2J");

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            CHECK(cell_at(&s, c, r)->codepoint == 0,
                  "cell erased after ESC[2J");
        }
    }

    screen_destroy(&s);
    printf("[PASS] test_erase_display\n");
}

static void test_erase_line(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    feed(&s, "Hello World");
    /* cursor is at col 11 — go back to col 5, erase to end of line */
    feed(&s, "\x1b[1;6H");  /* move to row1 col6 → but text is on row0 */
    /* move cursor to row 0, col 5 */
    feed(&s, "\x1b[1;6H");  /* row=1 col=6 → y=0 x=5 */
    feed(&s, "\x1b[K");     /* ESC[K = EL 0 = erase to right */

    CHECK(cell_at(&s, 0, 0)->codepoint == 'H', "col0 intact");
    CHECK(cell_at(&s, 4, 0)->codepoint == 'o', "col4 intact");
    CHECK(cell_at(&s, 5, 0)->codepoint == 0,   "col5 erased");
    CHECK(cell_at(&s, 10, 0)->codepoint == 0,  "col10 erased");

    screen_destroy(&s);
    printf("[PASS] test_erase_line\n");
}

static void test_scrollback(void)
{
    screen_t s;
    /* 4행 화면, 스크롤백 10줄 */
    assert(screen_init(&s, 10, 4, 10) == 0);

    /* 4줄 입력 후 한 줄 더 → 첫 번째 줄이 스크롤백으로 */
    feed(&s, "LINE0\r\nLINE1\r\nLINE2\r\nLINE3\r\nLINE4");

    /* 화면 첫 행에는 LINE1이 있어야 함 */
    CHECK(cell_at(&s, 0, 0)->codepoint == 'L', "row0 = 'L' of LINE1");
    CHECK(cell_at(&s, 1, 0)->codepoint == 'I', "row0 col1 = 'I'");
    CHECK(cell_at(&s, 4, 0)->codepoint == '1', "row0 col4 = '1'");

    /* 스크롤백 카운트 ≥ 1 */
    CHECK(s.sb_count >= 1, "scrollback count >= 1");

    screen_destroy(&s);
    printf("[PASS] test_scrollback\n");
}

static void test_alt_screen(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    /* 메인 화면에 텍스트 */
    feed(&s, "Main");

    /* 대체 화면 진입: ESC[?1049h */
    feed(&s, "\x1b[?1049h");
    CHECK(s.use_alt == 1, "alt screen active");
    /* 대체 화면은 비어있어야 함 */
    CHECK(cell_at(&s, 0, 0)->codepoint == 0, "alt screen empty");

    feed(&s, "Alt");

    /* 대체 화면 탈출: ESC[?1049l */
    feed(&s, "\x1b[?1049l");
    CHECK(s.use_alt == 0, "main screen restored");
    /* 메인 화면 내용 복원 */
    CHECK(cell_at(&s, 0, 0)->codepoint == 'M', "main screen 'M' restored");

    screen_destroy(&s);
    printf("[PASS] test_alt_screen\n");
}

static void test_cursor_hidden(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 500) == 0);

    CHECK(screen_cursor_hidden(&s) == 0, "cursor visible initially");

    /* ESC[?25l — hide cursor */
    feed(&s, "\x1b[?25l");
    CHECK(screen_cursor_hidden(&s) == 1, "cursor hidden after ?25l");

    /* ESC[?25h — show cursor */
    feed(&s, "\x1b[?25h");
    CHECK(screen_cursor_hidden(&s) == 0, "cursor visible after ?25h");

    screen_destroy(&s);
    printf("[PASS] test_cursor_hidden\n");
}

static void test_scroll_region(void)
{
    screen_t s;
    assert(screen_init(&s, 10, 5, 100) == 0);

    /* 스크롤 영역을 행 2~4 (1-based: ESC[2;4r) 로 설정 */
    feed(&s, "\x1b[2;4r");
    CHECK(s.scroll_top == 1, "scroll_top=1 (0-based)");
    CHECK(s.scroll_bot == 3, "scroll_bot=3 (0-based)");

    /* 영역 내에 텍스트 3줄 채우기 후 LF → 영역 내 스크롤 */
    feed(&s, "\x1b[2;1HA");  /* row2 col1 */
    feed(&s, "\x1b[3;1HB");  /* row3 col1 */
    feed(&s, "\x1b[4;1HC");  /* row4 col1 */
    feed(&s, "\x1b[4;1H\n"); /* LF at bottom of region → scroll up inside region */

    CHECK(cell_at(&s, 0, 1)->codepoint == 'B', "after scroll: row1='B'");
    CHECK(cell_at(&s, 0, 2)->codepoint == 'C', "after scroll: row2='C'");
    CHECK(cell_at(&s, 0, 3)->codepoint == 0,   "after scroll: row3 cleared");

    screen_destroy(&s);
    printf("[PASS] test_scroll_region\n");
}

static void test_insert_delete_lines(void)
{
    screen_t s;
    assert(screen_init(&s, 10, 5, 100) == 0);

    feed(&s, "AAA\r\nBBB\r\nCCC\r\nDDD");

    /* cursor at row 1, insert 1 line (ESC[L) */
    feed(&s, "\x1b[2;1H\x1b[L");

    CHECK(cell_at(&s, 0, 0)->codepoint == 'A', "row0 = 'A' (unchanged)");
    CHECK(cell_at(&s, 0, 1)->codepoint == 0,   "row1 = blank (inserted)");
    CHECK(cell_at(&s, 0, 2)->codepoint == 'B', "row2 = 'B' (shifted down)");
    CHECK(cell_at(&s, 0, 3)->codepoint == 'C', "row3 = 'C' (shifted down)");

    screen_destroy(&s);
    printf("[PASS] test_insert_delete_lines\n");
}

static void test_resize(void)
{
    screen_t s;
    assert(screen_init(&s, 20, 5, 100) == 0);

    feed(&s, "Hello");

    screen_resize(&s, 40, 10);
    CHECK(s.cols == 40, "cols=40 after resize");
    CHECK(s.rows == 10, "rows=10 after resize");
    CHECK(cell_at(&s, 0, 0)->codepoint == 'H', "content preserved after resize");
    CHECK(cell_at(&s, 4, 0)->codepoint == 'o', "content preserved (col4)");

    screen_destroy(&s);
    printf("[PASS] test_resize\n");
}

static void test_wrap(void)
{
    screen_t s;
    assert(screen_init(&s, 5, 5, 100) == 0);

    /* 딱 5글자 — pending_wrap 설정 */
    feed(&s, "ABCDE");
    CHECK(screen_cursor_x(&s) == 4, "cursor at last col after fill");

    /* 한 글자 더 — wrap 발생 */
    feed(&s, "F");
    CHECK(screen_cursor_y(&s) == 1, "wrapped to row 1");
    CHECK(screen_cursor_x(&s) == 1, "cursor x=1 after wrap");
    CHECK(cell_at(&s, 0, 1)->codepoint == 'F', "F at row1 col0");

    screen_destroy(&s);
    printf("[PASS] test_wrap\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* Phase 6 추가 테스트                                                        */
/* ══════════════════════════════════════════════════════════════════════════ */

static void test_wide_char(void)
{
    screen_t s;
    assert(screen_init(&s, 10, 5, 100) == 0);

    /* U+4E2D (中) — CJK, width=2 */
    feed(&s, "\xe4\xb8\xad");  /* UTF-8 for U+4E2D */

    const term_cell_t *lead = cell_at(&s, 0, 0);
    const term_cell_t *cont = cell_at(&s, 1, 0);

    CHECK(lead->codepoint == 0x4E2D,   "wide char codepoint");
    CHECK(lead->attrs & CELL_ATTR_WIDE,"CELL_ATTR_WIDE on lead cell");
    CHECK(cont->attrs & CELL_ATTR_WIDE_CONT, "CELL_ATTR_WIDE_CONT on cont cell");
    CHECK(cont->codepoint == 0,        "cont cell codepoint=0");
    CHECK(screen_cursor_x(&s) == 2,    "cursor advanced by 2 after wide char");

    screen_destroy(&s);
    printf("[PASS] test_wide_char\n");
}

static void test_wide_char_wrap(void)
{
    screen_t s;
    /* 5칸 화면: 4 narrow + wide → wide가 마지막 칸(col4)에서 wrap */
    assert(screen_init(&s, 5, 5, 100) == 0);

    /* ABCD가 col0~3을 채우면 cursor=col4(=cols-1).
     * wide char 도착 → col4에 공간 부족(2칸 필요) → col4 공백, 다음 줄 col0으로 */
    feed(&s, "ABCD\xe4\xb8\xad");

    CHECK(cell_at(&s, 0, 0)->codepoint == 'A', "row0 col0=A");
    CHECK(cell_at(&s, 3, 0)->codepoint == 'D', "row0 col3=D");
    CHECK(cell_at(&s, 4, 0)->codepoint == 0,   "col4 blank (wide pushed to next line)");
    CHECK(cell_at(&s, 0, 1)->codepoint == 0x4E2D, "row1 col0 = wide char");
    CHECK(cell_at(&s, 1, 1)->attrs & CELL_ATTR_WIDE_CONT, "row1 col1 = WIDE_CONT");

    screen_destroy(&s);
    printf("[PASS] test_wide_char_wrap\n");
}

static void test_osc_title(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 100) == 0);

    CHECK(screen_get_title(&s)[0] == '\0', "title empty initially");

    /* OSC 2 ; My Terminal BEL */
    feed(&s, "\x1b]2;My Terminal\x07");
    CHECK(strcmp(screen_get_title(&s), "My Terminal") == 0, "OSC 2 title set");

    /* OSC 0 ; Override Title BEL */
    feed(&s, "\x1b]0;Override\x07");
    CHECK(strcmp(screen_get_title(&s), "Override") == 0, "OSC 0 title set");

    screen_destroy(&s);
    printf("[PASS] test_osc_title\n");
}

static void test_mouse_mode(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 100) == 0);

    CHECK(screen_mouse_mode(&s) == SCREEN_MOUSE_NONE, "mouse mode none initially");

    feed(&s, "\x1b[?1000h");
    CHECK(screen_mouse_mode(&s) == SCREEN_MOUSE_X10, "?1000h = X10 mode");

    feed(&s, "\x1b[?1006h");
    CHECK(screen_mouse_mode(&s) == SCREEN_MOUSE_SGR, "?1006h = SGR mode");

    feed(&s, "\x1b[?1006l");
    CHECK(screen_mouse_mode(&s) == SCREEN_MOUSE_NONE, "?1006l = mode off");

    screen_destroy(&s);
    printf("[PASS] test_mouse_mode\n");
}

static void test_bracketed_paste(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 100) == 0);

    CHECK(screen_bracketed_paste(&s) == 0, "bracketed paste off initially");

    feed(&s, "\x1b[?2004h");
    CHECK(screen_bracketed_paste(&s) == 1, "?2004h = bracketed paste on");

    feed(&s, "\x1b[?2004l");
    CHECK(screen_bracketed_paste(&s) == 0, "?2004l = bracketed paste off");

    screen_destroy(&s);
    printf("[PASS] test_bracketed_paste\n");
}

static void test_cursor_style(void)
{
    screen_t s;
    assert(screen_init(&s, 80, 24, 100) == 0);

    CHECK(screen_cursor_style(&s) == CURSOR_STYLE_DEFAULT, "default cursor style");

    /* ESC [ 2   q = block (steady) — intermediate is space (0x20) */
    feed(&s, "\x1b[2 q");
    CHECK(screen_cursor_style(&s) == CURSOR_STYLE_BLOCK, "style=2 block");

    feed(&s, "\x1b[6 q");
    CHECK(screen_cursor_style(&s) == CURSOR_STYLE_BAR, "style=6 bar");

    feed(&s, "\x1b[0 q");
    CHECK(screen_cursor_style(&s) == CURSOR_STYLE_DEFAULT, "style=0 default");

    screen_destroy(&s);
    printf("[PASS] test_cursor_style\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    test_basic_print();
    test_newline();
    test_sgr_bold_color();
    test_sgr_256color();
    test_sgr_truecolor();
    test_cursor_movement();
    test_erase_display();
    test_erase_line();
    test_scrollback();
    test_alt_screen();
    test_cursor_hidden();
    test_scroll_region();
    test_insert_delete_lines();
    test_resize();
    test_wrap();
    /* Phase 6 */
    test_wide_char();
    test_wide_char_wrap();
    test_osc_title();
    test_mouse_mode();
    test_bracketed_paste();
    test_cursor_style();

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
