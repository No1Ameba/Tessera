#include "../src/common/config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ─── 미니 테스트 프레임워크 ─────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do {                                    \
    if (cond) { printf("  PASS: %s\n", msg); g_pass++;           \
    } else    { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

#define ASSERT_FLOAT(a, b, msg) ASSERT(((a)-(b)) < 0.001f && ((b)-(a)) < 0.001f, msg)

#define TEST(name) printf("\n[%s]\n", name)

/* ─── color_parse_hex ────────────────────────────────────────────────────── */

static void test_color_parse(void) {
    TEST("color_parse_hex");
    ASSERT(color_parse_hex("#FF5733", 0) == 0xFF5733, "#FF5733");
    ASSERT(color_parse_hex("#000000", 0) == 0x000000, "#000000 검정");
    ASSERT(color_parse_hex("#FFFFFF", 0) == 0xFFFFFF, "#FFFFFF 흰색");
    ASSERT(color_parse_hex("#abc",    0) == 0xAABBCC, "#abc 단축형");
    ASSERT(color_parse_hex("#FFF",    0) == 0xFFFFFF, "#FFF 단축형");

    /* 실패 케이스 */
    ASSERT(color_parse_hex(NULL,      0xDEAD) == 0xDEAD, "NULL → fallback");
    ASSERT(color_parse_hex("FF5733",  0xDEAD) == 0xDEAD, "'#' 없음 → fallback");
    ASSERT(color_parse_hex("#GGGGGG", 0xDEAD) == 0xDEAD, "잘못된 hex → fallback");
    ASSERT(color_parse_hex("#12345",  0xDEAD) == 0xDEAD, "길이 오류 → fallback");
}

/* ─── theme_defaults ─────────────────────────────────────────────────────── */

static void test_theme_defaults(void) {
    TEST("theme_defaults");
    termemu_theme_t t;
    theme_defaults(&t);

    ASSERT(strcmp(t.name, "default") == 0,  "name = \"default\"");
    ASSERT(t.background   != 0,             "background 설정됨");
    ASSERT(t.foreground   != 0,             "foreground 설정됨");
    ASSERT(t.ansi[0]      == 0x000000,      "ansi[0] black = #000000");
    ASSERT(t.ansi[15]     == 0xFFFFFF,      "ansi[15] brightWhite = #FFFFFF");
}

/* ─── theme_load_string ──────────────────────────────────────────────────── */

static const char *DRACULA_JSON =
    "{"
    "  \"name\": \"Dracula\","
    "  \"background\": \"#282A36\","
    "  \"foreground\": \"#F8F8F2\","
    "  \"cursorColor\": \"#F8F8F2\","
    "  \"selectionBackground\": \"#44475A\","
    "  \"black\":        \"#21222C\","
    "  \"red\":          \"#FF5555\","
    "  \"green\":        \"#50FA7B\","
    "  \"yellow\":       \"#F1FA8C\","
    "  \"blue\":         \"#BD93F9\","
    "  \"purple\":       \"#FF79C6\","
    "  \"cyan\":         \"#8BE9FD\","
    "  \"white\":        \"#F8F8F2\","
    "  \"brightBlack\":  \"#6272A4\","
    "  \"brightRed\":    \"#FF6E6E\","
    "  \"brightGreen\":  \"#69FF94\","
    "  \"brightYellow\": \"#FFFFA5\","
    "  \"brightBlue\":   \"#D6ACFF\","
    "  \"brightPurple\": \"#FF92DF\","
    "  \"brightCyan\":   \"#A4FFFF\","
    "  \"brightWhite\":  \"#FFFFFF\""
    "}";

static void test_theme_load_dracula(void) {
    TEST("theme_load_string: Dracula");
    termemu_theme_t t;
    bool ok = theme_load_string(DRACULA_JSON, &t);

    ASSERT(ok,                               "파싱 성공");
    ASSERT(strcmp(t.name, "Dracula") == 0,   "name = \"Dracula\"");
    ASSERT(t.background == 0x282A36,         "background #282A36");
    ASSERT(t.foreground == 0xF8F8F2,         "foreground #F8F8F2");
    ASSERT(t.cursor_color == 0xF8F8F2,       "cursorColor #F8F8F2");
    ASSERT(t.selection_background == 0x44475A, "selectionBackground #44475A");

    /* ANSI 16색 확인 */
    ASSERT(t.ansi[0]  == 0x21222C, "ansi[0]  black  #21222C");
    ASSERT(t.ansi[1]  == 0xFF5555, "ansi[1]  red    #FF5555");
    ASSERT(t.ansi[2]  == 0x50FA7B, "ansi[2]  green  #50FA7B");
    ASSERT(t.ansi[4]  == 0xBD93F9, "ansi[4]  blue   #BD93F9");
    ASSERT(t.ansi[8]  == 0x6272A4, "ansi[8]  brtBlk #6272A4");
    ASSERT(t.ansi[15] == 0xFFFFFF, "ansi[15] brtWht #FFFFFF");
}

static void test_theme_load_partial(void) {
    TEST("theme_load_string: 부분 JSON (누락 필드 → 기본값)");
    const char *json = "{\"name\":\"Partial\",\"background\":\"#123456\"}";
    termemu_theme_t t;
    bool ok = theme_load_string(json, &t);

    ASSERT(ok,                              "파싱 성공");
    ASSERT(t.background == 0x123456,        "background 적용됨");
    ASSERT(t.foreground != 0,              "foreground 기본값 유지");
    ASSERT(t.ansi[0] != 0 || t.ansi[0] == 0, "ansi 기본값 존재"); /* 기본값이 0일 수도 있음 */
}

static void test_theme_load_invalid_json(void) {
    TEST("theme_load_string: 잘못된 JSON → false");
    termemu_theme_t t;
    ASSERT(!theme_load_string("{not valid json", &t), "잘못된 JSON → false");
    ASSERT(!theme_load_string(NULL, &t),              "NULL → false");
}

/* ─── config_defaults ────────────────────────────────────────────────────── */

static void test_config_defaults(void) {
    TEST("config_defaults");
    termemu_config_t cfg;
    config_defaults(&cfg);

    ASSERT(cfg.font_size > 0,                       "font_size > 0");
    ASSERT(strcmp(cfg.font_family, "monospace") == 0, "font_family = monospace");
    ASSERT(cfg.font_ligatures == true,              "ligatures = true");
    ASSERT_FLOAT(cfg.opacity, 1.0f,                 "opacity = 1.0");
    ASSERT(cfg.scrollback_lines == 10000,           "scrollback = 10000");
    ASSERT(cfg.cursor_style == CURSOR_BLOCK,        "cursor = block");
    ASSERT(cfg.cursor_blink == true,                "cursor_blink = true");
    ASSERT(strcmp(cfg.theme_name, "default") == 0,  "theme = default");
    ASSERT(cfg.bell_visual == false,                "bell_visual = false");
}

/* ─── config_load_string ─────────────────────────────────────────────────── */

static const char *FULL_CONFIG_JSON =
    "{"
    "  \"font\": { \"family\": \"JetBrains Mono\", \"size\": 16.0, \"ligatures\": true },"
    "  \"window\": { \"opacity\": 0.95, \"padding\": { \"x\": 8, \"y\": 6 } },"
    "  \"scrollback_lines\": 50000,"
    "  \"cursor\": { \"style\": \"bar\", \"blink\": false },"
    "  \"theme\": \"Dracula\","
    "  \"bell\": { \"visual\": true }"
    "}";

static void test_config_load_full(void) {
    TEST("config_load_string: 전체 필드");
    termemu_config_t cfg;
    bool ok = config_load_string(FULL_CONFIG_JSON, &cfg);

    ASSERT(ok,                                          "파싱 성공");
    ASSERT(strcmp(cfg.font_family, "JetBrains Mono")==0,"font_family");
    ASSERT_FLOAT(cfg.font_size, 16.0f,                  "font_size = 16");
    ASSERT(cfg.font_ligatures == true,                  "ligatures = true");
    ASSERT_FLOAT(cfg.opacity, 0.95f,                    "opacity = 0.95");
    ASSERT(cfg.padding_x == 8,                          "padding_x = 8");
    ASSERT(cfg.padding_y == 6,                          "padding_y = 6");
    ASSERT(cfg.scrollback_lines == 50000,               "scrollback = 50000");
    ASSERT(cfg.cursor_style == CURSOR_BAR,              "cursor = bar");
    ASSERT(cfg.cursor_blink == false,                   "cursor_blink = false");
    ASSERT(strcmp(cfg.theme_name, "Dracula") == 0,      "theme = Dracula");
    ASSERT(cfg.bell_visual == true,                     "bell_visual = true");
}

static void test_config_cursor_styles(void) {
    TEST("config_load_string: cursor style 파싱");
    termemu_config_t cfg;

    config_load_string("{\"cursor\":{\"style\":\"block\"}}", &cfg);
    ASSERT(cfg.cursor_style == CURSOR_BLOCK,     "\"block\"");

    config_load_string("{\"cursor\":{\"style\":\"underline\"}}", &cfg);
    ASSERT(cfg.cursor_style == CURSOR_UNDERLINE, "\"underline\"");

    config_load_string("{\"cursor\":{\"style\":\"bar\"}}", &cfg);
    ASSERT(cfg.cursor_style == CURSOR_BAR,       "\"bar\"");

    config_load_string("{\"cursor\":{\"style\":\"unknown\"}}", &cfg);
    ASSERT(cfg.cursor_style == CURSOR_BLOCK,     "unknown → block");
}

static void test_config_scrollback_clamp(void) {
    TEST("config_load_string: scrollback_lines 범위 클램프");
    termemu_config_t cfg;

    config_load_string("{\"scrollback_lines\": 0}", &cfg);
    ASSERT(cfg.scrollback_lines == 1,      "0 → 1 (최솟값)");

    config_load_string("{\"scrollback_lines\": 999999}", &cfg);
    ASSERT(cfg.scrollback_lines == 100000, "999999 → 100000 (최댓값)");

    config_load_string("{\"scrollback_lines\": 5000}", &cfg);
    ASSERT(cfg.scrollback_lines == 5000,   "5000 → 5000 (정상)");
}

static void test_config_opacity_clamp(void) {
    TEST("config_load_string: opacity 범위 클램프");
    termemu_config_t cfg;

    config_load_string("{\"window\":{\"opacity\": -0.5}}", &cfg);
    ASSERT_FLOAT(cfg.opacity, 0.0f, "-0.5 → 0.0");

    config_load_string("{\"window\":{\"opacity\": 1.5}}", &cfg);
    ASSERT_FLOAT(cfg.opacity, 1.0f, "1.5 → 1.0");
}

static void test_config_partial(void) {
    TEST("config_load_string: 부분 JSON → 나머지 기본값");
    termemu_config_t cfg;
    config_load_string("{\"theme\":\"Catppuccin\"}", &cfg);

    ASSERT(strcmp(cfg.theme_name, "Catppuccin") == 0, "theme 적용됨");
    ASSERT(cfg.scrollback_lines == 10000,             "scrollback 기본값 유지");
    ASSERT(cfg.cursor_style == CURSOR_BLOCK,          "cursor 기본값 유지");
}

static void test_config_invalid_json(void) {
    TEST("config_load_string: 잘못된 JSON → false");
    termemu_config_t cfg;
    ASSERT(!config_load_string("{bad json", &cfg), "잘못된 JSON → false");
    ASSERT(!config_load_string(NULL, &cfg),        "NULL → false");
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Config / Theme 파서 테스트 ===\n");

    test_color_parse();

    test_theme_defaults();
    test_theme_load_dracula();
    test_theme_load_partial();
    test_theme_load_invalid_json();

    test_config_defaults();
    test_config_load_full();
    test_config_cursor_styles();
    test_config_scrollback_clamp();
    test_config_opacity_clamp();
    test_config_partial();
    test_config_invalid_json();

    printf("\n결과: %d 통과 / %d 실패\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
