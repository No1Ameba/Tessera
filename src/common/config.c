#include "config.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── 색상 파싱 ─────────────────────────────────────────────────────────── */

uint32_t color_parse_hex(const char *hex, uint32_t fallback) {
    if (!hex || hex[0] != '#') return fallback;

    const char *s = hex + 1;
    size_t len = strlen(s);

    unsigned r, g, b;
    if (len == 6) {
        if (sscanf(s, "%02x%02x%02x", &r, &g, &b) != 3) return fallback;
    } else if (len == 3) {
        if (sscanf(s, "%1x%1x%1x", &r, &g, &b) != 3) return fallback;
        r = r * 17; g = g * 17; b = b * 17;  /* #RGB → #RRGGBB */
    } else {
        return fallback;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ─── 테마 기본값 ────────────────────────────────────────────────────────── */

/*
 * 기본 테마: VS Code Dark+ 계열 색상.
 * 별도 테마 파일이 없어도 동작 가능하도록.
 */
void theme_defaults(tessera_theme_t *t) {
    memset(t, 0, sizeof(*t));
    strncpy(t->name, "default", sizeof(t->name) - 1);

    t->background          = 0x1E1E1E;
    t->foreground          = 0xD4D4D4;
    t->cursor_color        = 0xD4D4D4;
    t->selection_background= 0x264F78;

    t->statusbar_bg        = 0x2D2D30;
    t->statusbar_fg        = 0x9DA5B4;
    t->statusbar_active_bg = 0x2472C8;
    t->statusbar_active_fg = 0xFFFFFF;

    /* Normal: black red green yellow blue purple cyan white */
    t->ansi[0]  = 0x000000; t->ansi[1]  = 0xCD3131;
    t->ansi[2]  = 0x0DBC79; t->ansi[3]  = 0xE5E510;
    t->ansi[4]  = 0x2472C8; t->ansi[5]  = 0xBC3FBC;
    t->ansi[6]  = 0x11A8CD; t->ansi[7]  = 0xE5E5E5;
    /* Bright */
    t->ansi[8]  = 0x666666; t->ansi[9]  = 0xF14C4C;
    t->ansi[10] = 0x23D18B; t->ansi[11] = 0xF5F543;
    t->ansi[12] = 0x3B8EEA; t->ansi[13] = 0xD670D6;
    t->ansi[14] = 0x29B8DB; t->ansi[15] = 0xFFFFFF;
}

/* ─── 테마 파싱 ─────────────────────────────────────────────────────────── */

/* Windows Terminal 스키마의 color key 순서 */
static const char *ANSI_KEYS[16] = {
    "black",  "red",   "green",  "yellow",
    "blue",   "purple","cyan",   "white",
    "brightBlack",  "brightRed",   "brightGreen",  "brightYellow",
    "brightBlue",   "brightPurple","brightCyan",   "brightWhite",
};

bool theme_load_string(const char *json, tessera_theme_t *t) {
    if (!json || !t) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    theme_defaults(t);  /* 누락 필드는 기본값 유지 */

    cJSON *item;

#define PARSE_COLOR(key, field) \
    item = cJSON_GetObjectItemCaseSensitive(root, key); \
    if (cJSON_IsString(item)) \
        t->field = color_parse_hex(item->valuestring, t->field);

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item))
        strncpy(t->name, item->valuestring, sizeof(t->name) - 1);

    PARSE_COLOR("background",          background)
    PARSE_COLOR("foreground",          foreground)
    PARSE_COLOR("cursorColor",         cursor_color)
    PARSE_COLOR("selectionBackground", selection_background)
    PARSE_COLOR("statusbarBackground",       statusbar_bg)
    PARSE_COLOR("statusbarForeground",       statusbar_fg)
    PARSE_COLOR("statusbarActiveBackground", statusbar_active_bg)
    PARSE_COLOR("statusbarActiveForeground", statusbar_active_fg)

    for (int i = 0; i < 16; i++) {
        item = cJSON_GetObjectItemCaseSensitive(root, ANSI_KEYS[i]);
        if (cJSON_IsString(item))
            t->ansi[i] = color_parse_hex(item->valuestring, t->ansi[i]);
    }

#undef PARSE_COLOR

    cJSON_Delete(root);
    return true;
}

bool theme_save_file(const char *path, const tessera_theme_t *t) {
    if (!path || !t) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    char hex[8];
#define ADD_COLOR(key, val) \
    snprintf(hex, sizeof(hex), "#%06X", (val)); \
    cJSON_AddStringToObject(root, (key), hex);

    cJSON_AddStringToObject(root, "name", t->name);
    ADD_COLOR("background",          t->background)
    ADD_COLOR("foreground",          t->foreground)
    ADD_COLOR("cursorColor",         t->cursor_color)
    ADD_COLOR("selectionBackground", t->selection_background)
    ADD_COLOR("statusbarBackground",       t->statusbar_bg)
    ADD_COLOR("statusbarForeground",       t->statusbar_fg)
    ADD_COLOR("statusbarActiveBackground", t->statusbar_active_bg)
    ADD_COLOR("statusbarActiveForeground", t->statusbar_active_fg)
    for (int i = 0; i < 16; i++) {
        ADD_COLOR(ANSI_KEYS[i], t->ansi[i])
    }
#undef ADD_COLOR

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;

    FILE *f = fopen(path, "w");
    if (!f) { free(text); return false; }
    fputs(text, f);
    fclose(f);
    free(text);
    return true;
}

bool theme_load_file(const char *path, tessera_theme_t *t) {
    if (!path || !t) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size <= 0 || size > 64 * 1024) { fclose(f); return false; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }

    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { free(buf); return false; }
    buf[rd] = '\0';

    bool ok = theme_load_string(buf, t);
    free(buf);
    return ok;
}

/* ─── 설정 기본값 ────────────────────────────────────────────────────────── */

void config_defaults(tessera_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->font_family, "monospace", sizeof(cfg->font_family) - 1);
    cfg->font_size        = 14.0f;
    cfg->font_ligatures   = true;
    cfg->opacity          = 1.0f;
    cfg->padding_x        = 4;
    cfg->padding_y        = 4;
    cfg->scrollback_lines = 10000;
    cfg->cursor_style     = CURSOR_BLOCK;
    cfg->cursor_blink     = true;
    strncpy(cfg->theme_name, "default", sizeof(cfg->theme_name) - 1);
    cfg->bell_visual      = false;
    cfg->autosave_interval    = 300;  /* 기본 5분 */
    cfg->session_idle_timeout = 300;  /* 기본 5분: detach 후 세션 유지 기간 */
    cfg->confirm_close_pane    = true;
    cfg->confirm_close_window  = true;
    cfg->confirm_close_session = true;
    cfg->statusbar_show        = true;

    keybindings_t *kb = &cfg->keybindings;
    strncpy(kb->split_vertical,   "Alt+minus",   sizeof(kb->split_vertical) - 1);
    strncpy(kb->split_horizontal, "Alt+equal",   sizeof(kb->split_horizontal) - 1);
    strncpy(kb->focus_left,       "Alt+h",       sizeof(kb->focus_left) - 1);
    strncpy(kb->focus_right,      "Alt+l",       sizeof(kb->focus_right) - 1);
    strncpy(kb->focus_up,         "Alt+k",       sizeof(kb->focus_up) - 1);
    strncpy(kb->focus_down,       "Alt+j",       sizeof(kb->focus_down) - 1);
    strncpy(kb->close_pane,       "Ctrl+w",      sizeof(kb->close_pane) - 1);
    strncpy(kb->scroll_up,        "Shift+Prior", sizeof(kb->scroll_up) - 1);
    strncpy(kb->scroll_down,      "Shift+Next",  sizeof(kb->scroll_down) - 1);
    strncpy(kb->preferences,      "Ctrl+comma",  sizeof(kb->preferences) - 1);
    strncpy(kb->copy,             "Ctrl+Shift+c", sizeof(kb->copy) - 1);
    strncpy(kb->paste,            "Ctrl+Shift+v", sizeof(kb->paste) - 1);
    strncpy(kb->resize_left,      "Alt+Shift+h",  sizeof(kb->resize_left) - 1);
    strncpy(kb->resize_right,     "Alt+Shift+l",  sizeof(kb->resize_right) - 1);
    strncpy(kb->resize_up,        "Alt+Shift+k",  sizeof(kb->resize_up) - 1);
    strncpy(kb->resize_down,      "Alt+Shift+j",  sizeof(kb->resize_down) - 1);
    strncpy(kb->window_next,      "Ctrl+Alt+l",   sizeof(kb->window_next) - 1);
    strncpy(kb->window_prev,      "Ctrl+Alt+h",   sizeof(kb->window_prev) - 1);
    strncpy(kb->window_new,       "Ctrl+Alt+n",   sizeof(kb->window_new) - 1);
    strncpy(kb->window_close,     "Ctrl+Alt+w",   sizeof(kb->window_close) - 1);
}

/* ─── 설정 파싱 ─────────────────────────────────────────────────────────── */

static cursor_style_t parse_cursor_style(const char *s) {
    if (!s)                             return CURSOR_BLOCK;
    if (strcmp(s, "block")        == 0) return CURSOR_BLOCK;
    if (strcmp(s, "underline")    == 0) return CURSOR_UNDERLINE;
    if (strcmp(s, "bar")          == 0) return CURSOR_BAR;
    if (strcmp(s, "block_hollow") == 0) return CURSOR_BLOCK_HOLLOW;
    return CURSOR_BLOCK;
}

bool config_load_string(const char *json, tessera_config_t *cfg) {
    if (!json || !cfg) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    config_defaults(cfg);

    cJSON *item, *sub;

    /* font */
    sub = cJSON_GetObjectItemCaseSensitive(root, "font");
    if (cJSON_IsObject(sub)) {
        item = cJSON_GetObjectItemCaseSensitive(sub, "family");
        if (cJSON_IsString(item))
            strncpy(cfg->font_family, item->valuestring, sizeof(cfg->font_family) - 1);

        item = cJSON_GetObjectItemCaseSensitive(sub, "size");
        if (cJSON_IsNumber(item))
            cfg->font_size = (float)item->valuedouble;

        item = cJSON_GetObjectItemCaseSensitive(sub, "ligatures");
        if (cJSON_IsBool(item))
            cfg->font_ligatures = cJSON_IsTrue(item);
    }

    /* window */
    sub = cJSON_GetObjectItemCaseSensitive(root, "window");
    if (cJSON_IsObject(sub)) {
        item = cJSON_GetObjectItemCaseSensitive(sub, "opacity");
        if (cJSON_IsNumber(item)) {
            float v = (float)item->valuedouble;
            cfg->opacity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        cJSON *pad = cJSON_GetObjectItemCaseSensitive(sub, "padding");
        if (cJSON_IsObject(pad)) {
            item = cJSON_GetObjectItemCaseSensitive(pad, "x");
            if (cJSON_IsNumber(item)) cfg->padding_x = item->valueint;
            item = cJSON_GetObjectItemCaseSensitive(pad, "y");
            if (cJSON_IsNumber(item)) cfg->padding_y = item->valueint;
        }
    }

    /* scrollback_lines */
    item = cJSON_GetObjectItemCaseSensitive(root, "scrollback_lines");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < 1)       v = 1;
        if (v > 100000)  v = 100000;
        cfg->scrollback_lines = v;
    }

    /* cursor */
    sub = cJSON_GetObjectItemCaseSensitive(root, "cursor");
    if (cJSON_IsObject(sub)) {
        item = cJSON_GetObjectItemCaseSensitive(sub, "style");
        if (cJSON_IsString(item))
            cfg->cursor_style = parse_cursor_style(item->valuestring);

        item = cJSON_GetObjectItemCaseSensitive(sub, "blink");
        if (cJSON_IsBool(item))
            cfg->cursor_blink = cJSON_IsTrue(item);
    }

    /* theme */
    item = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (cJSON_IsString(item))
        strncpy(cfg->theme_name, item->valuestring, sizeof(cfg->theme_name) - 1);

    /* bell */
    sub = cJSON_GetObjectItemCaseSensitive(root, "bell");
    if (cJSON_IsObject(sub)) {
        item = cJSON_GetObjectItemCaseSensitive(sub, "visual");
        if (cJSON_IsBool(item))
            cfg->bell_visual = cJSON_IsTrue(item);
    }

    /* daemon (세션 수명 관리) */
    sub = cJSON_GetObjectItemCaseSensitive(root, "daemon");
    if (cJSON_IsObject(sub)) {
        item = cJSON_GetObjectItemCaseSensitive(sub, "autosave_interval");
        if (cJSON_IsNumber(item)) {
            int v = item->valueint;
            if (v < 0) v = 0;
            cfg->autosave_interval = v;
        }
        item = cJSON_GetObjectItemCaseSensitive(sub, "session_idle_timeout");
        if (cJSON_IsNumber(item)) {
            int v = item->valueint;
            if (v < 0) v = 0;
            cfg->session_idle_timeout = v;
        }
    }

    /* confirm (파괴적 동작 확인 팝업) */
    sub = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (cJSON_IsObject(sub)) {
        item = cJSON_GetObjectItemCaseSensitive(sub, "pane");
        if (cJSON_IsBool(item)) cfg->confirm_close_pane = cJSON_IsTrue(item);
        item = cJSON_GetObjectItemCaseSensitive(sub, "window");
        if (cJSON_IsBool(item)) cfg->confirm_close_window = cJSON_IsTrue(item);
        item = cJSON_GetObjectItemCaseSensitive(sub, "session");
        if (cJSON_IsBool(item)) cfg->confirm_close_session = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "statusbar_show");
    if (cJSON_IsBool(item)) cfg->statusbar_show = cJSON_IsTrue(item);

    /* keybindings */
    sub = cJSON_GetObjectItemCaseSensitive(root, "keybindings");
    if (cJSON_IsObject(sub)) {
        keybindings_t *kb = &cfg->keybindings;
#define PARSE_KB(key, field) \
        item = cJSON_GetObjectItemCaseSensitive(sub, key); \
        if (cJSON_IsString(item)) \
            strncpy(kb->field, item->valuestring, sizeof(kb->field) - 1);

        PARSE_KB("split_vertical",   split_vertical)
        PARSE_KB("split_horizontal", split_horizontal)
        PARSE_KB("focus_left",       focus_left)
        PARSE_KB("focus_right",      focus_right)
        PARSE_KB("focus_up",         focus_up)
        PARSE_KB("focus_down",       focus_down)
        PARSE_KB("close_pane",       close_pane)
        PARSE_KB("scroll_up",        scroll_up)
        PARSE_KB("scroll_down",      scroll_down)
        PARSE_KB("preferences",      preferences)
        PARSE_KB("copy",             copy)
        PARSE_KB("paste",            paste)
        PARSE_KB("resize_left",      resize_left)
        PARSE_KB("resize_right",     resize_right)
        PARSE_KB("resize_up",        resize_up)
        PARSE_KB("resize_down",      resize_down)
        PARSE_KB("window_next",      window_next)
        PARSE_KB("window_prev",      window_prev)
        PARSE_KB("window_new",       window_new)
        PARSE_KB("window_close",     window_close)
#undef PARSE_KB
    }

    cJSON_Delete(root);
    return true;
}

/* ─── 설정 저장 ─────────────────────────────────────────────────────────── */

bool config_save_file(const char *path, const tessera_config_t *cfg) {
    if (!path || !cfg) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    /* font */
    cJSON *font = cJSON_CreateObject();
    cJSON_AddStringToObject(font, "family", cfg->font_family);
    cJSON_AddNumberToObject(font, "size", (double)cfg->font_size);
    cJSON_AddBoolToObject(font, "ligatures", cfg->font_ligatures);
    cJSON_AddItemToObject(root, "font", font);

    /* window */
    cJSON *window = cJSON_CreateObject();
    cJSON_AddNumberToObject(window, "opacity", (double)cfg->opacity);
    cJSON *padding = cJSON_CreateObject();
    cJSON_AddNumberToObject(padding, "x", cfg->padding_x);
    cJSON_AddNumberToObject(padding, "y", cfg->padding_y);
    cJSON_AddItemToObject(window, "padding", padding);
    cJSON_AddItemToObject(root, "window", window);

    cJSON_AddNumberToObject(root, "scrollback_lines", cfg->scrollback_lines);

    /* cursor */
    cJSON *cursor = cJSON_CreateObject();
    const char *style_str = "block";
    if      (cfg->cursor_style == CURSOR_UNDERLINE)    style_str = "underline";
    else if (cfg->cursor_style == CURSOR_BAR)          style_str = "bar";
    else if (cfg->cursor_style == CURSOR_BLOCK_HOLLOW) style_str = "block_hollow";
    cJSON_AddStringToObject(cursor, "style", style_str);
    cJSON_AddBoolToObject(cursor, "blink", cfg->cursor_blink);
    cJSON_AddItemToObject(root, "cursor", cursor);

    cJSON_AddStringToObject(root, "theme", cfg->theme_name);

    /* bell */
    cJSON *bell = cJSON_CreateObject();
    cJSON_AddBoolToObject(bell, "visual", cfg->bell_visual);
    cJSON_AddItemToObject(root, "bell", bell);

    /* daemon */
    cJSON *daemon_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(daemon_obj, "autosave_interval",    cfg->autosave_interval);
    cJSON_AddNumberToObject(daemon_obj, "session_idle_timeout", cfg->session_idle_timeout);
    cJSON_AddItemToObject(root, "daemon", daemon_obj);

    /* confirm */
    cJSON *confirm = cJSON_CreateObject();
    cJSON_AddBoolToObject(confirm, "pane",    cfg->confirm_close_pane);
    cJSON_AddBoolToObject(confirm, "window",  cfg->confirm_close_window);
    cJSON_AddBoolToObject(confirm, "session", cfg->confirm_close_session);
    cJSON_AddItemToObject(root, "confirm", confirm);

    cJSON_AddBoolToObject(root, "statusbar_show", cfg->statusbar_show);

    /* keybindings */
    const keybindings_t *kb = &cfg->keybindings;
    cJSON *keybindings = cJSON_CreateObject();
    cJSON_AddStringToObject(keybindings, "split_vertical",   kb->split_vertical);
    cJSON_AddStringToObject(keybindings, "split_horizontal", kb->split_horizontal);
    cJSON_AddStringToObject(keybindings, "focus_left",       kb->focus_left);
    cJSON_AddStringToObject(keybindings, "focus_right",      kb->focus_right);
    cJSON_AddStringToObject(keybindings, "focus_up",         kb->focus_up);
    cJSON_AddStringToObject(keybindings, "focus_down",       kb->focus_down);
    cJSON_AddStringToObject(keybindings, "close_pane",       kb->close_pane);
    cJSON_AddStringToObject(keybindings, "scroll_up",        kb->scroll_up);
    cJSON_AddStringToObject(keybindings, "scroll_down",      kb->scroll_down);
    cJSON_AddStringToObject(keybindings, "preferences",      kb->preferences);
    cJSON_AddStringToObject(keybindings, "copy",             kb->copy);
    cJSON_AddStringToObject(keybindings, "paste",            kb->paste);
    cJSON_AddStringToObject(keybindings, "resize_left",      kb->resize_left);
    cJSON_AddStringToObject(keybindings, "resize_right",     kb->resize_right);
    cJSON_AddStringToObject(keybindings, "resize_up",        kb->resize_up);
    cJSON_AddStringToObject(keybindings, "resize_down",      kb->resize_down);
    cJSON_AddStringToObject(keybindings, "window_next",      kb->window_next);
    cJSON_AddStringToObject(keybindings, "window_prev",      kb->window_prev);
    cJSON_AddStringToObject(keybindings, "window_new",       kb->window_new);
    cJSON_AddStringToObject(keybindings, "window_close",     kb->window_close);
    cJSON_AddItemToObject(root, "keybindings", keybindings);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;

    FILE *f = fopen(path, "w");
    if (!f) { free(text); return false; }
    fputs(text, f);
    fclose(f);
    free(text);
    return true;
}

bool config_load_file(const char *path, tessera_config_t *cfg) {
    if (!path || !cfg) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size <= 0 || size > 256 * 1024) { fclose(f); return false; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }

    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { free(buf); return false; }
    buf[rd] = '\0';

    bool ok = config_load_string(buf, cfg);
    free(buf);
    return ok;
}
