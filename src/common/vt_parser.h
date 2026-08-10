#ifndef TESSERA_VT_PARSER_H
#define TESSERA_VT_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * VT 파서 — Paul Williams 상태 머신 기반
 * https://www.vt100.net/emu/dec_ansi_parser
 *
 * 구현된 상태:
 *   Ground → Escape → EscapeIntermediate
 *                   → CsiEntry → CsiParam → CsiIntermediate
 *                                          → CsiIgnore
 *                   → OscString
 */

#define VT_MAX_PARAMS         16
#define VT_MAX_INTERMEDIATES   4
#define VT_MAX_OSC_LEN      4096

/* ─── 상태 ──────────────────────────────────────────────────────────────── */

typedef enum {
    VT_STATE_GROUND = 0,
    VT_STATE_ESCAPE,
    VT_STATE_ESCAPE_INTERMEDIATE,
    VT_STATE_CSI_ENTRY,
    VT_STATE_CSI_PARAM,
    VT_STATE_CSI_INTERMEDIATE,
    VT_STATE_CSI_IGNORE,
    VT_STATE_OSC_STRING,
} vt_state_t;

/* ─── 콜백 ──────────────────────────────────────────────────────────────── */

typedef struct {
    /*
     * 출력 가능한 문자. codepoint 는 UTF-8 디코드된 유니코드 코드 포인트.
     * ASCII (< 0x80) 포함, 멀티바이트 시퀀스는 완전히 수신된 뒤 한 번 호출됨.
     */
    void (*print)(void *ctx, uint32_t codepoint);

    /*
     * C0 제어 문자 (0x00–0x1F). 예: \r \n \t \a \b
     */
    void (*execute)(void *ctx, uint8_t byte);

    /*
     * CSI 시퀀스 완료. 예: ESC[1;32m → final_byte='m', params=[1,32]
     *
     * params[i] == -1  : 해당 파라미터가 명시되지 않음 (기본값 사용)
     * params[i] >= 0   : 명시된 정수 값
     * intermediates    : 중간 바이트 문자열 (예: "?" in ESC[?1049h), NUL-terminated
     */
    void (*csi_dispatch)(void *ctx,
                         const int  *params,        size_t params_len,
                         const char *intermediates, size_t intermediates_len,
                         uint8_t     final_byte);

    /*
     * ESC 시퀀스 완료. 예: ESC(B → final_byte='B', intermediates="("
     */
    void (*esc_dispatch)(void *ctx,
                         const char *intermediates, size_t intermediates_len,
                         uint8_t     final_byte);

    /*
     * OSC 문자열 완료 (BEL 또는 ST 로 종료).
     * payload 는 NUL-terminated. 예: "0;My Title" (OSC 0)
     */
    void (*osc_dispatch)(void *ctx, const char *payload, size_t payload_len);
} vt_callbacks_t;

/* ─── 파서 구조체 ────────────────────────────────────────────────────────── */

typedef struct {
    vt_state_t state;

    /* CSI / ESC 파라미터 누적 */
    int    params[VT_MAX_PARAMS];
    size_t params_len;

    /* 중간 바이트 (NUL-terminated) */
    char   intermediates[VT_MAX_INTERMEDIATES + 1];
    size_t intermediates_len;

    /* OSC 페이로드 버퍼 (NUL-terminated) */
    char   osc_buf[VT_MAX_OSC_LEN + 1];
    size_t osc_len;
    bool   osc_esc_pending;  /* OSC 내에서 ESC 를 받았을 때 ST 판별 대기 */

    /* UTF-8 멀티바이트 디코드 상태 */
    uint32_t utf8_codepoint;
    int      utf8_remaining;

    /* 콜백 및 사용자 컨텍스트 */
    vt_callbacks_t cb;
    void          *ctx;
} vt_parser_t;

/* ─── 공개 API ───────────────────────────────────────────────────────────── */

/* 파서 초기화. cb 의 함수 포인터는 NULL 가능 (해당 이벤트 무시). */
void vt_parser_init(vt_parser_t *p, const vt_callbacks_t *cb, void *ctx);

/* PTY 에서 읽은 raw 바이트 스트림을 파서에 공급. */
void vt_parser_feed(vt_parser_t *p, const uint8_t *data, size_t len);

/* 파서를 Ground 상태로 초기화 (PTY 리셋 시 사용). 콜백/컨텍스트는 유지. */
void vt_parser_reset(vt_parser_t *p);

#endif /* TESSERA_VT_PARSER_H */
