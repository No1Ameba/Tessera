#include "confirm_dialog.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear.h"

#include <GLFW/glfw3.h>
#include <string.h>

static struct {
    int             open;
    confirm_kind_t  kind;
    char            title[128];
    char            body[512];
    bool           *dont_ask;       /* config 필드 주소 (true=여전히 확인 요구) */
    int             dont_ask_ui;    /* 체크박스 UI 상태 (체크 시 "다시 묻지 않기") */
    confirm_cb_t    on_confirm;
    void           *user;
    int             prev_esc;       /* Esc 에지 감지 */
    int             prev_enter;
    int             frame_seen;     /* 팝업이 렌더된 최초 프레임 무시(이전 엔터/Esc 스파이크 제거) */
} g_dlg = {0};

void confirm_dialog_open(confirm_kind_t kind,
                          const char *title, const char *body,
                          bool *dont_ask,
                          confirm_cb_t on_confirm, void *user)
{
    if (g_dlg.open) return;
    memset(&g_dlg, 0, sizeof g_dlg);
    g_dlg.open = 1;
    g_dlg.kind = kind;
    strncpy(g_dlg.title, title ? title : "Confirm",  sizeof g_dlg.title - 1);
    strncpy(g_dlg.body,  body  ? body  : "",         sizeof g_dlg.body  - 1);
    g_dlg.dont_ask   = dont_ask;
    g_dlg.dont_ask_ui = 0;
    g_dlg.on_confirm = on_confirm;
    g_dlg.user       = user;
    g_dlg.prev_esc   = 1;  /* 오픈 순간 눌린 키는 무시 */
    g_dlg.prev_enter = 1;
    g_dlg.frame_seen = 0;
}

int confirm_dialog_is_open(void) { return g_dlg.open; }

void confirm_dialog_close(void) { g_dlg.open = 0; }

static void apply_dont_ask(void)
{
    if (g_dlg.dont_ask && g_dlg.dont_ask_ui) {
        /* 체크 시 이후로 확인 요구 안 함 */
        *g_dlg.dont_ask = false;
    }
}

/* 본문 줄 수 추정 — \n 개수 + 창 폭 대비 긴 줄 wrap 추가 보정. */
static int estimate_body_lines(const char *body, int char_per_line)
{
    if (!body || !body[0]) return 1;
    if (char_per_line < 10) char_per_line = 10;
    int lines = 0;
    const char *s = body;
    while (*s) {
        const char *e = strchr(s, '\n');
        int len = e ? (int)(e - s) : (int)strlen(s);
        int sub = (len == 0) ? 1 : (len + char_per_line - 1) / char_per_line;
        lines += sub;
        if (!e) break;
        s = e + 1;
    }
    if (lines < 1) lines = 1;
    return lines;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void confirm_dialog_draw(struct nk_context *ctx,
                          struct GLFWwindow *win,
                          int win_w, int win_h)
{
    if (!g_dlg.open || !ctx) return;

    /* 키 입력 감지 (첫 프레임은 스킵 — 호출 시점의 잔여 키 제거). */
    int esc_down   = win ? glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS : 0;
    int enter_down = win ? (glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS ||
                             glfwGetKey(win, GLFW_KEY_KP_ENTER) == GLFW_PRESS) : 0;

    /* ── 반응형 크기 계산 ───────────────────────────────────────────────────
     * 폭: 화면 대비 60% (하한 360, 상한 640)
     * 본문 높이: 추정 라인 수 * 줄높이, 화면의 40% 까지만 확장. 넘치면 스크롤.
     * 전체 높이: 타이틀/체크박스/버튼 고정 영역 + 본문 높이.
     *
     * Nuklear 는 각 `nk_layout_row_*` 사이에 `window.spacing.y` 를 삽입하고,
     * 헤더(`NK_WINDOW_TITLE`) 높이도 폰트+padding 에 의존한다. 실제 런타임 스타일
     * 값을 읽어 정확히 반영하지 않으면 버튼이 창 경계 아래로 잘린다. */
    const float line_h       = 22.0f;
    const float buttons_h    = 34.0f;
    const float spacing1     = 6.0f;
    const float body_min_h   = 48.0f;
    const float dont_ask_h   = (g_dlg.dont_ask ? 26.0f : 0.0f);
    const float dont_ask_gap = (g_dlg.dont_ask ? 4.0f  : 0.0f);

    /* 실제 스타일 값 읽기 (기본값 fallback 포함) */
    float row_gap  = ctx->style.window.spacing.y;
    float wp_y     = ctx->style.window.padding.y;
    float font_h   = (ctx->style.font && ctx->style.font->height > 0)
                     ? ctx->style.font->height : 16.0f;
    /* 헤더 높이 = 폰트 + label_padding*2 + header_padding*2 */
    float header_h = font_h
                   + 2.0f * ctx->style.window.header.label_padding.y
                   + 2.0f * ctx->style.window.header.padding.y;
    if (header_h < 28.0f) header_h = 28.0f;
    /* window 외곽 border (NK_WINDOW_BORDER) — 상/하 각각 */
    float border_total = 2.0f * 2.0f;
    /* 버튼 뒤에 들어갈 trailing spacer (잘릴 때 스페이서가 먼저 잘리도록) */
    const float trailing_spacer_h = 6.0f;

    float dw = clampf((float)win_w * 0.6f, 360.0f, 640.0f);
    if (dw > (float)win_w - 40.0f) dw = (float)win_w - 40.0f;
    if (dw < 240.0f) dw = 240.0f;

    int char_per_line = (int)((dw - 40.0f) / 7.5f);
    int body_lines   = estimate_body_lines(g_dlg.body, char_per_line);

    float body_max_h  = clampf((float)win_h * 0.40f, 120.0f, 360.0f);
    float body_wanted = (float)body_lines * line_h + 12.0f;
    float body_h      = clampf(body_wanted, body_min_h, body_max_h);

    /* outer row 개수 — 본문 그룹 + (gap+checkbox)? + spacing spacer + buttons + trailing spacer */
    int rows = g_dlg.dont_ask ? 6 : 4;
    float row_gaps_total = (rows - 1) * row_gap;

    float controls_h =
        (g_dlg.dont_ask ? (dont_ask_gap + dont_ask_h) : 0.0f) +
        spacing1 + buttons_h + trailing_spacer_h;

    /* chrome: header + window border + 상/하 window padding + 폰트 metric 추정 오차 여유 */
    float chrome_h = header_h + border_total + 2.0f * wp_y + 16.0f;

    float fixed_h = chrome_h + controls_h + row_gaps_total;

    float dh = fixed_h + body_h;
    if (dh > (float)win_h - 40.0f) {
        float overflow = dh - ((float)win_h - 40.0f);
        body_h -= overflow;
        if (body_h < body_min_h) body_h = body_min_h;
        dh = fixed_h + body_h;
        if (dh > (float)win_h) dh = (float)win_h;
    }

    float dx = ((float)win_w - dw) * 0.5f;
    float dy = ((float)win_h - dh) * 0.5f;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;

    /* 창 타이틀은 NK_WINDOW_TITLE 로 표시 → nk_begin_titled 로 캡션 지정. */
    int begun = nk_begin_titled(ctx, "Confirm", g_dlg.title,
                                 nk_rect(dx, dy, dw, dh),
                                 NK_WINDOW_BORDER | NK_WINDOW_TITLE |
                                 NK_WINDOW_NO_SCROLLBAR);
    nk_window_set_focus(ctx, "Confirm");

    int confirm_clicked = 0;
    int cancel_clicked  = 0;

    if (begun) {
        /* ── 본문 (스크롤 가능한 group) ─────────────────────────────────── */
        nk_layout_row_dynamic(ctx, body_h, 1);
        if (nk_group_begin(ctx, "confirm_body", NK_WINDOW_BORDER)) {
            const char *s = g_dlg.body;
            while (*s) {
                const char *e = strchr(s, '\n');
                int len = e ? (int)(e - s) : (int)strlen(s);
                char line[512];
                if (len >= (int)sizeof line) len = (int)sizeof line - 1;
                memcpy(line, s, (size_t)len);
                line[len] = '\0';
                nk_layout_row_dynamic(ctx, line_h, 1);
                if (len == 0) nk_spacing(ctx, 1);
                else           nk_label_wrap(ctx, line);
                if (!e) break;
                s = e + 1;
            }
            nk_group_end(ctx);
        }

        /* ── "Don't ask again" ─────────────────────────────────────────── */
        if (g_dlg.dont_ask) {
            nk_layout_row_dynamic(ctx, dont_ask_gap, 1);
            nk_spacing(ctx, 1);
            nk_layout_row_dynamic(ctx, dont_ask_h, 1);
            nk_checkbox_label(ctx, "Don't ask again", &g_dlg.dont_ask_ui);
        }

        /* ── 버튼 (Cancel / Close) ─────────────────────────────────────── */
        nk_layout_row_dynamic(ctx, spacing1, 1);
        nk_spacing(ctx, 1);

        nk_layout_row_dynamic(ctx, buttons_h, 2);
        if (nk_button_label(ctx, "Cancel")) cancel_clicked = 1;
        {
            struct nk_style_button red = ctx->style.button;
            red.normal = nk_style_item_color(nk_rgb(160, 50, 50));
            red.hover  = nk_style_item_color(nk_rgb(200, 70, 70));
            red.active = nk_style_item_color(nk_rgb(120, 40, 40));
            red.text_normal = nk_rgb(255, 255, 255);
            red.text_hover  = nk_rgb(255, 255, 255);
            red.text_active = nk_rgb(255, 255, 255);
            if (nk_button_label_styled(ctx, &red, "Close")) confirm_clicked = 1;
        }

        /* trailing spacer — 계산 오차로 하단이 잘릴 경우 버튼 대신 이게 잘리도록. */
        nk_layout_row_dynamic(ctx, trailing_spacer_h, 1);
        nk_spacing(ctx, 1);

        /* 키보드 단축 — Enter / Esc 모두 Cancel (기본 포커스 = 안전쪽). */
        if (g_dlg.frame_seen) {
            int esc_edge   = esc_down   && !g_dlg.prev_esc;
            int enter_edge = enter_down && !g_dlg.prev_enter;
            if (esc_edge || enter_edge) cancel_clicked = 1;
        }
    }
    nk_end(ctx);

    if (confirm_clicked) {
        apply_dont_ask();
        confirm_cb_t cb = g_dlg.on_confirm;
        void *u         = g_dlg.user;
        g_dlg.open = 0;
        if (cb) cb(u);
    } else if (cancel_clicked) {
        g_dlg.open = 0;
    }

    g_dlg.prev_esc   = esc_down;
    g_dlg.prev_enter = enter_down;
    g_dlg.frame_seen = 1;
}
