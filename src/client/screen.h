#ifndef TESSERA_SCREEN_H
#define TESSERA_SCREEN_H

/*
 * Screen buffer — VT 파서 이벤트를 cell grid로 변환한다.
 *
 * 구조:
 *   - 현재 화면 (cols × rows term_cell_t 배열)
 *   - 대체 화면 (alternate screen buffer, ?1049h/l)
 *   - 스크롤백 링 버퍼 (scrollback_max 줄)
 *
 * 사용 흐름:
 *   1. screen_init()   — 메모리 할당, VT 파서 콜백 등록
 *   2. screen_feed()   — PTY 출력 바이트를 파서에 공급
 *   3. screen_get_cells() — 렌더러에 전달할 셀 포인터 획득
 *   4. screen_destroy() — 자원 해제
 */

#include <stdint.h>
#include <stddef.h>
#include "../common/vt_parser.h"
#include "cell.h"

/* 탭 정지 비트맵 크기. 이보다 넓은 열은 기본 8칸 간격으로 폴백한다. */
#define SCREEN_TAB_MAX 1024

/*
 * DECSC(ESC 7)/DECRC(ESC 8) 로 저장·복원되는 커서 상태 묶음.
 * DEC 스펙상 위치뿐 아니라 SGR 속성·원점 모드(DECOM)·wrap 대기 플래그까지 포함한다.
 */
typedef struct {
    int     cx, cy;
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t attrs;
    int     origin_mode;
    int     pending_wrap;
} screen_cursor_save_t;

typedef struct {
    int cols, rows;
    int scrollback_max;    /* 스크롤백 버퍼 최대 줄 수 */

    /* 현재 활성 셀 그리드 (cells = main or alt) */
    term_cell_t *cells;       /* [rows * cols] */
    term_cell_t *main_cells;  /* 메인 화면 */
    term_cell_t *alt_cells;   /* 대체 화면 */
    int          use_alt;

    /* ?1049 로 대체 화면에 진입할 때 저장해 둔 메인 화면 커서 상태 */
    screen_cursor_save_t saved_1049;

    /* 스크롤백 링 버퍼 [scrollback_max * cols] */
    term_cell_t *scrollback;
    int          sb_head;    /* 다음 쓸 위치 */
    int          sb_count;   /* 저장된 줄 수 */

    /* 스크롤백 뷰어 */
    int          sb_offset;   /* 0 = 현재 화면, N > 0 = N줄 과거로 */
    term_cell_t *view_cells;  /* sb_offset > 0 시 합성된 뷰 버퍼 */

    /* 누적 scrollback push 카운트 — 라이브 스크린이 위로 밀려 첫 행이
     * 스크롤백으로 들어간 총 횟수. 행의 절대 인덱스 (Line Index / LI) 는
     * scroll_epoch + live_row_index 로 계산되어, 스크롤이 발생해도 같은
     * 행에 stable 하게 머무른다 (선택 영역 앵커링 용). */
    uint64_t     scroll_epoch;

    /* 커서 */
    int cx, cy;

    /* SCOSC/SCORC (CSI s / CSI u) — ANSI.SYS 계열, 위치만 저장한다. */
    int sco_cx, sco_cy;

    /* DECSC/DECRC 저장 슬롯 — 화면별로 하나씩([0]=메인, [1]=대체, xterm 동작). */
    screen_cursor_save_t saved[2];

    /* 스크롤 영역 (0-based, inclusive) */
    int scroll_top, scroll_bot;

    /* 현재 SGR 상태 */
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t attrs;

    /* 줄 끝 자동 개행 대기 플래그 */
    int pending_wrap;

    /* DECOM(?6) — 커서 원점이 스크롤 영역 상단. CUP/HVP/VPA/CPR 이 상대 좌표가 된다. */
    int origin_mode;
    /* DECAWM(?7) — 줄 끝 자동 개행 (기본 1). 0 이면 마지막 칸에 덮어쓴다. */
    int autowrap;

    /* 마우스 추적 모드 */
    int mouse_mode;         /* SCREEN_MOUSE_NONE / X10 / SGR */
    int bracketed_paste;    /* ?2004h = 1, ?2004l = 0 */
    int sync_output;        /* ?2026h = 1 (렌더링 보류), ?2026l = 0 */

    /* 커서 상태 */
    int cursor_hidden;      /* ?25l = 1, ?25h = 0 */
    int cursor_style;       /* DECSCUSR 0=기본..6 */

    /* 탭 정지 (HTS/TBC/HT/CHT/CBT). 1 = 해당 열에 정지. */
    uint8_t  tab_stops[SCREEN_TAB_MAX];

    /* REP(CSI b) 용: 마지막으로 출력한 그래픽 문자(0 = 없음). */
    uint32_t last_print_cp;

    /* OSC 창 타이틀 */
    char title[256];

    /* 테마 색상 — screen_apply_theme() 으로 설정 */
    uint8_t default_fg_r, default_fg_g, default_fg_b;
    uint8_t default_bg_r, default_bg_g, default_bg_b;
    uint8_t ansi_palette[16][3];   /* ANSI 0-15 팔레트 */

    /* OSC 52 클립보드 콜백 */
    void (*clipboard_cb)(const char *text, void *user);
    void  *clipboard_user;

    /* 리플라이 콜백 — DSR/CPR/DA 등 터미널이 앱에 되돌려 보내는 바이트를
     * PTY stdin 으로 전달한다(키 입력과 동일 경로). */
    void (*reply_cb)(const char *bytes, size_t len, void *user);
    void  *reply_user;

    /* OSC 8 하이퍼링크 테이블 */
    struct {
        uint16_t id;
        char     url[512];
    }        links[64];
    int      link_count;
    uint16_t active_link_id;  /* 현재 OSC 8 로 활성화된 링크 id (0 = 없음) */
    uint16_t next_link_id;    /* 단조 증가 카운터 */

    /* VT 파서 (임베디드) */
    vt_parser_t parser;
} screen_t;

/* 마우스 모드 상수 */
#define SCREEN_MOUSE_NONE  0
#define SCREEN_MOUSE_X10   1   /* ?1000h — 기본 버튼 보고 */
#define SCREEN_MOUSE_SGR   2   /* ?1006h — SGR 확장 보고 */

/* 커서 스타일 상수 (DECSCUSR ESC[N q) */
#define CURSOR_STYLE_DEFAULT         0
#define CURSOR_STYLE_BLOCK_BLINK     1
#define CURSOR_STYLE_BLOCK           2
#define CURSOR_STYLE_UNDERLINE_BLINK 3
#define CURSOR_STYLE_UNDERLINE       4
#define CURSOR_STYLE_BAR_BLINK       5
#define CURSOR_STYLE_BAR             6

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * 초기화. 내부에서 cells / scrollback 메모리를 heap 할당한다.
 * @return 0 성공, -1 OOM
 */
int  screen_init(screen_t *s, int cols, int rows, int scrollback_lines);
void screen_destroy(screen_t *s);

/* PTY 출력 바이트를 VT 파서에 공급한다. */
void screen_feed(screen_t *s, const uint8_t *data, size_t len);

/*
 * 화면 크기를 변경한다.
 * 내용은 가능한 범위에서 보존(초과분 잘라냄, 부족분 공백).
 */
void screen_resize(screen_t *s, int cols, int rows);

/*
 * 현재 뷰의 셀 배열 포인터를 반환 ([rows * cols]).
 * sb_offset > 0 이면 scrollback + 현재 화면을 합성한 view_cells를 반환.
 */
const term_cell_t *screen_get_cells(screen_t *s);

/*
 * 스크롤백 뷰를 delta 줄 이동한다 (양수=과거, 음수=현재 방향).
 * alt screen 활성 중이거나 scrollback이 없으면 무시.
 */
void screen_scroll_view(screen_t *s, int delta);

/* 스크롤백 뷰를 현재 화면으로 리셋한다. */
void screen_scroll_view_reset(screen_t *s);

/* 현재 스크롤백 오프셋 (0 = 현재 화면). */
int  screen_scrollback_offset(const screen_t *s);

int screen_cursor_x(const screen_t *s);
int screen_cursor_y(const screen_t *s);

/* 커서를 화면 밖으로 가리킬 때 숨길지 여부 (0=표시, 1=숨김). */
int screen_cursor_hidden(const screen_t *s);

/* ── 절대 행 인덱스 (LI = Line Index) API ─────────────────────────────────── */

/*
 * 라이브 스크린이 위로 밀려 첫 행이 스크롤백으로 들어간 누적 횟수.
 * 현재 라이브 스크린 row R 의 LI = screen_scroll_epoch(s) + R.
 * 현재 라이브 top 의 LI = screen_scroll_epoch(s).
 */
uint64_t screen_scroll_epoch(const screen_t *s);

/*
 * 절대 행 인덱스 li 에 해당하는 cell row 포인터 반환.
 * li 가 라이브 스크린/스크롤백 범위를 벗어나 유실된 경우 NULL.
 */
const term_cell_t *screen_row_by_li(const screen_t *s, uint64_t li);

/* 커서 스타일 (CURSOR_STYLE_*). */
int screen_cursor_style(const screen_t *s);

/* 마우스 추적 모드 (SCREEN_MOUSE_*). */
int screen_mouse_mode(const screen_t *s);

/* 브라켓 페이스트 모드 활성 여부 (0/1). */
int screen_bracketed_paste(const screen_t *s);

/* Synchronized Output 모드 (0=즉시 렌더, 1=렌더 보류). */
int screen_sync_output(const screen_t *s);

/* 현재 창 타이틀 (OSC 0/2). 비어있으면 "". */
const char *screen_get_title(const screen_t *s);

/*
 * 테마 색상을 screen에 적용한다 (초기화용).
 * ANSI 팔레트, 기본 fg/bg, 빈 셀 배경색 갱신.
 * screen_init() 직후에 호출한다.
 */
#include "../common/config.h"
void screen_apply_theme(screen_t *s, const tessera_theme_t *t);

/*
 * 팔레트와 기본 fg/bg 색상만 갱신한다 (런타임 리로드용).
 * 기존 셀 내용은 변경하지 않는다.
 * SIGHUP 등 실행 중 테마 교체 시 사용한다.
 */
void screen_update_palette(screen_t *s, const tessera_theme_t *t);

/*
 * OSC 52 클립보드 콜백을 등록한다.
 * 앱(vim 등)이 OSC 52로 클립보드 설정 요청 시 호출된다.
 * text: NULL-terminated UTF-8 문자열 (base64 디코딩 후)
 */
void screen_set_clipboard_cb(screen_t *s,
                              void (*cb)(const char *text, void *user),
                              void *user);

/*
 * 리플라이 콜백 설정 — DSR/CPR/DA 응답 바이트를 PTY stdin 으로 전달할 함수.
 * 미설정 시 해당 질의는 조용히 무시된다.
 */
void screen_set_reply_cb(screen_t *s,
                          void (*cb)(const char *bytes, size_t len, void *user),
                          void *user);

/* link_id 로 저장된 URL 을 조회한다. 없으면 NULL. */
const char *screen_link_url(const screen_t *s, uint16_t link_id);

#endif /* TESSERA_SCREEN_H */
