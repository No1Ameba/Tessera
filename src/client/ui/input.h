#ifndef TESSERA_INPUT_H
#define TESSERA_INPUT_H

/*
 * Input translation — GLFW key/modifier → VT/ANSI escape sequences.
 *
 * Key sequences follow xterm's behaviour (most widely supported):
 *   - Arrow / cursor keys : ESC [ A/B/C/D
 *   - Function keys       : ESC O P/Q/R/S  (F1–F4),  ESC [ N ~  (F5–F12)
 *   - Editing keys        : ESC [ N ~  (Insert=2, Delete=3, PgUp=5, PgDn=6,
 *                           Home=1 / H, End=4 / F)
 *   - Ctrl+letter         : 0x01–0x1A
 *   - Alt+key             : ESC + normal bytes
 *
 * Mouse sequences use xterm SGR format:
 *   press:   ESC [ < button ; col ; row M
 *   release: ESC [ < button ; col ; row m
 */

#include <stdint.h>
#include <stddef.h>

/* Modifier flags (platform-agnostic). */
#define INPUT_MOD_SHIFT   (1 << 0)
#define INPUT_MOD_CTRL    (1 << 1)
#define INPUT_MOD_ALT     (1 << 2)  /* Mod1 / Meta */
#define INPUT_MOD_SUPER   (1 << 3)  /* Mod4 / Logo */

/*
 * Translate a GLFW key code + modifier flags to VT bytes.
 * glfw_key: GLFW_KEY_* constant
 * modifiers: INPUT_MOD_* flags
 * out_buf: caller buffer
 * buf_size: capacity of out_buf (recommend at least 16)
 * @return number of bytes written (0 if unhandled / printable text via CharCallback).
 */
int input_key_to_bytes(int glfw_key, unsigned int modifiers,
                       uint8_t *out_buf, int buf_size);

/*
 * Translate GLFW modifier bitmask to INPUT_MOD_* flags.
 * glfw_mods: GLFW modifier bits from key/mouse callbacks
 */
unsigned int input_glfw_mods(int glfw_mods);

/*
 * Translate a config key name string to GLFW_KEY_* value.
 * e.g. "minus" → GLFW_KEY_MINUS, "Prior" → GLFW_KEY_PAGE_UP
 * @return GLFW key code, or -1 if unknown.
 */
int input_glfw_key_from_name(const char *name);

/*
 * "Mod+Key" 바인딩 문자열(예: "Ctrl+Shift+c", "Alt+minus")이 주어진
 * modifier 플래그 + GLFW 키와 정확히 일치하는지 검사. 일치 시 1.
 */
int keybind_matches(const char *binding, unsigned int mods, int glfw_key);

/*
 * (키, 모디파이어) → 바인딩 문자열. keybind_matches 가 파싱하는 형식 그대로
 * 만든다("Alt+equal", "Ctrl+Shift+w" 등). 설정 UI 에서 키를 직접 눌러 바인딩을
 * 지정할 때 쓴다.
 *
 * 모디파이어 단독 키(Shift/Ctrl/Alt/Super)는 바인딩이 될 수 없으므로 거절한다.
 *
 * @return 0 성공, -1 표현할 수 없는 키이거나 버퍼 부족.
 */
int input_keybind_format(int glfw_key, unsigned int mods,
                          char *out, size_t out_size);

/*
 * Translate a mouse event to SGR escape sequence bytes.
 * col, row: 1-based terminal cell coordinates
 * button: 0=left, 1=middle, 2=right, 3=release (when press=0)
 * press: 1=button press, 0=button release
 * @return number of bytes written, 0 if nothing to send.
 */
int input_mouse_to_bytes(int col, int row, int button, int press,
                          unsigned int modifiers,
                          uint8_t *out_buf, int buf_size);

#endif /* TESSERA_INPUT_H */
