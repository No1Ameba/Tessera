#include "text_select.h"

#include <stdlib.h>
#include <string.h>

#include "cell.h"
#include "../common/utf8.h"

int is_word_codepoint(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp >= 0x80) return 1;
    if (cp >= '0' && cp <= '9') return 1;
    if (cp >= 'A' && cp <= 'Z') return 1;
    if (cp >= 'a' && cp <= 'z') return 1;
    if (cp == '_') return 1;
    return 0;
}

/* (li, col) 위치를 선택 단위(mode)에 맞게 확장.
 *   mode 0 = 단일 셀: [col, col]
 *   mode 1 = 단어: 워드 문자로 연결된 좌/우 최대 범위. 워드 아니면 단일 셀.
 *   mode 2 = 라인: [0, cols-1] */
void expand_range(const screen_t *s, int64_t li, int col, int mode,
                  int *out_sc, int *out_ec)
{
    int cols = s->cols;
    if (mode == 2) { *out_sc = 0; *out_ec = cols - 1; return; }

    const term_cell_t *row = screen_row_by_li(s, (uint64_t)li);
    if (mode == 1 && row && col >= 0 && col < cols &&
        is_word_codepoint(row[col].codepoint))
    {
        int sc = col, ec = col;
        while (sc > 0 && is_word_codepoint(row[sc - 1].codepoint)) sc--;
        while (ec < cols - 1 && is_word_codepoint(row[ec + 1].codepoint)) ec++;
        *out_sc = sc; *out_ec = ec;
        return;
    }
    *out_sc = *out_ec = col;
}

/* 두 위치(li, col) 를 lexicographic 비교. a<b 면 -1, a==b 면 0, a>b 면 +1. */
int pos_cmp(int64_t a_li, int a_col, int64_t b_li, int b_col)
{
    if (a_li < b_li) return -1;
    if (a_li > b_li) return 1;
    if (a_col < b_col) return -1;
    if (a_col > b_col) return 1;
    return 0;
}

/*
 * 선택 영역에서 텍스트 추출. 행은 절대 행 인덱스(LI) 로 지정한다.
 * 스크롤백 범위로 거슬러 올라간 선택도 동작한다.
 */
char *selection_to_text(const screen_t *scr,
                        int sc, int64_t sr_li, int ec, int64_t er_li)
{
    /* 정규화: (r0, c0) <= (r1, c1) 가 되도록 */
    int64_t r0_li, r1_li;
    int c0, c1;
    if (sr_li < er_li || (sr_li == er_li && sc <= ec)) {
        r0_li = sr_li; c0 = sc; r1_li = er_li; c1 = ec;
    } else {
        r0_li = er_li; c0 = ec; r1_li = sr_li; c1 = sc;
    }

    int cols = scr->cols;
    int64_t row_span = r1_li - r0_li + 1;
    if (row_span <= 0 || row_span > 1000000) return NULL;  /* sanity */

    size_t max_sz = (size_t)row_span * (size_t)(cols * 4 + 1) + 1;
    char *buf = malloc(max_sz);
    if (!buf) return NULL;
    size_t pos = 0;

    for (int64_t li = r0_li; li <= r1_li; li++) {
        const term_cell_t *row_cells = screen_row_by_li(scr, (uint64_t)li);
        if (!row_cells) {
            /* 유실된 줄 — 빈 줄로 스킵 */
            if (li < r1_li && pos + 1 < max_sz) buf[pos++] = '\n';
            continue;
        }
        int col_start = (li == r0_li) ? c0 : 0;
        int col_end   = (li == r1_li) ? c1 : cols - 1;

        /* trailing whitespace trim */
        int last_nonspace = col_start - 1;
        for (int col = col_start; col <= col_end; col++) {
            const term_cell_t *cell = &row_cells[col];
            if (cell->attrs & CELL_ATTR_WIDE_CONT) continue;
            uint32_t cp = cell->codepoint;
            if (cp && cp != ' ') last_nonspace = col;
        }

        for (int col = col_start; col <= last_nonspace; col++) {
            const term_cell_t *cell = &row_cells[col];
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

        if (li < r1_li && pos + 1 < max_sz)
            buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return buf;
}

/* view row ↔ LI 변환 헬퍼 */
int64_t li_from_view_row(const screen_t *s, int view_row)
{
    return (int64_t)screen_scroll_epoch(s) + view_row - screen_scrollback_offset(s);
}
int view_row_from_li(const screen_t *s, int64_t li)
{
    return (int)(li - (int64_t)screen_scroll_epoch(s) + screen_scrollback_offset(s));
}
