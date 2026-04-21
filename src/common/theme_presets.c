#include "config.h"
#include <string.h>

/* ─── 내장 테마 프리셋 정의 ──────────────────────────────────────────────── */

/*
 * 팔레트 순서 (termemu_theme_t.ansi):
 *   0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white
 *   8 brBlack, 9 brRed, 10 brGreen, 11 brYellow, 12 brBlue, 13 brMagenta,
 *   14 brCyan, 15 brWhite
 */

static const termemu_theme_t PRESETS[] = {
    /* 0: Default (VS Code Dark+) — theme_defaults 와 동일 */
    {
        .name = "Default",
        .background = 0x1E1E1E, .foreground = 0xD4D4D4,
        .cursor_color = 0xD4D4D4, .selection_background = 0x264F78,
        .ansi = {
            0x000000, 0xCD3131, 0x0DBC79, 0xE5E510,
            0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
            0x666666, 0xF14C4C, 0x23D18B, 0xF5F543,
            0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
        },
    },
    /* 1: Solarized Dark (Ethan Schoonover) */
    {
        .name = "Solarized Dark",
        .background = 0x002B36, .foreground = 0x93A1A1,
        .cursor_color = 0x93A1A1, .selection_background = 0x073642,
        .ansi = {
            0x073642, 0xDC322F, 0x859900, 0xB58900,
            0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
            0x002B36, 0xCB4B16, 0x586E75, 0x657B83,
            0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3,
        },
    },
    /* 2: Solarized Light */
    {
        .name = "Solarized Light",
        .background = 0xFDF6E3, .foreground = 0x657B83,
        .cursor_color = 0x586E75, .selection_background = 0xEEE8D5,
        .ansi = {
            0x073642, 0xDC322F, 0x859900, 0xB58900,
            0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
            0xFDF6E3, 0xCB4B16, 0x586E75, 0x657B83,
            0x839496, 0x6C71C4, 0x93A1A1, 0x002B36,
        },
    },
    /* 3: Dracula (dracula.dev) */
    {
        .name = "Dracula",
        .background = 0x282A36, .foreground = 0xF8F8F2,
        .cursor_color = 0xF8F8F2, .selection_background = 0x44475A,
        .ansi = {
            0x000000, 0xFF5555, 0x50FA7B, 0xF1FA8C,
            0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xBBBBBB,
            0x555555, 0xFF5555, 0x50FA7B, 0xF1FA8C,
            0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xFFFFFF,
        },
    },
    /* 4: Nord (nordtheme.com) */
    {
        .name = "Nord",
        .background = 0x2E3440, .foreground = 0xD8DEE9,
        .cursor_color = 0xD8DEE9, .selection_background = 0x434C5E,
        .ansi = {
            0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B,
            0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0,
            0x4C566A, 0xBF616A, 0xA3BE8C, 0xEBCB8B,
            0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4,
        },
    },
    /* 5: Gruvbox Dark (morhetz) */
    {
        .name = "Gruvbox Dark",
        .background = 0x282828, .foreground = 0xEBDBB2,
        .cursor_color = 0xEBDBB2, .selection_background = 0x3C3836,
        .ansi = {
            0x282828, 0xCC241D, 0x98971A, 0xD79921,
            0x458588, 0xB16286, 0x689D6A, 0xA89984,
            0x928374, 0xFB4934, 0xB8BB26, 0xFABD2F,
            0x83A598, 0xD3869B, 0x8EC07C, 0xEBDBB2,
        },
    },
};

#define PRESET_COUNT ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

/* ─── API 구현 ───────────────────────────────────────────────────────────── */

int theme_preset_count(void) { return PRESET_COUNT; }

const char *theme_preset_name(int idx)
{
    if (idx < 0 || idx >= PRESET_COUNT) return "";
    return PRESETS[idx].name;
}

bool theme_preset_get(int idx, termemu_theme_t *out)
{
    if (!out || idx < 0 || idx >= PRESET_COUNT) return false;
    *out = PRESETS[idx];
    return true;
}

/* 테마의 "보이는" 색상 필드만 비교 (name 은 제외).
 * background / foreground / cursor_color / selection_background / ansi[16]. */
static int theme_palette_equal(const termemu_theme_t *a, const termemu_theme_t *b)
{
    if (a->background != b->background) return 0;
    if (a->foreground != b->foreground) return 0;
    if (a->cursor_color != b->cursor_color) return 0;
    if (a->selection_background != b->selection_background) return 0;
    for (int i = 0; i < 16; i++)
        if (a->ansi[i] != b->ansi[i]) return 0;
    return 1;
}

int theme_preset_match(const termemu_theme_t *t)
{
    if (!t) return -1;
    for (int i = 0; i < PRESET_COUNT; i++)
        if (theme_palette_equal(t, &PRESETS[i])) return i;
    return -1;
}
