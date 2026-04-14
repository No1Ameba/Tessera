#include "settings_ui.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear.h"

#include <stdio.h>
#include <string.h>

/* ── color helpers ──────────────────────────────────────────────────────── */

static struct nk_colorf u32_to_nkcolorf(uint32_t c)
{
    struct nk_colorf cf;
    cf.r = ((c >> 16) & 0xFF) / 255.0f;
    cf.g = ((c >>  8) & 0xFF) / 255.0f;
    cf.b = ( c        & 0xFF) / 255.0f;
    cf.a = 1.0f;
    return cf;
}

static uint32_t nkcolorf_to_u32(struct nk_colorf cf)
{
    uint8_t r = (uint8_t)(cf.r * 255.0f);
    uint8_t g = (uint8_t)(cf.g * 255.0f);
    uint8_t b = (uint8_t)(cf.b * 255.0f);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 키바인딩 편집 행 헬퍼 */
static void keybind_row(struct nk_context *ctx, const char *label,
                         char *buf, int buflen)
{
    nk_layout_row_dynamic(ctx, 25, 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buf, buflen,
                                   nk_filter_default);
}

/* 팝업 색상 피커를 가진 색상 버튼 */
static int color_button_picker(struct nk_context *ctx, const char *id,
                                uint32_t *color)
{
    struct nk_colorf cf = u32_to_nkcolorf(*color);
    struct nk_color nc = nk_rgb_cf(cf);
    int changed = 0;

    /* 화살표 없는 순수 색상 사각형 버튼 */
    if (nk_button_color(ctx, nc)) {
        /* 토글 팝업 */
    }

    /* 색상 편집은 combo 팝업으로 */
    if (nk_combo_begin_color(ctx, nc, nk_vec2(250, 350))) {
        nk_layout_row_dynamic(ctx, 180, 1);
        cf = nk_color_picker(ctx, cf, NK_RGB);
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, id, NK_TEXT_CENTERED);
        *color = nkcolorf_to_u32(cf);
        changed = 1;
        nk_combo_end(ctx);
    }
    return changed;
}

/* ── 설정 패널 ──────────────────────────────────────────────────────────── */

int settings_ui_draw(struct nk_context *ctx,
                      termemu_config_t *cfg,
                      termemu_theme_t *theme,
                      const char *cfg_path,
                      const char *theme_path)
{
    int modified = 0;

    if (!nk_begin(ctx, "Settings", nk_rect(50, 50, 420, 550),
                  NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                  NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE))
    {
        /* 닫기 버튼(X) 클릭 → nk_begin이 0 반환 */
        nk_end(ctx);
        return -1;
    }
    {
        /* ── 폰트 ──────────────────────────────────────────────────────── */
        if (nk_tree_push(ctx, NK_TREE_TAB, "Font", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 25, 2);
            nk_label(ctx, "Family:", NK_TEXT_LEFT);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
                cfg->font_family, sizeof(cfg->font_family), nk_filter_default);

            nk_layout_row_dynamic(ctx, 25, 2);
            nk_label(ctx, "Size:", NK_TEXT_LEFT);
            nk_property_float(ctx, "#pt", 6.0f, &cfg->font_size, 48.0f, 0.5f, 0.2f);

            nk_tree_pop(ctx);
        }

        /* ── 창 ────────────────────────────────────────────────────────── */
        if (nk_tree_push(ctx, NK_TREE_TAB, "Window", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 25, 2);
            nk_label(ctx, "Opacity:", NK_TEXT_LEFT);
            nk_slider_float(ctx, 0.1f, &cfg->opacity, 1.0f, 0.01f);

            nk_layout_row_dynamic(ctx, 25, 2);
            nk_label(ctx, "Padding X:", NK_TEXT_LEFT);
            nk_property_int(ctx, "#px", 0, &cfg->padding_x, 32, 1, 1);
            nk_label(ctx, "Padding Y:", NK_TEXT_LEFT);
            nk_property_int(ctx, "#py", 0, &cfg->padding_y, 32, 1, 1);

            nk_tree_pop(ctx);
        }

        /* ── 키바인딩 ──────────────────────────────────────────────────── */
        if (nk_tree_push(ctx, NK_TREE_TAB, "Keybindings", NK_MINIMIZED)) {
            keybindings_t *kb = &cfg->keybindings;
            keybind_row(ctx, "Split Vertical:",   kb->split_vertical,   sizeof(kb->split_vertical));
            keybind_row(ctx, "Split Horizontal:", kb->split_horizontal, sizeof(kb->split_horizontal));
            keybind_row(ctx, "Focus Left:",       kb->focus_left,       sizeof(kb->focus_left));
            keybind_row(ctx, "Focus Right:",      kb->focus_right,      sizeof(kb->focus_right));
            keybind_row(ctx, "Focus Up:",         kb->focus_up,         sizeof(kb->focus_up));
            keybind_row(ctx, "Focus Down:",       kb->focus_down,       sizeof(kb->focus_down));
            keybind_row(ctx, "Close Pane:",       kb->close_pane,       sizeof(kb->close_pane));
            keybind_row(ctx, "Scroll Up:",        kb->scroll_up,        sizeof(kb->scroll_up));
            keybind_row(ctx, "Scroll Down:",      kb->scroll_down,      sizeof(kb->scroll_down));
            keybind_row(ctx, "Preferences:",      kb->preferences,      sizeof(kb->preferences));

            nk_tree_pop(ctx);
        }

        /* ── 테마 색상 ─────────────────────────────────────────────────── */
        if (nk_tree_push(ctx, NK_TREE_TAB, "Colors", NK_MINIMIZED)) {
            /* Foreground / Background */
            nk_layout_row_dynamic(ctx, 30, 2);
            nk_label(ctx, "Foreground:", NK_TEXT_LEFT);
            color_button_picker(ctx, "FG", &theme->foreground);

            nk_layout_row_dynamic(ctx, 30, 2);
            nk_label(ctx, "Background:", NK_TEXT_LEFT);
            color_button_picker(ctx, "BG", &theme->background);

            /* ANSI 16색 팔레트 — 색상 사각형만 표시 (화살표 없음) */
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "ANSI Palette (click to edit):", NK_TEXT_LEFT);

            static const char *ansi_names[16] = {
                "Black","Red","Green","Yellow","Blue","Magenta","Cyan","White",
                "Bright Black","Bright Red","Bright Green","Bright Yellow",
                "Bright Blue","Bright Magenta","Bright Cyan","Bright White"
            };
            /* 각 색상을 라벨+버튼 행으로 표시 (화살표 없는 순수 색상 버튼) */
            for (int idx = 0; idx < 16; idx++) {
                struct nk_colorf cf = u32_to_nkcolorf(theme->ansi[idx]);
                struct nk_color nc = nk_rgb_cf(cf);
                char popup_id[32];
                snprintf(popup_id, sizeof(popup_id), "ansi_%d", idx);

                nk_layout_row_begin(ctx, NK_STATIC, 25, 2);
                nk_layout_row_push(ctx, 130);
                nk_label(ctx, ansi_names[idx], NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 60);
                if (nk_combo_begin_color(ctx, nc, nk_vec2(250, 280))) {
                    nk_layout_row_dynamic(ctx, 180, 1);
                    cf = nk_color_picker(ctx, cf, NK_RGB);
                    theme->ansi[idx] = nkcolorf_to_u32(cf);
                    nk_combo_end(ctx);
                }
                nk_layout_row_end(ctx);
            }

            nk_tree_pop(ctx);
        }

        /* ── 저장 버튼 ─────────────────────────────────────────────────── */
        nk_layout_row_dynamic(ctx, 35, 2);
        if (nk_button_label(ctx, "Save & Apply")) {
            if (cfg_path && cfg_path[0])
                config_save_file(cfg_path, cfg);
            if (theme_path && theme_path[0])
                theme_save_file(theme_path, theme);
            modified = 1;
        }
        if (nk_button_label(ctx, "Close")) {
            modified = -1;
        }
    }
    nk_end(ctx);

    return modified;
}
