#include "../src/common/utf8.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── 미니 테스트 프레임워크 ─────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do {                                    \
    if (cond) { printf("  PASS: %s\n", msg); g_pass++;           \
    } else    { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

#define TEST(name) printf("\n[%s]\n", name)

/* ─── utf8_encode ────────────────────────────────────────────────────────── */

static void test_encode_ascii(void) {
    TEST("encode: ASCII");
    char buf[4];
    ASSERT(utf8_encode('A', buf) == 1 && buf[0] == 'A',   "U+0041 'A' → 1바이트");
    ASSERT(utf8_encode(0x7F, buf) == 1 && (uint8_t)buf[0] == 0x7F, "U+007F → 1바이트");
    ASSERT(utf8_encode(0,    buf) == 1 && buf[0] == 0,    "U+0000 NUL → 1바이트");
}

static void test_encode_2byte(void) {
    TEST("encode: 2바이트 (U+00C9 É)");
    char buf[4];
    int n = utf8_encode(0x00C9, buf);
    ASSERT(n == 2,                             "길이 2");
    ASSERT((uint8_t)buf[0] == 0xC3 && (uint8_t)buf[1] == 0x89, "바이트 일치");
}

static void test_encode_3byte(void) {
    TEST("encode: 3바이트 (U+AC00 '가')");
    char buf[4];
    int n = utf8_encode(0xAC00, buf);
    ASSERT(n == 3,                                                        "길이 3");
    ASSERT((uint8_t)buf[0]==0xEA && (uint8_t)buf[1]==0xB0 && (uint8_t)buf[2]==0x80,
           "바이트 0xEA 0xB0 0x80");
}

static void test_encode_4byte(void) {
    TEST("encode: 4바이트 (U+1F600 😀)");
    char buf[4];
    int n = utf8_encode(0x1F600, buf);
    ASSERT(n == 4, "길이 4");
    ASSERT((uint8_t)buf[0]==0xF0 && (uint8_t)buf[1]==0x9F &&
           (uint8_t)buf[2]==0x98 && (uint8_t)buf[3]==0x80, "바이트 일치");
}

static void test_encode_invalid(void) {
    TEST("encode: 유효하지 않은 코드 포인트");
    char buf[4];
    ASSERT(utf8_encode(0xD800, buf) == 0, "서로게이트 U+D800 → 0");
    ASSERT(utf8_encode(0x110000, buf) == 0, "범위 초과 U+110000 → 0");
}

/* ─── utf8_decode_one ────────────────────────────────────────────────────── */

static void test_decode_ascii(void) {
    TEST("decode: ASCII");
    uint32_t cp;
    ASSERT(utf8_decode_one("Z", 1, &cp) == 1 && cp == 'Z', "'Z' 디코드");
    ASSERT(utf8_decode_one("", 0, &cp) == 0,               "빈 버퍼 → 0");
}

static void test_decode_hangul(void) {
    TEST("decode: 한글 '가' (3바이트)");
    const char ga[] = { (char)(unsigned char)0xEA, (char)(unsigned char)0xB0, (char)(unsigned char)0x80 };
    uint32_t cp;
    int n = utf8_decode_one(ga, 3, &cp);
    ASSERT(n == 3,         "소비 바이트 3");
    ASSERT(cp == 0xAC00,   "U+AC00");
}

static void test_decode_emoji(void) {
    TEST("decode: 이모지 U+1F600 (4바이트)");
    const char e[] = { (char)(unsigned char)0xF0, (char)(unsigned char)0x9F, (char)(unsigned char)0x98, (char)(unsigned char)0x80 };
    uint32_t cp;
    ASSERT(utf8_decode_one(e, 4, &cp) == 4 && cp == 0x1F600, "U+1F600");
}

static void test_decode_invalid_lead(void) {
    TEST("decode: 연속 바이트를 선두로 사용 → U+FFFD");
    const char bad[] = { (char)(unsigned char)0x80 };
    uint32_t cp;
    ASSERT(utf8_decode_one(bad, 1, &cp) == 1 && cp == 0xFFFD, "0x80 → U+FFFD");
}

static void test_decode_truncated(void) {
    TEST("decode: 잘린 시퀀스 → U+FFFD");
    const char trunc[] = { (char)(unsigned char)0xEA, (char)(unsigned char)0xB0 };  /* '가' 에서 마지막 바이트 없음 */
    uint32_t cp;
    ASSERT(utf8_decode_one(trunc, 2, &cp) == 1 && cp == 0xFFFD, "잘린 3바이트 → U+FFFD");
}

static void test_decode_roundtrip(void) {
    TEST("decode: encode → decode 왕복");
    uint32_t cps[] = { 'A', 0x00C9, 0xAC00, 0x1F600, 0x4E2D };
    for (int i = 0; i < 5; i++) {
        char buf[4];
        int n = utf8_encode(cps[i], buf);
        uint32_t out;
        utf8_decode_one(buf, (size_t)n, &out);
        ASSERT(out == cps[i], "왕복 일치");
    }
}

/* ─── utf8_char_width ────────────────────────────────────────────────────── */

static void test_width_ascii(void) {
    TEST("width: ASCII");
    ASSERT(utf8_char_width('A') == 1,  "'A' → 1");
    ASSERT(utf8_char_width(' ') == 1,  "' ' → 1");
    ASSERT(utf8_char_width(0x7E) == 1, "'~' → 1");
}

static void test_width_control(void) {
    TEST("width: 제어 문자 → 0");
    ASSERT(utf8_char_width(0)    == 0, "NUL → 0");
    ASSERT(utf8_char_width('\n') == 0, "LF  → 0");
    ASSERT(utf8_char_width('\t') == 0, "HT  → 0");
    ASSERT(utf8_char_width(0x1B) == 0, "ESC → 0");
}

static void test_width_combining(void) {
    TEST("width: 결합 문자 → 0");
    ASSERT(utf8_char_width(0x0300) == 0, "U+0300 combining grave → 0");
    ASSERT(utf8_char_width(0x0308) == 0, "U+0308 combining diaeresis → 0");
    ASSERT(utf8_char_width(0x200B) == 0, "U+200B zero-width space → 0");
}

static void test_width_hangul(void) {
    TEST("width: 한글 → 2");
    ASSERT(utf8_char_width(0xAC00) == 2, "U+AC00 '가' → 2");
    ASSERT(utf8_char_width(0xD7A3) == 2, "U+D7A3 '힣' → 2");
    ASSERT(utf8_char_width(0x1100) == 2, "U+1100 Hangul Jamo → 2");
}

static void test_width_cjk(void) {
    TEST("width: CJK 한자 → 2");
    ASSERT(utf8_char_width(0x4E2D) == 2, "U+4E2D '中' → 2");
    ASSERT(utf8_char_width(0x65E5) == 2, "U+65E5 '日' → 2");
    ASSERT(utf8_char_width(0x4E00) == 2, "U+4E00 '一' → 2");
}

static void test_width_emoji(void) {
    TEST("width: 이모지 → 2");
    ASSERT(utf8_char_width(0x1F600) == 2, "U+1F600 😀 → 2");
    ASSERT(utf8_char_width(0x1F4BB) == 2, "U+1F4BB 💻 → 2");
    ASSERT(utf8_char_width(0x1F680) == 2, "U+1F680 🚀 → 2");
}

static void test_width_latin_ext(void) {
    TEST("width: 라틴 확장 → 1");
    ASSERT(utf8_char_width(0x00C9) == 1, "U+00C9 'É' → 1");
    ASSERT(utf8_char_width(0x00E0) == 1, "U+00E0 'à' → 1");
    ASSERT(utf8_char_width(0x0430) == 1, "U+0430 'а' (키릴) → 1");
}

static void test_width_fullwidth(void) {
    TEST("width: 전각 폼 → 2");
    ASSERT(utf8_char_width(0xFF21) == 2, "U+FF21 'Ａ' fullwidth → 2");
    ASSERT(utf8_char_width(0xFF01) == 2, "U+FF01 '！' fullwidth → 2");
}

/* ─── utf8_str_width / utf8_cp_count ─────────────────────────────────────── */

static void test_str_width(void) {
    TEST("str_width");
    /* "AB" = 2열, "가나" = 4열, "A가B" = 4열 */
    ASSERT(utf8_str_width("AB") == 2, "\"AB\" → 2");

    /* "가나" in UTF-8 */
    const char gana[] = { (char)(unsigned char)0xEA,(char)(unsigned char)0xB0,(char)(unsigned char)0x80,
                          (char)(unsigned char)0xEB,(char)(unsigned char)0x82,(char)(unsigned char)0x98, 0 };
    ASSERT(utf8_str_width(gana) == 4, "\"가나\" → 4");

    ASSERT(utf8_str_width("") == 0, "빈 문자열 → 0");
}

static void test_cp_count(void) {
    TEST("cp_count");
    ASSERT(utf8_cp_count("ABC") == 3, "\"ABC\" → 3");

    /* "가나다" = 3 코드 포인트 */
    const char gnd[] = { (char)(unsigned char)0xEA,(char)(unsigned char)0xB0,(char)(unsigned char)0x80,
                         (char)(unsigned char)0xEB,(char)(unsigned char)0x82,(char)(unsigned char)0x98,
                         (char)(unsigned char)0xEB,(char)(unsigned char)0x8B,(char)(unsigned char)0xA4, 0 };
    ASSERT(utf8_cp_count(gnd) == 3, "\"가나다\" → 3");

    ASSERT(utf8_cp_count("") == 0, "빈 문자열 → 0");
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== UTF-8 유틸리티 테스트 ===\n");

    test_encode_ascii();
    test_encode_2byte();
    test_encode_3byte();
    test_encode_4byte();
    test_encode_invalid();

    test_decode_ascii();
    test_decode_hangul();
    test_decode_emoji();
    test_decode_invalid_lead();
    test_decode_truncated();
    test_decode_roundtrip();

    test_width_ascii();
    test_width_control();
    test_width_combining();
    test_width_hangul();
    test_width_cjk();
    test_width_emoji();
    test_width_latin_ext();
    test_width_fullwidth();

    test_str_width();
    test_cp_count();

    printf("\n결과: %d 통과 / %d 실패\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
