/*
 * test_font.c — Unit tests for FreeType + HarfBuzz font rasterization.
 * Requires DejaVu Sans Mono (standard Debian/Ubuntu package fonts-dejavu-core).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/client/renderer/font.h"

#define FONT_PATH "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FONT_SIZE  12.0f
#define FONT_DPI   96

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { g_pass++; }                                \
        else {                                                  \
            fprintf(stderr, "FAIL [%s:%d] %s\n",               \
                    __FILE__, __LINE__, msg);                   \
            g_fail++;                                           \
        }                                                       \
    } while (0)

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_load_face(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    CHECK(f != NULL, "font_face_load succeeds for DejaVu Sans Mono");

    int cw = font_cell_width(f);
    int ch = font_cell_height(f);
    int asc = font_ascender(f);

    CHECK(cw > 0,  "cell_width > 0");
    CHECK(ch > 0,  "cell_height > 0");
    CHECK(asc > 0, "ascender > 0");
    CHECK(asc < ch, "ascender < cell_height");

    fprintf(stderr, "  font metrics: cell=%dx%d ascender=%d\n", cw, ch, asc);

    font_face_destroy(f);
}

static void test_load_bad_path(void)
{
    font_face_t *f = font_face_load("/nonexistent/font.ttf", FONT_SIZE, FONT_DPI);
    CHECK(f == NULL, "load of nonexistent font returns NULL");
}

static void test_rasterize_ascii(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    /* Rasterize 'A' */
    uint8_t       *bitmap  = NULL;
    glyph_metrics_t metrics = {0};
    int ret = font_rasterize(f, 'A', &bitmap, &metrics);
    CHECK(ret == 0,           "rasterize 'A' succeeds");
    CHECK(bitmap != NULL,     "'A' has a non-empty bitmap");
    CHECK(metrics.width > 0,  "'A' bitmap width > 0");
    CHECK(metrics.height > 0, "'A' bitmap height > 0");
    CHECK(metrics.advance_x > 0, "'A' advance > 0");
    font_bitmap_free(bitmap);

    /* Rasterize space — should have advance but no bitmap. */
    bitmap = NULL;
    memset(&metrics, 0, sizeof metrics);
    ret = font_rasterize(f, ' ', &bitmap, &metrics);
    CHECK(ret == 0,                "rasterize ' ' succeeds");
    CHECK(metrics.advance_x >= 0, "space has non-negative advance");
    font_bitmap_free(bitmap); /* NULL is safe to free */

    font_face_destroy(f);
}

static void test_rasterize_unicode(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    /* 'é' U+00E9 — should be in DejaVu. */
    uint8_t        *bitmap  = NULL;
    glyph_metrics_t metrics = {0};
    int ret = font_rasterize(f, 0x00E9, &bitmap, &metrics);
    CHECK(ret == 0,          "rasterize U+00E9 (é) succeeds");
    CHECK(metrics.width > 0, "é has bitmap width > 0");
    font_bitmap_free(bitmap);

    font_face_destroy(f);
}

static void test_shape_ascii(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    shaped_glyph_t glyphs[32];
    int n = font_shape(f, "Hello", -1, glyphs, 32);
    CHECK(n == 5, "shape 'Hello' → 5 glyphs");
    for (int i = 0; i < n; i++)
        CHECK(glyphs[i].codepoint != 0, "each glyph has non-zero index");

    font_face_destroy(f);
}

static void test_shape_empty(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    shaped_glyph_t glyphs[8];
    int n = font_shape(f, "", 0, glyphs, 8);
    CHECK(n == 0, "shape empty string → 0 glyphs");

    font_face_destroy(f);
}

/* 한글 fallback: DejaVu 에 없는 U+AC00('가') 이 fallback face 로 rasterize 되는지.
 * 후보 폰트가 시스템에 설치돼 있지 않으면 SKIP (경고만 출력, 실패 아님). */
static void test_fallback_cjk(void)
{
    static const char *cjk_candidates[] = {
        "/usr/share/fonts/truetype/nanum/NanumGothicCoding.ttf",
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
        "/usr/share/fonts/truetype/nanum/NanumBarunGothic.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        NULL
    };
    const char *cjk_path = NULL;
    for (int i = 0; cjk_candidates[i]; i++) {
        FILE *fp = fopen(cjk_candidates[i], "rb");
        if (fp) { fclose(fp); cjk_path = cjk_candidates[i]; break; }
    }
    if (!cjk_path) {
        fprintf(stderr, "  [SKIP] no CJK font installed — fallback test skipped\n");
        return;
    }

    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    /* Without fallback: DejaVu has no Hangul, rasterize returns .notdef
     * (width can be 0 or a tofu box — either way not a true glyph). */
    uint8_t        *bitmap  = NULL;
    glyph_metrics_t metrics = {0};
    int ret = font_rasterize(f, 0xAC00 /* '가' */, &bitmap, &metrics);
    CHECK(ret == 0, "rasterize U+AC00 succeeds (no fallback → .notdef)");
    font_bitmap_free(bitmap);

    /* Attach fallback and retry. */
    int add_ret = font_face_add_fallback(f, cjk_path);
    CHECK(add_ret == 0, "font_face_add_fallback succeeds");

    bitmap = NULL;
    memset(&metrics, 0, sizeof metrics);
    ret = font_rasterize(f, 0xAC00, &bitmap, &metrics);
    CHECK(ret == 0, "rasterize U+AC00 succeeds with fallback");
    CHECK(metrics.width > 0 && metrics.height > 0,
           "U+AC00 produces a real glyph bitmap via fallback");
    CHECK(metrics.advance_x > 0, "U+AC00 advance > 0 via fallback");
    font_bitmap_free(bitmap);

    font_face_destroy(f);
}

static void test_cell_metrics_consistent(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    /* Cell width should match the advance of 'M'. */
    uint8_t        *bitmap  = NULL;
    glyph_metrics_t metrics = {0};
    font_rasterize(f, 'M', &bitmap, &metrics);
    font_bitmap_free(bitmap);

    int cell_w = font_cell_width(f);
    CHECK(cell_w > 0, "cell_width > 0");

    /* For a monospace font, M's advance should equal cell width.
     * Allow ±1 pixel for rounding differences. */
    int diff = metrics.advance_x - cell_w;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 1, "M advance ≈ cell_width (±1 pixel)");

    font_face_destroy(f);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    test_load_face();
    test_load_bad_path();
    test_rasterize_ascii();
    test_rasterize_unicode();
    test_shape_ascii();
    test_shape_empty();
    test_fallback_cjk();
    test_cell_metrics_consistent();

    printf("font: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
