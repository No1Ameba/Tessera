#include "vt_parser.h"
#include <string.h>

/* ─── UTF-8 디코드 헬퍼 ─────────────────────────────────────────────────── */

/*
 * 선행 바이트(leading byte) 이후에 오는 연속 바이트 수를 반환.
 *   0  → ASCII (연속 바이트 없음)
 *  -1  → 유효하지 않은 선행 바이트
 */
static int utf8_continuation_count(uint8_t b) {
    if (b < 0x80) return  0;
    if (b < 0xC0) return -1;  /* 연속 바이트를 선행으로 사용 → 오류 */
    if (b < 0xE0) return  1;
    if (b < 0xF0) return  2;
    if (b < 0xF8) return  3;
    return -1;
}

/* 선행 바이트에서 코드 포인트 비트를 추출하기 위한 마스크 */
static const uint8_t UTF8_LEAD_MASK[4] = { 0x7F, 0x1F, 0x0F, 0x07 };

/* ─── 내부 액션 ─────────────────────────────────────────────────────────── */

static void do_clear(vt_parser_t *p) {
    p->params_len        = 0;
    p->params[0]         = -1;
    p->intermediates_len = 0;
    p->intermediates[0]  = '\0';
}

static void do_collect(vt_parser_t *p, uint8_t byte) {
    if (p->intermediates_len < VT_MAX_INTERMEDIATES) {
        p->intermediates[p->intermediates_len++] = (char)byte;
        p->intermediates[p->intermediates_len]   = '\0';
    }
    /* 오버플로우: 조용히 버림 — 시퀀스가 비정상임을 나타냄 */
}

static void do_param(vt_parser_t *p, uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        /* 첫 번째 파라미터 시작 */
        if (p->params_len == 0) {
            p->params_len = 1;
            p->params[0]  = 0;
        }
        int idx = (int)p->params_len - 1;
        if (idx < VT_MAX_PARAMS) {
            if (p->params[idx] < 0) p->params[idx] = 0;
            /* 비정상적으로 큰 값 방어 */
            if (p->params[idx] < 65535)
                p->params[idx] = p->params[idx] * 10 + (byte - '0');
        }
    } else {
        /* ';' 또는 ':' — 파라미터 구분자 */
        if (p->params_len == 0) {
            p->params[0] = -1;
            p->params_len = 1;
        }
        if (p->params_len < VT_MAX_PARAMS) {
            p->params[p->params_len] = -1;
            p->params_len++;
        }
    }
}

static void fire_csi(vt_parser_t *p, uint8_t final_byte) {
    if (p->cb.csi_dispatch)
        p->cb.csi_dispatch(p->ctx,
                           p->params,        p->params_len,
                           p->intermediates, p->intermediates_len,
                           final_byte);
}

static void fire_esc(vt_parser_t *p, uint8_t final_byte) {
    if (p->cb.esc_dispatch)
        p->cb.esc_dispatch(p->ctx,
                           p->intermediates, p->intermediates_len,
                           final_byte);
}

static void fire_osc(vt_parser_t *p) {
    if (p->cb.osc_dispatch)
        p->cb.osc_dispatch(p->ctx, p->osc_buf, p->osc_len);
    p->osc_len    = 0;
    p->osc_buf[0] = '\0';
}

static void osc_put(vt_parser_t *p, uint8_t byte) {
    if (p->osc_len < VT_MAX_OSC_LEN) {
        p->osc_buf[p->osc_len++] = (char)byte;
        p->osc_buf[p->osc_len]   = '\0';
    }
}

/* ─── 바이트 처리 (상태 머신 핵심) ─────────────────────────────────────── */

static void process_byte(vt_parser_t *p, uint8_t byte) {

    /* ── UTF-8 연속 바이트 처리 (Ground 상태 내부) ──────────────────────── */
    if (p->utf8_remaining > 0) {
        if (p->state == VT_STATE_GROUND && (byte & 0xC0) == 0x80) {
            p->utf8_codepoint = (p->utf8_codepoint << 6) | (byte & 0x3F);
            if (--p->utf8_remaining == 0) {
                if (p->cb.print) p->cb.print(p->ctx, p->utf8_codepoint);
            }
            return;
        }
        /* 연속 바이트가 와야 할 자리에 다른 바이트 → 대체 문자 출력 후 계속 */
        if (p->cb.print) p->cb.print(p->ctx, 0xFFFD);
        p->utf8_remaining = 0;
        p->utf8_codepoint = 0;
    }

    /* ── Anywhere 전이 ───────────────────────────────────────────────────── */

    /* CAN(0x18) / SUB(0x1A): 현재 시퀀스 취소 */
    if (byte == 0x18 || byte == 0x1A) {
        if (p->cb.execute) p->cb.execute(p->ctx, byte);
        p->state = VT_STATE_GROUND;
        return;
    }

    /* ESC(0x1B): 새 이스케이프 시퀀스 시작 (OSC 내부는 ST 판별을 위해 별도 처리) */
    if (byte == 0x1B && p->state != VT_STATE_OSC_STRING) {
        do_clear(p);
        p->state = VT_STATE_ESCAPE;
        return;
    }

    /* ── 상태별 처리 ─────────────────────────────────────────────────────── */
    switch (p->state) {

    /* ═══════════════════════════════════════════════════════════════════════
     * GROUND
     * 기본 상태. 일반 텍스트 출력 또는 C0 제어 문자 처리.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_GROUND:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte == 0x7F) {
            /* DEL: 무시 */
        } else if (byte < 0x80) {
            if (p->cb.print) p->cb.print(p->ctx, (uint32_t)byte);
        } else {
            /* UTF-8 멀티바이트 시퀀스 시작 */
            int cont = utf8_continuation_count(byte);
            if (cont <= 0) {
                /* 유효하지 않은 바이트: 대체 문자 */
                if (p->cb.print) p->cb.print(p->ctx, 0xFFFD);
            } else {
                p->utf8_codepoint = byte & UTF8_LEAD_MASK[cont];
                p->utf8_remaining = cont;
            }
        }
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * ESCAPE
     * ESC 를 받은 직후. 다음 바이트로 시퀀스 종류 결정.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_ESCAPE:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte == 0x1B) {
            do_clear(p);  /* ESC ESC: 새 시퀀스 시작으로 취급 */
        } else if (byte <= 0x2F) {
            /* 중간 바이트 (SP, !, ", ..., /) */
            do_collect(p, byte);
            p->state = VT_STATE_ESCAPE_INTERMEDIATE;
        } else if (byte == 0x5B) {
            /* '[' → CSI */
            do_clear(p);
            p->state = VT_STATE_CSI_ENTRY;
        } else if (byte == 0x5D) {
            /* ']' → OSC */
            p->osc_len         = 0;
            p->osc_buf[0]      = '\0';
            p->osc_esc_pending = false;
            p->state           = VT_STATE_OSC_STRING;
        } else if (byte == 0x50 || byte == 0x58 || byte == 0x5E || byte == 0x5F) {
            /* DCS(P) / SOS(X) / PM(^) / APC(_): 미구현 → Ground */
            p->state = VT_STATE_GROUND;
        } else if (byte <= 0x7E) {
            /* 최종 바이트: ESC 시퀀스 디스패치 */
            fire_esc(p, byte);
            p->state = VT_STATE_GROUND;
        }
        /* 0x7F DEL: 무시 */
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * ESCAPE INTERMEDIATE
     * ESC 이후 중간 바이트(0x20–0x2F)를 수집 중.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_ESCAPE_INTERMEDIATE:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte <= 0x2F) {
            do_collect(p, byte);
        } else if (byte <= 0x7E) {
            fire_esc(p, byte);
            p->state = VT_STATE_GROUND;
        }
        /* 0x7F DEL: 무시 */
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * CSI ENTRY
     * ESC[ 직후. 파라미터 또는 최종 바이트 대기.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_CSI_ENTRY:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte <= 0x2F) {
            /* 중간 바이트 → CSI_INTERMEDIATE */
            do_collect(p, byte);
            p->state = VT_STATE_CSI_INTERMEDIATE;
        } else if ((byte >= 0x30 && byte <= 0x39) || byte == 0x3A || byte == 0x3B) {
            /* '0'–'9', ':', ';' */
            do_param(p, byte);
            p->state = VT_STATE_CSI_PARAM;
        } else if (byte >= 0x3C && byte <= 0x3F) {
            /* Private-use 접두어: '?', '<', '=', '>' */
            do_collect(p, byte);
            p->state = VT_STATE_CSI_PARAM;
        } else if (byte >= 0x40 && byte <= 0x7E) {
            /* 최종 바이트 */
            fire_csi(p, byte);
            p->state = VT_STATE_GROUND;
        }
        /* 0x7F DEL: 무시 */
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * CSI PARAM
     * 파라미터 바이트('0'–'9', ';', ':') 수집 중.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_CSI_PARAM:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte <= 0x2F) {
            do_collect(p, byte);
            p->state = VT_STATE_CSI_INTERMEDIATE;
        } else if ((byte >= 0x30 && byte <= 0x39) || byte == 0x3A || byte == 0x3B) {
            do_param(p, byte);
        } else if (byte >= 0x3C && byte <= 0x3F) {
            /* 파라미터 이후 private-use 바이트: 비정상 → Ignore */
            p->state = VT_STATE_CSI_IGNORE;
        } else if (byte >= 0x40 && byte <= 0x7E) {
            fire_csi(p, byte);
            p->state = VT_STATE_GROUND;
        }
        /* 0x7F DEL: 무시 */
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * CSI INTERMEDIATE
     * 중간 바이트 수집 후 최종 바이트 대기.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_CSI_INTERMEDIATE:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte <= 0x2F) {
            do_collect(p, byte);
        } else if (byte <= 0x3F) {
            /* 중간 바이트 이후 파라미터 바이트: 비정상 → Ignore */
            p->state = VT_STATE_CSI_IGNORE;
        } else if (byte <= 0x7E) {
            fire_csi(p, byte);
            p->state = VT_STATE_GROUND;
        }
        /* 0x7F DEL: 무시 */
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * CSI IGNORE
     * 비정상 CSI 시퀀스. 최종 바이트까지 바이트를 버림.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_CSI_IGNORE:
        if (byte < 0x20) {
            if (p->cb.execute) p->cb.execute(p->ctx, byte);
        } else if (byte <= 0x3F) {
            /* 무시 */
        } else if (byte <= 0x7E) {
            /* 최종 바이트 도달: 디스패치 없이 Ground 복귀 */
            p->state = VT_STATE_GROUND;
        }
        /* 0x7F DEL: 무시 */
        break;

    /* ═══════════════════════════════════════════════════════════════════════
     * OSC STRING
     * ESC] 이후 페이로드 수집. BEL(0x07) 또는 ST(ESC \)로 종료.
     * ═══════════════════════════════════════════════════════════════════════ */
    case VT_STATE_OSC_STRING:
        if (byte == 0x07) {
            /* BEL: OSC 종료 */
            fire_osc(p);
            p->state = VT_STATE_GROUND;
        } else if (byte == 0x1B) {
            /* ESC: ST(ESC \) 의 첫 바이트일 수 있음 */
            p->osc_esc_pending = true;
        } else if (byte == 0x5C && p->osc_esc_pending) {
            /* '\': ST 완성 → OSC 종료 */
            p->osc_esc_pending = false;
            fire_osc(p);
            p->state = VT_STATE_GROUND;
        } else {
            if (p->osc_esc_pending) {
                /* 앞서 받은 ESC 가 ST 가 아님: 버퍼에 넣음 */
                osc_put(p, 0x1B);
                p->osc_esc_pending = false;
            }
            /* 출력 가능 문자 + 허용된 C0 (HT, LF, CR) */
            if (byte >= 0x20 || byte == 0x09 || byte == 0x0A || byte == 0x0D)
                osc_put(p, byte);
            /* 그 외 C0: 무시 */
        }
        break;
    }
}

/* ─── 공개 API ───────────────────────────────────────────────────────────── */

void vt_parser_init(vt_parser_t *p, const vt_callbacks_t *cb, void *ctx) {
    memset(p, 0, sizeof(*p));
    if (cb) p->cb = *cb;
    p->ctx   = ctx;
    p->state = VT_STATE_GROUND;
    /* -1 은 "명시되지 않음(기본값 사용)" 을 의미 */
    for (int i = 0; i < VT_MAX_PARAMS; i++)
        p->params[i] = -1;
}

void vt_parser_feed(vt_parser_t *p, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        process_byte(p, data[i]);
}

void vt_parser_reset(vt_parser_t *p) {
    vt_callbacks_t cb  = p->cb;
    void          *ctx = p->ctx;
    vt_parser_init(p, &cb, ctx);
}
