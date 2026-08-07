#ifndef TERMEMU_PANE_STORE_H
#define TERMEMU_PANE_STORE_H

#include <stdint.h>
#include "screen.h"

/* 클라이언트가 로컬로 들고 있는 pane 별 화면 상태 레지스트리. */

#define MAX_PANES   16

typedef struct {
    uint32_t  pane_id;
    uint32_t  parent_pane_id;  /* 이 pane 을 만든 부모 pane (0 = 없음). 닫힐 때 포커스 복귀용 */
    screen_t  screen;
    int       used;
} pane_slot_t;

/* pane_id 로 슬롯 조회 (없으면 NULL). */
pane_slot_t *pane_slot_find(uint32_t pane_id);

/* 슬롯 할당(같은 pane_id 가 있으면 재사용). screen 을 cols×rows 로 초기화하며
 * scrollback_lines<=0 이면 1000 을 사용. 빈 슬롯이 없으면 NULL. */
pane_slot_t *pane_slot_alloc(uint32_t pane_id, int cols, int rows, int scrollback_lines);

/* 슬롯 해제 + screen 파괴. */
void pane_slot_free(uint32_t pane_id);

/* 인덱스(0..MAX_PANES-1)로 슬롯 접근. 전체 순회용. 범위 밖이면 NULL. */
pane_slot_t *pane_slot_at(int index);

#endif /* TERMEMU_PANE_STORE_H */
