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

/* ── 색상 헬퍼 ──────────────────────────────────────────────────────────── */

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

/* ── 키바인딩 편집 행 ────────────────────────────────────────────────────── */

static void keybind_row(struct nk_context *ctx, const char *label,
                         char *buf, int buflen)
{
    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.45f);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.55f);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buf, buflen,
                                   nk_filter_default);
    nk_layout_row_end(ctx);
}

/* ── 색상 행 (라벨 + 컬러 피커) ──────────────────────────────────────────── */

static void color_row(struct nk_context *ctx, const char *label, uint32_t *color)
{
    struct nk_colorf cf = u32_to_nkcolorf(*color);
    struct nk_color nc = nk_rgb_cf(cf);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.45f);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.55f);
    if (nk_combo_begin_color(ctx, nc, nk_vec2(260, 300))) {
        nk_layout_row_dynamic(ctx, 180, 1);
        cf = nk_color_picker(ctx, cf, NK_RGB);
        *color = nkcolorf_to_u32(cf);
        nk_combo_end(ctx);
    }
    nk_layout_row_end(ctx);
}

/* ── 내부 상태 ──────────────────────────────────────────────────────────── */

static int g_active_tab = 0;  /* 0=Font, 1=Window, 2=Keys, 3=Colors, 4=Export */

static char g_export_path[256] = {0};
static char g_import_path[256] = {0};
static char g_status_msg[512] = {0};

/* ── 탭 버튼 행 ─────────────────────────────────────────────────────────── */

static void draw_tabs(struct nk_context *ctx)
{
    static const char *tab_names[] = { "Font", "Window", "Keys", "Colors", "Export" };
    int tab_count = 5;

    nk_layout_row_dynamic(ctx, 30, tab_count);
    for (int i = 0; i < tab_count; i++) {
        struct nk_style_button style = ctx->style.button;
        if (i == g_active_tab) {
            style.normal = nk_style_item_color(nk_rgb(60, 60, 80));
            style.hover  = style.normal;
            style.text_normal = nk_rgb(255, 255, 255);
        }
        if (nk_button_label_styled(ctx, &style, tab_names[i]))
            g_active_tab = i;
    }
    nk_layout_row_dynamic(ctx, 4, 1);
    nk_spacing(ctx, 1);
}

/* ── 시스템 모노스페이스 폰트 목록 로드 (fc-list) ─────────────────────────── */

#define FONT_LIST_MAX  128
#define FONT_NAME_MAX  128

static char  g_font_list[FONT_LIST_MAX][FONT_NAME_MAX];
static const char *g_font_list_ptrs[FONT_LIST_MAX];
static int   g_font_list_count = 0;
static int   g_font_list_loaded = 0;

static void load_font_list(void)
{
    if (g_font_list_loaded) return;
    g_font_list_loaded = 1;

    /* fc-list :spacing=100 : family (모노스페이스 폰트만) */
    FILE *fp = popen("fc-list :spacing=100 : family 2>/dev/null | sort -u", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof line, fp) && g_font_list_count < FONT_LIST_MAX) {
        /* 첫 번째 쉼표까지만 (여러 이름 중 첫 이름) */
        char *comma = strchr(line, ',');
        if (comma) *comma = '\0';
        /* 개행 제거 */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r' || line[ln-1] == ' '))
            line[--ln] = '\0';
        if (ln == 0) continue;
        strncpy(g_font_list[g_font_list_count], line, FONT_NAME_MAX - 1);
        g_font_list[g_font_list_count][FONT_NAME_MAX - 1] = '\0';
        g_font_list_ptrs[g_font_list_count] = g_font_list[g_font_list_count];
        g_font_list_count++;
    }
    pclose(fp);
}

/* ── 탭별 내용 ──────────────────────────────────────────────────────────── */

static void tab_font(struct nk_context *ctx, termemu_config_t *cfg)
{
    load_font_list();

    /* 현재 폰트의 인덱스 찾기 */
    int selected = -1;
    for (int i = 0; i < g_font_list_count; i++) {
        if (strcmp(g_font_list[i], cfg->font_family) == 0) { selected = i; break; }
    }

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Family:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    if (g_font_list_count > 0) {
        int new_sel = nk_combo(ctx, g_font_list_ptrs, g_font_list_count,
                                selected < 0 ? 0 : selected,
                                25, nk_vec2(320, 400));
        if (new_sel != selected && new_sel >= 0 && new_sel < g_font_list_count) {
            strncpy(cfg->font_family, g_font_list[new_sel], sizeof(cfg->font_family) - 1);
            cfg->font_family[sizeof(cfg->font_family) - 1] = '\0';
        }
    } else {
        /* fc-list 실패 시 텍스트 입력으로 폴백 */
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
            cfg->font_family, sizeof(cfg->font_family), nk_filter_default);
    }
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Size:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    nk_property_float(ctx, "#pt", 6.0f, &cfg->font_size, 48.0f, 0.5f, 0.2f);
    nk_layout_row_end(ctx);
}

static void tab_window(struct nk_context *ctx, termemu_config_t *cfg)
{
    /* Opacity — 바 클릭으로 위치 이동 (커스텀 slider) */
    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Opacity:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    {
        struct nk_rect bar = nk_widget_bounds(ctx);
        struct nk_input *in = &ctx->input;

        /* 바 내부 클릭 (press 또는 hover+down) 시 위치로 opacity 설정 */
        int hovered = nk_input_is_mouse_hovering_rect(in, bar);
        int down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT);
        if (hovered && down) {
            float rel = (in->mouse.pos.x - bar.x) / bar.w;
            if (rel < 0.0f) rel = 0.0f;
            if (rel > 1.0f) rel = 1.0f;
            cfg->opacity = 0.1f + rel * 0.9f; /* [0.1, 1.0] */
        }
        /* 드래그도 지원 */
        nk_slider_float(ctx, 0.1f, &cfg->opacity, 1.0f, 0.01f);
    }
    nk_layout_row_end(ctx);

    /* 값 표시 */
    nk_layout_row_begin(ctx, NK_DYNAMIC, 25, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 0.7f);
    char opstr[16];
    snprintf(opstr, sizeof opstr, "%.0f%%", (double)(cfg->opacity * 100.0f));
    nk_label(ctx, opstr, NK_TEXT_LEFT);
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Padding X:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    nk_property_int(ctx, "#px", 0, &cfg->padding_x, 32, 1, 1);
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Padding Y:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    nk_property_int(ctx, "#py", 0, &cfg->padding_y, 32, 1, 1);
    nk_layout_row_end(ctx);
}

static void tab_keybindings(struct nk_context *ctx, termemu_config_t *cfg)
{
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
}

static void tab_colors(struct nk_context *ctx, termemu_theme_t *theme)
{
    color_row(ctx, "Foreground:", &theme->foreground);
    color_row(ctx, "Background:", &theme->background);

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, "ANSI Palette:", NK_TEXT_LEFT);

    static const char *ansi_names[16] = {
        "Black","Red","Green","Yellow","Blue","Magenta","Cyan","White",
        "Bright Black","Bright Red","Bright Green","Bright Yellow",
        "Bright Blue","Bright Magenta","Bright Cyan","Bright White"
    };
    for (int idx = 0; idx < 16; idx++)
        color_row(ctx, ansi_names[idx], &theme->ansi[idx]);
}

static void tab_export(struct nk_context *ctx,
                        termemu_config_t *cfg, termemu_theme_t *theme,
                        const char *cfg_path, const char *theme_path)
{
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Export config to file:", NK_TEXT_LEFT);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.75f);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        g_export_path, sizeof(g_export_path), nk_filter_default);
    nk_layout_row_push(ctx, 0.25f);
    if (nk_button_label(ctx, "Export")) {
        if (g_export_path[0]) {
            config_save_file(g_export_path, cfg);
            snprintf(g_status_msg, sizeof g_status_msg,
                     "Exported to %s", g_export_path);
        }
    }
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, 15, 1);
    nk_spacing(ctx, 1);

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Import config from file:", NK_TEXT_LEFT);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.75f);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        g_import_path, sizeof(g_import_path), nk_filter_default);
    nk_layout_row_push(ctx, 0.25f);
    if (nk_button_label(ctx, "Import")) {
        if (g_import_path[0]) {
            config_load_file(g_import_path, cfg);
            snprintf(g_status_msg, sizeof g_status_msg,
                     "Imported from %s", g_import_path);
        }
    }
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, 15, 1);
    nk_spacing(ctx, 1);

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Export theme to file:", NK_TEXT_LEFT);

    static char theme_export[256] = {0};
    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.75f);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        theme_export, sizeof(theme_export), nk_filter_default);
    nk_layout_row_push(ctx, 0.25f);
    if (nk_button_label(ctx, "Export")) {
        if (theme_export[0]) {
            theme_save_file(theme_export, theme);
            snprintf(g_status_msg, sizeof g_status_msg,
                     "Theme exported to %s", theme_export);
        }
    }
    nk_layout_row_end(ctx);

    /* 현재 경로 표시 */
    nk_layout_row_dynamic(ctx, 15, 1);
    nk_spacing(ctx, 1);
    if (cfg_path && cfg_path[0]) {
        nk_layout_row_dynamic(ctx, 20, 1);
        char info[512];
        snprintf(info, sizeof info, "Config: %s", cfg_path);
        nk_label(ctx, info, NK_TEXT_LEFT);
    }
    if (theme_path && theme_path[0]) {
        nk_layout_row_dynamic(ctx, 20, 1);
        char info[512];
        snprintf(info, sizeof info, "Theme: %s", theme_path);
        nk_label(ctx, info, NK_TEXT_LEFT);
    }

    /* 상태 메시지 */
    if (g_status_msg[0]) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, g_status_msg, NK_TEXT_LEFT, nk_rgb(100, 220, 100));
    }
}

/* ── 공개 API ───────────────────────────────────────────────────────────── */

int settings_ui_draw(struct nk_context *ctx,
                      termemu_config_t *cfg,
                      termemu_theme_t *theme,
                      const char *cfg_path,
                      const char *theme_path)
{
    int modified = 0;

    if (!nk_begin(ctx, "Settings", nk_rect(50, 50, 480, 520),
                  NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                  NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE))
    {
        nk_end(ctx);
        return -1;
    }

    /* ── 탭 헤더 ── */
    draw_tabs(ctx);

    /* ── 탭 내용 ── */
    switch (g_active_tab) {
    case 0: tab_font(ctx, cfg); break;
    case 1: tab_window(ctx, cfg); break;
    case 2: tab_keybindings(ctx, cfg); break;
    case 3: tab_colors(ctx, theme); break;
    case 4: tab_export(ctx, cfg, theme, cfg_path, theme_path); break;
    }

    /* ── 하단 버튼 ── */
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 35, 3);
    if (nk_button_label(ctx, "Save & Apply")) {
        if (cfg_path && cfg_path[0])
            config_save_file(cfg_path, cfg);
        if (theme_path && theme_path[0])
            theme_save_file(theme_path, theme);
        modified = 1;
        snprintf(g_status_msg, sizeof g_status_msg, "Saved!");
    }
    if (nk_button_label(ctx, "Close")) {
        modified = -1;
    }
    if (nk_button_label(ctx, "Reset Defaults")) {
        config_defaults(cfg);
        theme_defaults(theme);
    }

    nk_end(ctx);
    return modified;
}
