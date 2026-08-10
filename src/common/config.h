#ifndef TESSERA_CONFIG_H
#define TESSERA_CONFIG_H

/*
 * 설정 파일 파서 (JSON 기반)
 *
 * 파일 구조:
 *   ~/.config/tessera/config.json
 *   ~/.config/tessera/themes/<name>.json
 *
 * 테마 포맷: Windows Terminal schemes 스키마 호환.
 * 색상 값: 0x00RRGGBB
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── 커서 스타일 ───────────────────────────────────────────────────────── */

typedef enum {
    CURSOR_BLOCK        = 1,
    CURSOR_UNDERLINE    = 2,
    CURSOR_BAR          = 3,
    CURSOR_BLOCK_HOLLOW = 4,
} cursor_style_t;

/* ─── 테마 ──────────────────────────────────────────────────────────────── */

/*
 * ansi[0–7]  : black, red, green, yellow, blue, purple, cyan, white
 * ansi[8–15] : 각 bright 버전
 */
typedef struct {
    char     name[256];
    uint32_t background;
    uint32_t foreground;
    uint32_t cursor_color;
    uint32_t selection_background;
    uint32_t ansi[16];
} tessera_theme_t;

/* ─── 단축키 ─────────────────────────────────────────────────────────────── */

/*
 * 각 필드는 "Mod+KeyName" 형태의 문자열.
 * 예: "Alt+minus", "Ctrl+w", "Shift+Page_Up"
 * 빈 문자열이면 해당 단축키 비활성.
 */
typedef struct {
    char split_vertical[64];    /* 기본: Alt+minus   */
    char split_horizontal[64];  /* 기본: Alt+equal   */
    char focus_left[64];        /* 기본: Alt+h       */
    char focus_right[64];       /* 기본: Alt+l       */
    char focus_up[64];          /* 기본: Alt+k       */
    char focus_down[64];        /* 기본: Alt+j       */
    char close_pane[64];        /* 기본: Ctrl+w      */
    char scroll_up[64];         /* 기본: Shift+Prior */
    char scroll_down[64];       /* 기본: Shift+Next  */
    char preferences[64];       /* 기본: Ctrl+comma  */
    char copy[64];              /* 기본: Ctrl+Shift+c (Ctrl+Insert 도 상시 지원) */
    char paste[64];             /* 기본: Ctrl+Shift+v (Shift+Insert 도 상시 지원) */
    char resize_left[64];       /* 기본: Alt+Shift+h */
    char resize_right[64];      /* 기본: Alt+Shift+l */
    char resize_up[64];         /* 기본: Alt+Shift+k */
    char resize_down[64];       /* 기본: Alt+Shift+j */
} keybindings_t;

/* ─── 설정 ──────────────────────────────────────────────────────────────── */

typedef struct {
    char           font_family[256];
    float          font_size;
    bool           font_ligatures;
    float          opacity;           /* 0.0–1.0 */
    int            padding_x;
    int            padding_y;
    int            scrollback_lines;  /* 1–100000 */
    cursor_style_t cursor_style;
    bool           cursor_blink;
    char           theme_name[256];
    bool           bell_visual;
    int            autosave_interval;     /* 세션 자동 저장 주기 (초, 0=비활성) */
    int            session_idle_timeout;  /* 모든 클라이언트 detach 후 세션 유지 시간 (초) */
    bool           confirm_close_pane;    /* Ctrl+W 등 pane 닫기 전 확인 팝업 */
    bool           confirm_close_window;  /* window 닫기 전 확인 팝업 (다중 window 합류 시 사용) */
    bool           confirm_close_session; /* 앱/세션 종료 전 확인 팝업 */
    keybindings_t  keybindings;
} tessera_config_t;

/* ─── 색상 파싱 ─────────────────────────────────────────────────────────── */

/* "#RRGGBB" 또는 "#RGB" → 0x00RRGGBB. 실패 시 fallback 반환. */
uint32_t color_parse_hex(const char *hex, uint32_t fallback);

/* ─── 테마 API ──────────────────────────────────────────────────────────── */

void theme_defaults(tessera_theme_t *t);
bool theme_load_string(const char *json, tessera_theme_t *t);
bool theme_load_file(const char *path, tessera_theme_t *t);
bool theme_save_file(const char *path, const tessera_theme_t *t);

/* ─── 샘플 테마 프리셋 ───────────────────────────────────────────────────── */

/* 내장 테마 개수. idx 는 0..count-1. */
int  theme_preset_count(void);
/* 프리셋 이름 (UI 표시용). NULL 이 아님이 보장됨. */
const char *theme_preset_name(int idx);
/* 프리셋 내용을 out 에 기록. 잘못된 idx 면 false. */
bool theme_preset_get(int idx, tessera_theme_t *out);
/* 주어진 테마가 어떤 프리셋과 정확히 일치하면 그 idx, 아니면 -1 (Custom). */
int  theme_preset_match(const tessera_theme_t *t);

/* ─── 설정 API ──────────────────────────────────────────────────────────── */

void config_defaults(tessera_config_t *cfg);
bool config_load_string(const char *json, tessera_config_t *cfg);
bool config_load_file(const char *path, tessera_config_t *cfg);

/* cfg 내용을 path에 JSON으로 저장한다. 성공 시 true. */
bool config_save_file(const char *path, const tessera_config_t *cfg);

#endif /* TESSERA_CONFIG_H */
