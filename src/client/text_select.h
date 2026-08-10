#ifndef TESSERA_TEXT_SELECT_H
#define TESSERA_TEXT_SELECT_H

#include <stdint.h>
#include "screen.h"

/*
 * 마우스 선택 지오메트리 / 텍스트 추출 순수 헬퍼.
 * 전역 상태에 의존하지 않고 screen_t 와 좌표만 받는다(선택 상태 자체는 main.c
 * 가 소유). 절대 행 인덱스(LI, scroll epoch 기준)를 좌표계로 사용한다.
 */

/* 단어 경계 판정: 영숫자·'_'·비-ASCII(≥0x80)는 워드 문자. */
int is_word_codepoint(uint32_t cp);

/* (li, col) 을 선택 단위로 확장. mode 0=셀, 1=단어, 2=라인. */
void expand_range(const screen_t *s, int64_t li, int col, int mode,
                  int *out_sc, int *out_ec);

/* 두 위치 (li,col) lexicographic 비교: a<b→-1, a==b→0, a>b→+1. */
int pos_cmp(int64_t a_li, int a_col, int64_t b_li, int b_col);

/* 선택 영역(LI 좌표)에서 UTF-8 텍스트 추출. malloc 반환(호출자 free). NULL 가능. */
char *selection_to_text(const screen_t *scr,
                        int sc, int64_t sr_li, int ec, int64_t er_li);

/* view row ↔ 절대 행 인덱스(LI) 변환. */
int64_t li_from_view_row(const screen_t *s, int view_row);
int     view_row_from_li(const screen_t *s, int64_t li);

#endif /* TESSERA_TEXT_SELECT_H */
