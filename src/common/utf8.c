#include "utf8.h"
#include <stddef.h>

/* ─── 인코딩 ────────────────────────────────────────────────────────────── */

int utf8_encode(uint32_t cp, char buf[4]) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        /* U+D800–U+DFFF: 서로게이트 쌍은 유효하지 않음 */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6)  & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;  /* U+10FFFF 초과: 유효하지 않음 */
}

/* ─── 디코딩 ────────────────────────────────────────────────────────────── */

int utf8_decode_one(const char *buf, size_t len, uint32_t *out) {
    if (len == 0) {
        if (out) *out = 0;
        return 0;
    }

    const unsigned char *b = (const unsigned char *)buf;
    uint32_t cp;
    int seq_len;

    if (b[0] < 0x80) {
        if (out) *out = b[0];
        return 1;
    } else if (b[0] < 0xC0) {
        /* 연속 바이트가 선두에 옴: 오류 */
        if (out) *out = 0xFFFD;
        return 1;
    } else if (b[0] < 0xE0) {
        cp = b[0] & 0x1F; seq_len = 2;
    } else if (b[0] < 0xF0) {
        cp = b[0] & 0x0F; seq_len = 3;
    } else if (b[0] < 0xF8) {
        cp = b[0] & 0x07; seq_len = 4;
    } else {
        if (out) *out = 0xFFFD;
        return 1;
    }

    if ((size_t)seq_len > len) {
        /* 버퍼 경계에서 잘림 */
        if (out) *out = 0xFFFD;
        return 1;
    }

    for (int i = 1; i < seq_len; i++) {
        if ((b[i] & 0xC0) != 0x80) {
            /* 유효하지 않은 연속 바이트 */
            if (out) *out = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (b[i] & 0x3F);
    }

    /* 과도하게 긴 인코딩(overlong) 검사 */
    if ((seq_len == 2 && cp < 0x80)  ||
        (seq_len == 3 && cp < 0x800) ||
        (seq_len == 4 && cp < 0x10000)) {
        if (out) *out = 0xFFFD;
        return seq_len;
    }

    /* 서로게이트 / 범위 초과 */
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        if (out) *out = 0xFFFD;
        return seq_len;
    }

    if (out) *out = cp;
    return seq_len;
}

/* ─── 터미널 열 너비 ────────────────────────────────────────────────────── */

/*
 * Unicode East Asian Width 기반 너비 판정.
 * 참조: https://www.unicode.org/reports/tr11/
 *
 * 주요 판정 기준:
 *   W (Wide)         → 2
 *   Na (Narrow)      → 1
 *   A (Ambiguous)    → 1  (터미널 관례상 1로 처리)
 *   N (Neutral)      → 1
 *   Z (Zero-width)   → 0
 */
int utf8_char_width(uint32_t cp) {
    /* NUL */
    if (cp == 0) return 0;

    /* C0 / C1 제어 문자 */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;

    /* ASCII 출력 가능 */
    if (cp < 0x7F) return 1;

    /* 결합 문자 (Zero-width) */
    if ((cp >= 0x0300 && cp <= 0x036F)  ||  /* Combining Diacritical Marks */
        (cp >= 0x0610 && cp <= 0x061A)  ||
        (cp >= 0x064B && cp <= 0x065F)  ||
        (cp >= 0x1DC0 && cp <= 0x1DFF)  ||  /* Combining Diacritical Marks Supplement */
        (cp >= 0x20D0 && cp <= 0x20FF)  ||  /* Combining Marks for Symbols */
        (cp >= 0xFE20 && cp <= 0xFE2F))     /* Combining Half Marks */
        return 0;

    /* Zero-width space / joiner 류 */
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D ||
        cp == 0x2060 || cp == 0xFEFF)
        return 0;

    /* ── 전각(Wide) 범위 ──────────────────────────────────────────────── */

    if (cp >= 0x1100 && cp <= 0x115F) return 2;  /* Hangul Jamo */
    if (cp == 0x2329 || cp == 0x232A) return 2;  /* CJK 괄호 */

    if (cp >= 0x2E80 && cp <= 0x303E) return 2;  /* CJK Radicals, Kangxi, Punctuation */
    if (cp >= 0x3040 && cp <= 0x33FF) return 2;  /* Hiragana, Katakana, Bopomofo, Hangul Compat,
                                                      Kanbun, Radicals Supplement, CJK Compat */
    if (cp >= 0x3400 && cp <= 0x4DBF) return 2;  /* CJK Extension A */
    if (cp >= 0x4E00 && cp <= 0xA4CF) return 2;  /* CJK Unified Ideographs */
    if (cp >= 0xA960 && cp <= 0xA97F) return 2;  /* Hangul Jamo Extended-A */
    if (cp >= 0xAC00 && cp <= 0xD7FF) return 2;  /* Hangul Syllables + Jamo Extended-B */
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2;  /* CJK Compatibility Ideographs */
    if (cp >= 0xFE10 && cp <= 0xFE1F) return 2;  /* Vertical Forms */
    if (cp >= 0xFE30 && cp <= 0xFE6F) return 2;  /* CJK Compatibility Forms, Small Forms */
    if (cp >= 0xFF00 && cp <= 0xFF60) return 2;  /* Fullwidth Forms */
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;  /* Fullwidth Currency/Signs */

    /* Supplementary CJK */
    if (cp >= 0x1B000 && cp <= 0x1B0FF) return 2; /* Kana Supplement */
    if (cp >= 0x1B100 && cp <= 0x1B12F) return 2; /* Kana Extended-A */
    if (cp >= 0x20000 && cp <= 0x2A6DF) return 2; /* CJK Extension B */
    if (cp >= 0x2A700 && cp <= 0x2CEAF) return 2; /* CJK Extension C, D, E */
    if (cp >= 0x2CEB0 && cp <= 0x2EBEF) return 2; /* CJK Extension F */
    if (cp >= 0x2F800 && cp <= 0x2FA1F) return 2; /* CJK Compatibility Supplement */
    if (cp >= 0x30000 && cp <= 0x3134F) return 2; /* CJK Extension G */

    /* Emoji (기본 이모지는 대부분 전각) */
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return 2;
    if (cp >= 0x1FA00 && cp <= 0x1FAFF) return 2;

    return 1;
}

/* ─── 문자열 유틸리티 ───────────────────────────────────────────────────── */

size_t utf8_str_width(const char *str) {
    size_t width = 0;
    size_t len   = 0;
    /* strlen 대신 포인터 이동으로 길이 계산 */
    const char *p = str;
    while (*p) { p++; len++; }

    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode_one(str + i, len - i, &cp);
        width += (size_t)utf8_char_width(cp);
        i += (size_t)(n > 0 ? n : 1);
    }
    return width;
}

size_t utf8_cp_count(const char *str) {
    size_t count = 0;
    size_t len   = 0;
    const char *p = str;
    while (*p) { p++; len++; }

    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode_one(str + i, len - i, &cp);
        count++;
        i += (size_t)(n > 0 ? n : 1);
    }
    return count;
}
