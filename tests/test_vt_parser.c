#include "../src/common/vt_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ─── 미니 테스트 프레임워크 ─────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do {                                   \
    if (cond) {                                                  \
        printf("  PASS: %s\n", msg);                             \
        g_pass++;                                                \
    } else {                                                     \
        printf("  FAIL: %s  (line %d)\n", msg, __LINE__);       \
        g_fail++;                                                \
    }                                                            \
} while (0)

#define TEST(name) printf("\n[%s]\n", name)

/* ─── 콜백 기록 구조체 ───────────────────────────────────────────────────── */

#define MAX_EVENTS 64

typedef enum {
    EV_PRINT, EV_EXECUTE, EV_CSI, EV_ESC, EV_OSC
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t     codepoint;          /* EV_PRINT */
    uint8_t      byte;               /* EV_EXECUTE */
    int          params[VT_MAX_PARAMS];
    size_t       params_len;
    char         intermediates[VT_MAX_INTERMEDIATES + 1];
    uint8_t      final_byte;         /* EV_CSI / EV_ESC */
    char         osc_payload[VT_MAX_OSC_LEN + 1];
    size_t       osc_len;
} event_t;

typedef struct {
    event_t events[MAX_EVENTS];
    size_t  count;
} recorder_t;

static void rec_print(void *ctx, uint32_t cp) {
    recorder_t *r = ctx;
    if (r->count >= MAX_EVENTS) return;
    event_t *e = &r->events[r->count++];
    e->type      = EV_PRINT;
    e->codepoint = cp;
}

static void rec_execute(void *ctx, uint8_t byte) {
    recorder_t *r = ctx;
    if (r->count >= MAX_EVENTS) return;
    event_t *e = &r->events[r->count++];
    e->type = EV_EXECUTE;
    e->byte = byte;
}

static void rec_csi(void *ctx,
                    const int *params, size_t params_len,
                    const char *intermediates, size_t intermediates_len,
                    uint8_t final_byte) {
    recorder_t *r = ctx;
    if (r->count >= MAX_EVENTS) return;
    event_t *e = &r->events[r->count++];
    e->type       = EV_CSI;
    e->params_len = params_len;
    e->final_byte = final_byte;
    for (size_t i = 0; i < params_len && i < VT_MAX_PARAMS; i++)
        e->params[i] = params[i];
    strncpy(e->intermediates, intermediates, VT_MAX_INTERMEDIATES);
    e->intermediates[intermediates_len < VT_MAX_INTERMEDIATES
                     ? intermediates_len : VT_MAX_INTERMEDIATES] = '\0';
    (void)intermediates_len;
}

static void rec_esc(void *ctx,
                    const char *intermediates, size_t intermediates_len,
                    uint8_t final_byte) {
    recorder_t *r = ctx;
    if (r->count >= MAX_EVENTS) return;
    event_t *e = &r->events[r->count++];
    e->type       = EV_ESC;
    e->final_byte = final_byte;
    strncpy(e->intermediates, intermediates, VT_MAX_INTERMEDIATES);
    e->intermediates[intermediates_len < VT_MAX_INTERMEDIATES
                     ? intermediates_len : VT_MAX_INTERMEDIATES] = '\0';
    (void)intermediates_len;
}

static void rec_osc(void *ctx, const char *payload, size_t payload_len) {
    recorder_t *r = ctx;
    if (r->count >= MAX_EVENTS) return;
    event_t *e = &r->events[r->count++];
    e->type    = EV_OSC;
    e->osc_len = payload_len < VT_MAX_OSC_LEN ? payload_len : VT_MAX_OSC_LEN;
    memcpy(e->osc_payload, payload, e->osc_len);
    e->osc_payload[e->osc_len] = '\0';
}

static void recorder_init(recorder_t *r, vt_parser_t *p) {
    memset(r, 0, sizeof(*r));
    vt_callbacks_t cb = {
        .print        = rec_print,
        .execute      = rec_execute,
        .csi_dispatch = rec_csi,
        .esc_dispatch = rec_esc,
        .osc_dispatch = rec_osc,
    };
    vt_parser_init(p, &cb, r);
}

static void feed_str(vt_parser_t *p, const char *s) {
    vt_parser_feed(p, (const uint8_t *)s, strlen(s));
}

static void feed_bytes(vt_parser_t *p, const uint8_t *b, size_t n) {
    vt_parser_feed(p, b, n);
}

/* ─── 테스트 케이스 ──────────────────────────────────────────────────────── */

static void test_ascii_print(void) {
    TEST("ASCII 출력");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "Hi!");

    ASSERT(r.count == 3, "이벤트 3개");
    ASSERT(r.events[0].type == EV_PRINT && r.events[0].codepoint == 'H', "'H' 출력");
    ASSERT(r.events[1].type == EV_PRINT && r.events[1].codepoint == 'i', "'i' 출력");
    ASSERT(r.events[2].type == EV_PRINT && r.events[2].codepoint == '!', "'!' 출력");
}

static void test_c0_execute(void) {
    TEST("C0 제어 문자 (\\r\\n\\t\\a)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\r\n\t\a");

    ASSERT(r.count == 4, "이벤트 4개");
    ASSERT(r.events[0].type == EV_EXECUTE && r.events[0].byte == 0x0D, "CR(0x0D)");
    ASSERT(r.events[1].type == EV_EXECUTE && r.events[1].byte == 0x0A, "LF(0x0A)");
    ASSERT(r.events[2].type == EV_EXECUTE && r.events[2].byte == 0x09, "HT(0x09)");
    ASSERT(r.events[3].type == EV_EXECUTE && r.events[3].byte == 0x07, "BEL(0x07)");
}

static void test_sgr_bold_green(void) {
    TEST("SGR: ESC[1;32m (굵게 + 초록)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b[1;32m");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].type      == EV_CSI,  "CSI 이벤트");
    ASSERT(r.events[0].final_byte == 'm',     "final_byte = 'm'");
    ASSERT(r.events[0].params_len == 2,       "파라미터 2개");
    ASSERT(r.events[0].params[0] == 1,        "params[0] = 1 (굵게)");
    ASSERT(r.events[0].params[1] == 32,       "params[1] = 32 (초록)");
}

static void test_sgr_reset(void) {
    TEST("SGR: ESC[m (파라미터 없음, 전체 초기화)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b[m");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].type       == EV_CSI, "CSI 이벤트");
    ASSERT(r.events[0].final_byte == 'm',    "final_byte = 'm'");
    ASSERT(r.events[0].params_len == 0,      "파라미터 0개 (기본값)");
}

static void test_cursor_move_home(void) {
    TEST("커서 이동: ESC[H (홈)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b[H");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].type       == EV_CSI, "CSI 이벤트");
    ASSERT(r.events[0].final_byte == 'H',    "final_byte = 'H'");
    ASSERT(r.events[0].params_len == 0,      "파라미터 없음");
}

static void test_cursor_move_pos(void) {
    TEST("커서 이동: ESC[5;10H (행5, 열10)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b[5;10H");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].params[0] == 5,  "row = 5");
    ASSERT(r.events[0].params[1] == 10, "col = 10");
}

static void test_private_mode(void) {
    TEST("DEC Private: ESC[?1049h (대체 화면 버퍼 진입)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b[?1049h");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].type       == EV_CSI,  "CSI 이벤트");
    ASSERT(r.events[0].final_byte == 'h',     "final_byte = 'h'");
    ASSERT(r.events[0].params[0]  == 1049,    "params[0] = 1049");
    ASSERT(strcmp(r.events[0].intermediates, "?") == 0, "intermediates = '?'");
}

static void test_osc_title_bel(void) {
    TEST("OSC 0: ESC]0;My Term\\a (BEL 종료)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b]0;My Term\x07");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].type == EV_OSC,                       "OSC 이벤트");
    ASSERT(strcmp(r.events[0].osc_payload, "0;My Term") == 0, "페이로드 일치");
}

static void test_osc_title_st(void) {
    TEST("OSC 0: ESC]0;My Term ESC\\ (ST 종료)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b]0;My Term\x1b\\");

    ASSERT(r.count == 1, "이벤트 1개");
    ASSERT(r.events[0].type == EV_OSC,                       "OSC 이벤트");
    ASSERT(strcmp(r.events[0].osc_payload, "0;My Term") == 0, "페이로드 일치");
}

static void test_utf8_hangul(void) {
    TEST("UTF-8: 한글 '가' (U+AC00, 3바이트)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    /* U+AC00: 0xEA 0xB0 0x80 */
    const uint8_t ga[] = { 0xEA, 0xB0, 0x80 };
    feed_bytes(&p, ga, sizeof(ga));

    ASSERT(r.count == 1,                      "이벤트 1개");
    ASSERT(r.events[0].type == EV_PRINT,      "PRINT 이벤트");
    ASSERT(r.events[0].codepoint == 0xAC00,   "코드 포인트 U+AC00");
}

static void test_utf8_emoji(void) {
    TEST("UTF-8: 이모지 U+1F600 (4바이트)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    /* U+1F600: 0xF0 0x9F 0x98 0x80 */
    const uint8_t emoji[] = { 0xF0, 0x9F, 0x98, 0x80 };
    feed_bytes(&p, emoji, sizeof(emoji));

    ASSERT(r.count == 1,                      "이벤트 1개");
    ASSERT(r.events[0].codepoint == 0x1F600,  "코드 포인트 U+1F600");
}

static void test_utf8_invalid(void) {
    TEST("UTF-8: 유효하지 않은 바이트 → U+FFFD 대체 문자");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    /* 0xFF 는 유효한 UTF-8 선행 바이트가 아님 */
    const uint8_t bad[] = { 0xFF };
    feed_bytes(&p, bad, sizeof(bad));

    ASSERT(r.count == 1,                      "이벤트 1개");
    ASSERT(r.events[0].codepoint == 0xFFFD,   "대체 문자 U+FFFD");
}

static void test_can_cancels_sequence(void) {
    TEST("CAN(0x18): 진행 중인 시퀀스 취소");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    /* ESC[ 시작 후 CAN → 시퀀스 버려지고 Ground 복귀 */
    const uint8_t data[] = { 0x1B, '[', 0x18, 'A' };
    feed_bytes(&p, data, sizeof(data));

    /* CAN 이 execute 로 전달되고, 이후 'A' 는 일반 문자 출력 */
    int found_can = 0, found_A = 0;
    for (size_t i = 0; i < r.count; i++) {
        if (r.events[i].type == EV_EXECUTE && r.events[i].byte == 0x18) found_can = 1;
        if (r.events[i].type == EV_PRINT   && r.events[i].codepoint == 'A') found_A = 1;
    }
    ASSERT(found_can, "CAN execute 이벤트 발생");
    ASSERT(found_A,   "'A' 가 일반 문자로 출력됨");
}

static void test_ignore_malformed_csi(void) {
    TEST("CSI Ignore: ESC[?1;?2m (파라미터 후 private-use → 무시)");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "\x1b[1;?2m");

    /* CSI_IGNORE 로 빠져서 디스패치되지 않아야 함 */
    int csi_fired = 0;
    for (size_t i = 0; i < r.count; i++)
        if (r.events[i].type == EV_CSI) csi_fired = 1;
    ASSERT(!csi_fired, "비정상 CSI 는 디스패치되지 않음");
}

static void test_mixed_sequence(void) {
    TEST("복합: 'ls\\r\\n' + ESC[1;32m + 'ok'");
    recorder_t r; vt_parser_t p;
    recorder_init(&r, &p);

    feed_str(&p, "ls\r\n\x1b[1;32mok");

    /* 이벤트 순서: l, s, CR, LF, CSI(1;32m), o, k */
    ASSERT(r.count == 7, "이벤트 7개");
    ASSERT(r.events[0].codepoint == 'l',  "'l'");
    ASSERT(r.events[1].codepoint == 's',  "'s'");
    ASSERT(r.events[2].byte == 0x0D,      "CR");
    ASSERT(r.events[3].byte == 0x0A,      "LF");
    ASSERT(r.events[4].type == EV_CSI,    "CSI");
    ASSERT(r.events[5].codepoint == 'o',  "'o'");
    ASSERT(r.events[6].codepoint == 'k',  "'k'");
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== VT Parser 테스트 ===\n");

    test_ascii_print();
    test_c0_execute();
    test_sgr_bold_green();
    test_sgr_reset();
    test_cursor_move_home();
    test_cursor_move_pos();
    test_private_mode();
    test_osc_title_bel();
    test_osc_title_st();
    test_utf8_hangul();
    test_utf8_emoji();
    test_utf8_invalid();
    test_can_cancels_sequence();
    test_ignore_malformed_csi();
    test_mixed_sequence();

    printf("\n결과: %d 통과 / %d 실패\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
