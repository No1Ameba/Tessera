#ifndef TESSERA_STATUS_BAR_H
#define TESSERA_STATUS_BAR_H

/*
 * 창 하단 상태바 (vim-airline 풍).
 *
 * 별도의 렌더 pass 를 만들지 않고 "1행짜리 터미널 그리드" 로 만들어
 * 기존 gl_renderer_draw_cells() 로 그린다. 상태바는 결국 텍스트라
 * 셀 렌더러를 그대로 재사용하는 편이 셰이더/폰트 경로를 나누는 것보다 깔끔하다.
 *
 * 레이아웃 예약은 호출자(main.c compute_layout_rect)가 담당한다 —
 * 하단 한 행을 pane rect 에서 빼주면 pane 과 겹치지 않는다.
 *
 * 사용:
 *   term_cell_t row[STATUS_BAR_MAX_COLS];
 *   status_bar_build(row, cols, &info, theme);
 *   gl_renderer_draw_cells(r, row, cols, 1, rect, -1,-1,-1,-1);
 */

#include <stdint.h>
#include "../cell.h"
#include "../../common/config.h"

/* 상태바가 차지하는 행 수. */
#define STATUS_BAR_ROWS      1

/* 한 행에 그릴 수 있는 최대 열 수 (버퍼 상한). */
#define STATUS_BAR_MAX_COLS  1024

#define STATUS_BAR_MAX_WINDOWS 32

typedef struct {
    uint32_t id;
    char     name[64];
    int      pane_count;
    int      activity;    /* 비활성 window 에 새 출력이 있었으면 1 → '*' 표시 */
} status_bar_window_t;

typedef struct {
    const char *session_name;

    status_bar_window_t windows[STATUS_BAR_MAX_WINDOWS];
    int                 window_count;
    int                 active_window;   /* windows[] 내 인덱스 */

    /* 활성 pane 정보 */
    uint32_t active_pane_id;
    int      pane_cols, pane_rows;

    /* 상태 플래그 */
    int      remote;        /* SSH 브리지로 붙은 세션이면 1 */
    int      scrollback;    /* 스크롤백 뷰 오프셋 (0 = 라이브) */
    int      selecting;     /* 텍스트 선택 중이면 1 */
} status_bar_info_t;

/*
 * 상태바 한 행을 cells[0..cols-1] 에 채운다.
 * cols 는 STATUS_BAR_MAX_COLS 로 클램프된다.
 *
 * 레이아웃: 왼쪽에 세션명 + window 목록, 오른쪽에 pane 정보/상태.
 * 폭이 모자라면 오른쪽 블록부터 생략하고, 그래도 모자라면 왼쪽을 잘라낸다.
 */
void status_bar_build(term_cell_t *cells, int cols,
                       const status_bar_info_t *info,
                       const tessera_theme_t *theme);

#endif /* TESSERA_STATUS_BAR_H */
