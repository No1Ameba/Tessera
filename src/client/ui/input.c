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
