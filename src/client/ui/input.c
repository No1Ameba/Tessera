#include "input.h"
#include <GLFW/glfw3.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── GLFW modifier translation ──────────────────────────────────────────── */

unsigned int input_glfw_mods(int glfw_mods)
{
    unsigned int m = 0;
    if (glfw_mods & GLFW_MOD_SHIFT)   m |= INPUT_MOD_SHIFT;
    if (glfw_mods & GLFW_MOD_CONTROL) m |= INPUT_MOD_CTRL;
    if (glfw_mods & GLFW_MOD_ALT)     m |= INPUT_MOD_ALT;
    if (glfw_mods & GLFW_MOD_SUPER)   m |= INPUT_MOD_SUPER;
    return m;
}

/* ── Key name → GLFW_KEY lookup ─────────────────────────────────────────── */

int input_glfw_key_from_name(const char *name)
{
    if (!name || !name[0]) return -1;

    /* Single character: letter or digit */
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return GLFW_KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return GLFW_KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return GLFW_KEY_0 + (c - '0');
    }

    /* Named keys (case-insensitive comparison) */
    static const struct { const char *name; int key; } table[] = {
        {"minus",       GLFW_KEY_MINUS},
        {"equal",       GLFW_KEY_EQUAL},
        {"comma",       GLFW_KEY_COMMA},
        {"period",      GLFW_KEY_PERIOD},
        {"slash",       GLFW_KEY_SLASH},
        {"backslash",   GLFW_KEY_BACKSLASH},
        {"semicolon",   GLFW_KEY_SEMICOLON},
        {"apostrophe",  GLFW_KEY_APOSTROPHE},
        {"grave",       GLFW_KEY_GRAVE_ACCENT},
        {"space",       GLFW_KEY_SPACE},
        {"BackSpace",   GLFW_KEY_BACKSPACE},
        {"backspace",   GLFW_KEY_BACKSPACE},
        {"Tab",         GLFW_KEY_TAB},
        {"tab",         GLFW_KEY_TAB},
        {"Return",      GLFW_KEY_ENTER},
        {"return",      GLFW_KEY_ENTER},
        {"Escape",      GLFW_KEY_ESCAPE},
        {"escape",      GLFW_KEY_ESCAPE},
        {"Prior",       GLFW_KEY_PAGE_UP},
        {"prior",       GLFW_KEY_PAGE_UP},
        {"Page_Up",     GLFW_KEY_PAGE_UP},
        {"Next",        GLFW_KEY_PAGE_DOWN},
        {"next",        GLFW_KEY_PAGE_DOWN},
        {"Page_Down",   GLFW_KEY_PAGE_DOWN},
        {"Home",        GLFW_KEY_HOME},
        {"home",        GLFW_KEY_HOME},
        {"End",         GLFW_KEY_END},
        {"end",         GLFW_KEY_END},
        {"Insert",      GLFW_KEY_INSERT},
        {"insert",      GLFW_KEY_INSERT},
        {"Delete",      GLFW_KEY_DELETE},
        {"delete",      GLFW_KEY_DELETE},
        {"Up",          GLFW_KEY_UP},
        {"Down",        GLFW_KEY_DOWN},
        {"Left",        GLFW_KEY_LEFT},
        {"Right",       GLFW_KEY_RIGHT},
        {"F1",          GLFW_KEY_F1},
        {"F2",          GLFW_KEY_F2},
        {"F3",          GLFW_KEY_F3},
        {"F4",          GLFW_KEY_F4},
        {"F5",          GLFW_KEY_F5},
        {"F6",          GLFW_KEY_F6},
        {"F7",          GLFW_KEY_F7},
        {"F8",          GLFW_KEY_F8},
        {"F9",          GLFW_KEY_F9},
        {"F10",         GLFW_KEY_F10},
        {"F11",         GLFW_KEY_F11},
        {"F12",         GLFW_KEY_F12},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].key;
    }

    return -1;
}

/* GLFW_KEY → 이름. input_glfw_key_from_name 의 역방향이며 같은 표기를 쓴다.
 * 표현할 수 없으면 NULL. */
static const char *key_name_from_glfw(int key, char *scratch, size_t scratch_size)
{
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        snprintf(scratch, scratch_size, "%c", 'a' + (key - GLFW_KEY_A));
        return scratch;
    }
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        snprintf(scratch, scratch_size, "%c", '0' + (key - GLFW_KEY_0));
        return scratch;
    }
    switch (key) {
    case GLFW_KEY_MINUS:         return "minus";
    case GLFW_KEY_EQUAL:         return "equal";
    case GLFW_KEY_COMMA:         return "comma";
    case GLFW_KEY_PERIOD:        return "period";
    case GLFW_KEY_SLASH:         return "slash";
    case GLFW_KEY_BACKSLASH:     return "backslash";
    case GLFW_KEY_SEMICOLON:     return "semicolon";
    case GLFW_KEY_APOSTROPHE:    return "apostrophe";
    case GLFW_KEY_GRAVE_ACCENT:  return "grave";
    case GLFW_KEY_SPACE:         return "space";
    case GLFW_KEY_BACKSPACE:     return "BackSpace";
    case GLFW_KEY_TAB:           return "Tab";
    case GLFW_KEY_ENTER:         return "Return";
    case GLFW_KEY_ESCAPE:        return "Escape";
    case GLFW_KEY_PAGE_UP:       return "Prior";
    case GLFW_KEY_PAGE_DOWN:     return "Next";
    case GLFW_KEY_HOME:          return "Home";
    case GLFW_KEY_END:           return "End";
    case GLFW_KEY_INSERT:        return "Insert";
    case GLFW_KEY_DELETE:        return "Delete";
    case GLFW_KEY_UP:            return "Up";
    case GLFW_KEY_DOWN:          return "Down";
    case GLFW_KEY_LEFT:          return "Left";
    case GLFW_KEY_RIGHT:         return "Right";
    default: break;
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) {
        snprintf(scratch, scratch_size, "F%d", 1 + (key - GLFW_KEY_F1));
        return scratch;
    }
    return NULL;
}

int input_keybind_format(int glfw_key, unsigned int mods,
                          char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    out[0] = '\0';

    /* 모디파이어 자체는 바인딩의 키가 될 수 없다. */
    switch (glfw_key) {
    case GLFW_KEY_LEFT_SHIFT:   case GLFW_KEY_RIGHT_SHIFT:
    case GLFW_KEY_LEFT_CONTROL: case GLFW_KEY_RIGHT_CONTROL:
    case GLFW_KEY_LEFT_ALT:     case GLFW_KEY_RIGHT_ALT:
    case GLFW_KEY_LEFT_SUPER:   case GLFW_KEY_RIGHT_SUPER:
        return -1;
    default: break;
    }

    char scratch[8];
    const char *name = key_name_from_glfw(glfw_key, scratch, sizeof scratch);
    if (!name) return -1;

    /* 파서가 '+' 로 토큰을 나누므로 순서는 자유지만 읽기 쉽게 고정한다. */
    int n = snprintf(out, out_size, "%s%s%s%s",
                     (mods & INPUT_MOD_CTRL)  ? "Ctrl+"  : "",
                     (mods & INPUT_MOD_ALT)   ? "Alt+"   : "",
                     (mods & INPUT_MOD_SHIFT) ? "Shift+" : "",
                     name);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

int keybind_matches(const char *binding, unsigned int mods, int glfw_key)
{
    if (!binding || !binding[0]) return 0;

    char buf[64];
    strncpy(buf, binding, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    unsigned int req_mods = 0;
    char *p = buf;
    char *plus;

    while ((plus = strchr(p, '+')) != NULL) {
        *plus = '\0';
        if      (strcmp(p, "Alt")   == 0) req_mods |= INPUT_MOD_ALT;
        else if (strcmp(p, "Ctrl")  == 0) req_mods |= INPUT_MOD_CTRL;
        else if (strcmp(p, "Shift") == 0) req_mods |= INPUT_MOD_SHIFT;
        p = plus + 1;
    }

    int req_key = input_glfw_key_from_name(p);
    if (req_key < 0) return 0;

    return (mods == req_mods) && (glfw_key == req_key);
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int write_tilde(uint8_t *buf, int size, int n)
{
    return snprintf((char*)buf, (size_t)size, "\x1b[%d~", n);
}

static int write_esc(uint8_t *buf, int size, const char *seq, int alt)
{
    int len = 0;
    if (alt && size > 0) { buf[len++] = '\x1b'; size--; }
    int slen = (int)strlen(seq);
    if (slen > size) slen = size;
    memcpy(buf + len, seq, (size_t)slen);
    return len + slen;
}

/* ── Key → byte translation ─────────────────────────────────────────────── */

int input_key_to_bytes(int key, unsigned int mods,
                        uint8_t *buf, int size)
{
    if (!buf || size <= 0) return 0;

    int alt   = (mods & INPUT_MOD_ALT)  ? 1 : 0;
    int ctrl  = (mods & INPUT_MOD_CTRL) ? 1 : 0;

    /* ── Ctrl + letter → C0 control byte ────────────────────────────────── */
    if (ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        int byte = key - GLFW_KEY_A + 1; /* Ctrl-A=1 … Ctrl-Z=26 */
        if (alt && size >= 2) { buf[0] = '\x1b'; buf[1] = (uint8_t)byte; return 2; }
        buf[0] = (uint8_t)byte;
        return 1;
    }

    /* ── Special keys ───────────────────────────────────────────────────── */
    switch (key) {
    case GLFW_KEY_BACKSPACE: return write_esc(buf, size, "\x7f", alt);
    case GLFW_KEY_TAB:       return write_esc(buf, size, "\t",   alt);
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:  return write_esc(buf, size, "\r",   alt);
    case GLFW_KEY_ESCAPE:    return write_esc(buf, size, "\x1b", alt);

    /* Cursor movement */
    case GLFW_KEY_UP:        return write_esc(buf, size, "\x1b[A", alt);
    case GLFW_KEY_DOWN:      return write_esc(buf, size, "\x1b[B", alt);
    case GLFW_KEY_RIGHT:     return write_esc(buf, size, "\x1b[C", alt);
    case GLFW_KEY_LEFT:      return write_esc(buf, size, "\x1b[D", alt);

    /* Home / End */
    case GLFW_KEY_HOME:      return write_esc(buf, size, "\x1b[H", alt);
    case GLFW_KEY_END:       return write_esc(buf, size, "\x1b[F", alt);

    /* Editing */
    case GLFW_KEY_INSERT:    return write_tilde(buf, size, 2);
    case GLFW_KEY_DELETE:    return write_tilde(buf, size, 3);
    case GLFW_KEY_PAGE_UP:   return write_tilde(buf, size, 5);
    case GLFW_KEY_PAGE_DOWN: return write_tilde(buf, size, 6);

    /* Function keys */
    case GLFW_KEY_F1:  return write_esc(buf, size, "\x1bOP",  alt);
    case GLFW_KEY_F2:  return write_esc(buf, size, "\x1bOQ",  alt);
    case GLFW_KEY_F3:  return write_esc(buf, size, "\x1bOR",  alt);
    case GLFW_KEY_F4:  return write_esc(buf, size, "\x1bOS",  alt);
    case GLFW_KEY_F5:  return write_tilde(buf, size, 15);
    case GLFW_KEY_F6:  return write_tilde(buf, size, 17);
    case GLFW_KEY_F7:  return write_tilde(buf, size, 18);
    case GLFW_KEY_F8:  return write_tilde(buf, size, 19);
    case GLFW_KEY_F9:  return write_tilde(buf, size, 20);
    case GLFW_KEY_F10: return write_tilde(buf, size, 21);
    case GLFW_KEY_F11: return write_tilde(buf, size, 23);
    case GLFW_KEY_F12: return write_tilde(buf, size, 24);

    /* Ctrl codes for punctuation */
    case GLFW_KEY_LEFT_BRACKET:
        if (ctrl) return write_esc(buf, size, "\x1b", alt); /* Ctrl-[ = ESC */
        break;
    case GLFW_KEY_BACKSLASH:
        if (ctrl) { buf[0] = '\x1c'; return 1; }
        break;
    case GLFW_KEY_RIGHT_BRACKET:
        if (ctrl) { buf[0] = '\x1d'; return 1; }
        break;

    default:
        break;
    }

    /* ── Alt + letter (no Ctrl) → ESC + lowercase ────────────────────────── */
    if (alt && !ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z && size >= 2) {
        buf[0] = '\x1b';
        buf[1] = (uint8_t)('a' + (key - GLFW_KEY_A));
        return 2;
    }

    /* Unhandled — printable text comes via GLFW CharCallback instead. */
    return 0;
}

/* ── Mouse → SGR bytes ──────────────────────────────────────────────────── */

int input_mouse_to_bytes(int col, int row, int button, int press,
                          unsigned int modifiers,
                          uint8_t *buf, int size)
{
    int btn_code = button & 0x3;
    if (modifiers & INPUT_MOD_SHIFT) btn_code |= 4;
    if (modifiers & INPUT_MOD_ALT)   btn_code |= 8;
    if (modifiers & INPUT_MOD_CTRL)  btn_code |= 16;

    return snprintf((char*)buf, (size_t)size, "\x1b[<%d;%d;%d%c",
                    btn_code, col, row, press ? 'M' : 'm');
}
