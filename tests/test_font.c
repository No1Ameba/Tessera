/*
 * test_font.c — Unit tests for FreeType + HarfBuzz font rasterization.
 * Requires DejaVu Sans Mono (standard Debian/Ubuntu package fonts-dejavu-core).
 */
#define _POSIX_C_SOURCE 200809L  /* popen/pclose for the fallback test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/client/renderer/font.h"

#ifdef _WIN32
/* Consolas — Windows 에 항상 설치되는 모노스페이스 폰트. */
#define FONT_PATH "C:\\Windows\\Fonts\\consola.ttf"
#else
#define FONT_PATH "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#endif
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
    CHECK(f != NULL, "font_face_load succeeds for " FONT_PATH);
    /* 로드 실패 시 이후 접근은 전부 NULL 역참조 → 세그폴트. 여기서 끊는다. */
    if (!f) return;

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

/* Resolve a font path via fontconfig, mirroring the client's resolve_font_path.
 * Returns 1 with an absolute path in out, 0 if nothing usable was found. */
static int fc_resolve(const char *pattern, char *out, size_t outsz)
{
#ifdef _WIN32
    /* fontconfig 가 없으므로 폴백 체인 테스트는 Windows 에서 건너뛴다. */
    (void)pattern; (void)out; (void)outsz;
    return 0;
#else
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "fc-match --format='%%{file}' '%s' 2>/dev/null", pattern);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    size_t n = fread(out, 1, outsz - 1, p);
    pclose(p);
    if (n == 0) return 0;
    out[n] = '\0';
    if (out[n - 1] == '\n') out[--n] = '\0';
    return out[0] == '/';
#endif
}

static void test_fallback_chain(void)
{
    font_face_t *f = font_face_load(FONT_PATH, FONT_SIZE, FONT_DPI);
    if (!f) { g_fail++; return; }

    /* API contract. */
    CHECK(font_face_add_fallback(NULL, FONT_PATH) == -1, "add_fallback(NULL face) rejected");
    CHECK(font_face_add_fallback(f, NULL) == -1,          "add_fallback(NULL path) rejected");
    CHECK(font_face_add_fallback(f, "/nonexistent/x.ttf") == -1, "add_fallback(bad path) rejected");

    /* DejaVu Sans Mono lacks Hangul; U+AC00 must still rasterize (as the
     * primary's .notdef box) rather than error — no silently-failed glyph. */
    uint8_t *bmp = NULL; glyph_metrics_t m = {0};
    int ret = font_rasterize(f, 0xAC00, &bmp, &m);
    CHECK(ret == 0, "rasterize U+AC00 succeeds without coverage (.notdef)");
    font_bitmap_free(bmp);

    /* Attach a Korean-capable fallback (when the host has one) and verify the
     * Hangul syllable '가' now rasterizes to a real, non-empty glyph. */
    char ko_path[512];
    if (fc_resolve(":lang=ko", ko_path, sizeof ko_path) &&
        strcmp(ko_path, FONT_PATH) != 0) {
        CHECK(font_face_add_fallback(f, ko_path) == 0, "add Korean fallback succeeds");

        bmp = NULL; memset(&m, 0, sizeof m);
        ret = font_rasterize(f, 0xAC00, &bmp, &m);
        CHECK(ret == 0,       "rasterize '가' via fallback succeeds");
        CHECK(bmp != NULL,    "'가' has a non-empty bitmap via fallback");
        CHECK(m.width > 0,    "'가' bitmap width > 0 via fallback");
        CHECK(m.height > 0,   "'가' bitmap height > 0 via fallback");
        fprintf(stderr, "  fallback: '가' %ux%u from %s\n", m.width, m.height, ko_path);
        font_bitmap_free(bmp);

        /* Primary coverage must still win: 'A' comes from the primary face. */
        bmp = NULL; memset(&m, 0, sizeof m);
        ret = font_rasterize(f, 'A', &bmp, &m);
        CHECK(ret == 0 && m.width > 0, "primary glyph 'A' still rasterizes with fallback attached");
        font_bitmap_free(bmp);
    } else {
        fprintf(stderr, "  SKIP: no Korean fallback font on host (fc-match :lang=ko)\n");
    }

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
    test_cell_metrics_consistent();
    test_fallback_chain();

    printf("font: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
