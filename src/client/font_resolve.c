#define _GNU_SOURCE       /* popen/pclose */
#include "font_resolve.h"

#include <stdio.h>
#include <string.h>

/* ── 폰트 경로 해석 ──────────────────────────────────────────────────────── */

const char *resolve_font_path(const char *name, char *buf, size_t bufsz)
{
    if (name && name[0] == '/') {
        FILE *f = fopen(name, "rb");
        if (f) { fclose(f); return name; }
    }
#ifdef _WIN32
    /* fontconfig(fc-match) 가 없으므로 아래 폴백 목록으로 바로 내려간다. */
    (void)buf; (void)bufsz;
#else
    if (name && name[0]) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "fc-match --format='%%{file}' '%s' 2>/dev/null", name);
        FILE *p = popen(cmd, "r");
        if (p) {
            size_t n = fread(buf, 1, bufsz - 1, p);
            pclose(p);
            if (n > 0) {
                buf[n] = '\0';
                if (buf[n-1] == '\n') buf[--n] = '\0';
                FILE *f = fopen(buf, "rb");
                if (f) { fclose(f); return buf; }
            }
        }
    }
#endif
    static const char *fallbacks[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
        NULL
    };
    for (int i = 0; fallbacks[i]; i++) {
        FILE *f = fopen(fallbacks[i], "rb");
        if (f) { fclose(f); return fallbacks[i]; }
    }
    return name;
}

/*
 * Attach CJK (한중일) fallback fonts so codepoints the primary monospace font
 * lacks — most importantly Hangul (U+AC00–U+D7A3) — still render instead of
 * showing a blank cell. Uses fontconfig (:lang=…) to locate a covering face
 * per script, skipping the primary itself and duplicates (ko/ja/zh commonly
 * resolve to a single pan-CJK file such as Noto Sans CJK).
 */
void add_cjk_fallbacks(font_face_t *font, const char *primary_path)
{
    static const char *patterns[] = { ":lang=ko", ":lang=ja", ":lang=zh-cn", NULL };
    char added[FONT_MAX_FALLBACK][512];
    int  n_added = 0;
    for (int i = 0; patterns[i] && n_added < FONT_MAX_FALLBACK; i++) {
        char buf[512] = {0};
        const char *p = resolve_font_path(patterns[i], buf, sizeof buf);
        /* resolve_font_path echoes the pattern back (":lang=…") when fc-match
         * finds nothing → not an absolute path; skip those. */
        if (!p || p[0] != '/') continue;
        if (primary_path && strcmp(p, primary_path) == 0) continue;
        int dup = 0;
        for (int k = 0; k < n_added; k++)
            if (strcmp(added[k], p) == 0) { dup = 1; break; }
        if (dup) continue;
        if (font_face_add_fallback(font, p) == 0)
            strncpy(added[n_added++], p, sizeof added[0] - 1);
    }
}
