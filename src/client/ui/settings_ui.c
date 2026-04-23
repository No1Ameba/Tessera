#include "settings_ui.h"
#include "file_picker.h"
#include "ui_overlay.h"

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

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    /* ── Cursor style / blink ─────────────────────────────────────────── */
    static const char *cursor_labels[] = {
        "Block", "Hollow Block", "Bar", "Underline"
    };
    /* cfg->cursor_style 값: BLOCK=1, UNDERLINE=2, BAR=3, BLOCK_HOLLOW=4
     * 표시 순서: Block(1)=0, Hollow(4)=1, Bar(3)=2, Underline(2)=3 */
    int cur_idx = 0;
    switch (cfg->cursor_style) {
    case CURSOR_BLOCK:        cur_idx = 0; break;
    case CURSOR_BLOCK_HOLLOW: cur_idx = 1; break;
    case CURSOR_BAR:          cur_idx = 2; break;
    case CURSOR_UNDERLINE:    cur_idx = 3; break;
    }

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Cursor Style:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    int new_idx = nk_combo(ctx, cursor_labels, 4, cur_idx, 25, nk_vec2(200, 160));
    if (new_idx != cur_idx) {
        switch (new_idx) {
        case 0: cfg->cursor_style = CURSOR_BLOCK;        break;
        case 1: cfg->cursor_style = CURSOR_BLOCK_HOLLOW; break;
        case 2: cfg->cursor_style = CURSOR_BAR;          break;
        case 3: cfg->cursor_style = CURSOR_UNDERLINE;    break;
        }
    }
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Cursor Blink:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    int blink = cfg->cursor_blink ? 1 : 0;
    nk_checkbox_label(ctx, blink ? "On" : "Off", &blink);
    cfg->cursor_blink = blink ? true : false;
    nk_layout_row_end(ctx);

    /* ── Confirm on close (파괴적 동작 확인 팝업) ─────────────────────────── */
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, "Confirm before closing:", NK_TEXT_LEFT);

    int c_pane    = cfg->confirm_close_pane    ? 1 : 0;
    int c_window  = cfg->confirm_close_window  ? 1 : 0;
    int c_session = cfg->confirm_close_session ? 1 : 0;
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_checkbox_label(ctx, "Pane",    &c_pane);
    nk_checkbox_label(ctx, "Window",  &c_window);
    nk_checkbox_label(ctx, "Session", &c_session);
    cfg->confirm_close_pane    = c_pane    ? true : false;
    cfg->confirm_close_window  = c_window  ? true : false;
    cfg->confirm_close_session = c_session ? true : false;
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
    keybind_row(ctx, "Copy:",             kb->copy,             sizeof(kb->copy));
    keybind_row(ctx, "Paste:",            kb->paste,            sizeof(kb->paste));
    keybind_row(ctx, "Resize Left:",      kb->resize_left,      sizeof(kb->resize_left));
    keybind_row(ctx, "Resize Right:",     kb->resize_right,     sizeof(kb->resize_right));
    keybind_row(ctx, "Resize Up:",        kb->resize_up,        sizeof(kb->resize_up));
    keybind_row(ctx, "Resize Down:",      kb->resize_down,      sizeof(kb->resize_down));
}

static void tab_colors(struct nk_context *ctx, termemu_theme_t *theme)
{
    /* ── 프리셋 드롭다운 (최상단) ─────────────────────────────────────── */
    int preset_count = theme_preset_count();
    /* 표시 리스트: 프리셋 0..N-1, 마지막에 "Custom" */
    static const char *menu[16];
    int menu_n = 0;
    for (int i = 0; i < preset_count && menu_n < 15; i++)
        menu[menu_n++] = theme_preset_name(i);
    menu[menu_n++] = "Custom";

    int matched = theme_preset_match(theme);
    int current = (matched >= 0) ? matched : (menu_n - 1);  /* Custom 행 */

    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.3f);
    nk_label(ctx, "Theme:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.7f);
    int new_sel = nk_combo(ctx, menu, menu_n, current, 25, nk_vec2(260, 300));
    nk_layout_row_end(ctx);

    if (new_sel != current && new_sel >= 0 && new_sel < preset_count) {
        /* 프리셋 선택 — name 은 유지, 색상만 교체 */
        termemu_theme_t preset;
        if (theme_preset_get(new_sel, &preset)) {
            theme->background            = preset.background;
            theme->foreground            = preset.foreground;
            theme->cursor_color          = preset.cursor_color;
            theme->selection_background  = preset.selection_background;
            for (int i = 0; i < 16; i++) theme->ansi[i] = preset.ansi[i];
            strncpy(theme->name, preset.name, sizeof(theme->name) - 1);
            theme->name[sizeof(theme->name) - 1] = '\0';
        }
    }

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    /* 색상 수정 시 UI 드롭다운은 다음 프레임에 자동으로 "Custom" 전환됨
     * (theme_preset_match 재계산). 수동 표시 없이도 동작. */

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
    /* Config Export — 파일 picker 로 경로 선택 */
    nk_layout_row_dynamic(ctx, 30, 2);
    if (nk_button_label(ctx, "Export Config...")) {
        char path[512];
        int r = file_picker_save(path, sizeof path,
                                   "Export Config", "config.json", "*.json");
        if (r == 1 && path[0]) {
            if (config_save_file(path, cfg))
                snprintf(g_status_msg, sizeof g_status_msg, "Exported to %.480s", path);
            else
                snprintf(g_status_msg, sizeof g_status_msg, "Export failed: %.480s", path);
        } else if (r == -1) {
            snprintf(g_status_msg, sizeof g_status_msg,
                     "File picker unavailable (install zenity or kdialog)");
        }
    }
    if (nk_button_label(ctx, "Import Config...")) {
        char path[512];
        int r = file_picker_open(path, sizeof path, "Import Config", "*.json");
        if (r == 1 && path[0]) {
            if (config_load_file(path, cfg))
                snprintf(g_status_msg, sizeof g_status_msg, "Imported from %.480s", path);
            else
                snprintf(g_status_msg, sizeof g_status_msg, "Import failed: %.480s", path);
        } else if (r == -1) {
            snprintf(g_status_msg, sizeof g_status_msg,
                     "File picker unavailable (install zenity or kdialog)");
        }
    }

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    /* Theme Export / Import */
    nk_layout_row_dynamic(ctx, 30, 2);
    if (nk_button_label(ctx, "Export Theme...")) {
        char path[512];
        int r = file_picker_save(path, sizeof path,
                                   "Export Theme", "theme.json", "*.json");
        if (r == 1 && path[0]) {
            if (theme_save_file(path, theme))
                snprintf(g_status_msg, sizeof g_status_msg, "Theme exported to %.480s", path);
            else
                snprintf(g_status_msg, sizeof g_status_msg, "Export failed: %.480s", path);
        } else if (r == -1) {
            snprintf(g_status_msg, sizeof g_status_msg,
                     "File picker unavailable (install zenity or kdialog)");
        }
    }
    if (nk_button_label(ctx, "Import Theme...")) {
        char path[512];
        int r = file_picker_open(path, sizeof path, "Import Theme", "*.json");
        if (r == 1 && path[0]) {
            if (theme_load_file(path, theme))
                snprintf(g_status_msg, sizeof g_status_msg, "Theme imported from %.480s", path);
            else
                snprintf(g_status_msg, sizeof g_status_msg, "Import failed: %.480s", path);
        } else if (r == -1) {
            snprintf(g_status_msg, sizeof g_status_msg,
                     "File picker unavailable (install zenity or kdialog)");
        }
    }

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
                      const char *theme_path,
                      float win_w, float win_h)
{
    int modified = 0;

    /* 초기 rect 는 화면 비율 기반. 이후는 Nuklear 가 사용자 조정치(MOVABLE/SCALABLE) 유지.
     * min h 는 하단 버튼 행 + tabs + 최소 탭 콘텐츠가 충분히 들어갈 크기로 넉넉히 둔다. */
    float sx, sy, sw, sh;
    ui_overlay_centered_rect(win_w, win_h,
                              0.55f, 0.80f,
                              520.0f, 820.0f,
                              600.0f, 900.0f,
                              &sx, &sy, &sw, &sh);

    /* NO_SCROLLBAR — 외부 창은 스크롤 없이 고정 영역으로 두고, 탭 내용 group 만 스크롤.
     * 두 군데 모두 스크롤이 생기면 이중 스크롤이 되어 조작이 혼란스러움. */
    if (!nk_begin(ctx, "Settings", nk_rect(sx, sy, sw, sh),
                  NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                  NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR))
    {
        nk_end(ctx);
        return -1;
    }

    /* ── 탭 헤더 ── */
    draw_tabs(ctx);

    /* ── 탭 내용을 스크롤 group 으로 감싼다.
     * 창의 content region 에서 탭 헤더/하단 버튼 영역을 제외한 나머지를 group 높이로 잡아
     * 창 크기가 변해도 탭 내용이 리플로우되고, 내용이 넘치면 세로 스크롤된다.
     *
     * confirm_dialog 의 trailing-spacer 패턴과 동일하게 버튼 뒤에 여유 spacer 를 두어
     * 계산 오차(SCALABLE grip, 스타일 padding 추정치) 로 인한 하단 잘림을 흡수한다. */
    const float tabs_row_h     = 30.0f;
    const float tabs_spacer_h  = 4.0f;
    const float footer_spacer  = 8.0f;
    const float footer_btn_h   = 35.0f;
    const float trailing_spacer = 18.0f;  /* SCALABLE grip + 하단 border 여유 */

    struct nk_rect content = nk_window_get_content_region(ctx);
    float sp = ctx->style.window.spacing.y;

    /* row 수 총합: tabs(2) + group(1) + footer(2) + trailing(1) = 6 행, 행 사이 spacing = 5 × sp */
    float fixed_h = tabs_row_h + tabs_spacer_h
                  + footer_spacer + footer_btn_h + trailing_spacer
                  + 5.0f * sp
                  + 20.0f;  /* 스타일 추정치 오차 흡수용 safety margin */

    float group_h = content.h - fixed_h;
    if (group_h < 60.0f) group_h = 60.0f;

    nk_layout_row_dynamic(ctx, group_h, 1);
    if (nk_group_begin(ctx, "settings_tab_body", NK_WINDOW_BORDER)) {
        switch (g_active_tab) {
        case 0: tab_font(ctx, cfg); break;
        case 1: tab_window(ctx, cfg); break;
        case 2: tab_keybindings(ctx, cfg); break;
        case 3: tab_colors(ctx, theme); break;
        case 4: tab_export(ctx, cfg, theme, cfg_path, theme_path); break;
        }
        nk_group_end(ctx);
    }

    /* ── 하단 버튼 ── */
    nk_layout_row_dynamic(ctx, footer_spacer, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, footer_btn_h, 3);
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

    /* trailing spacer — 계산 오차로 하단이 잘려야 할 경우 버튼 대신 이 spacer 가 먼저 잘리도록. */
    nk_layout_row_dynamic(ctx, trailing_spacer, 1);
    nk_spacing(ctx, 1);

    nk_end(ctx);
    return modified;
}
