#ifndef TERMEMU_CONFIRM_DIALOG_H
#define TERMEMU_CONFIRM_DIALOG_H

#include <stdbool.h>

/*
 * 파괴적 동작(pane/window/session 닫기)에 대한 Nuklear 모달 확인 팝업.
 *
 * - 한 번에 하나만 표시되는 싱글톤.
 * - "다시 묻지 않기" 체크박스는 호출측이 전달한 `dont_ask` 포인터를 갱신한다.
 *   (이 포인터는 해당 kind 에 대응하는 termemu_config_t 필드 주소)
 * - Esc 또는 Cancel 버튼 = 취소. Enter 또는 Confirm 버튼 = 승인.
 * - 기본 포커스는 Cancel 쪽 (실수 방지).
 */

struct nk_context;
struct GLFWwindow;

typedef enum {
    CONFIRM_KIND_PANE    = 0,
    CONFIRM_KIND_WINDOW  = 1,
    CONFIRM_KIND_SESSION = 2,
} confirm_kind_t;

/* 승인 시 호출될 콜백. user 는 호출자가 open 시 넘긴 값. */
typedef void (*confirm_cb_t)(void *user);

/*
 * 팝업을 연다.
 *   kind     — 표시용 종류
 *   title    — 창 타이틀 (예: "Close Pane?")
 *   body     — 본문 문구 (여러 줄 \n 허용)
 *   dont_ask — "다시 묻지 않기" 체크박스가 연결될 bool (NULL 이면 체크박스 숨김).
 *              체크 시 false 가 되도록 호출측이 해석 (즉, 이 필드는 confirm_close_* 로
 *              "확인 요구 여부" 를 담는다).
 *   on_confirm/user — 승인 시 실행할 콜백.
 *
 * 이미 열려있으면 아무 일도 하지 않는다 (중첩 방지).
 */
void confirm_dialog_open(confirm_kind_t kind,
                          const char *title, const char *body,
                          bool *dont_ask,
                          confirm_cb_t on_confirm, void *user);

/* 현재 팝업이 열려있는지. */
int confirm_dialog_is_open(void);

/* 외부에서 강제로 닫기 (취소 처리 없이). */
void confirm_dialog_close(void);

/*
 * 매 프레임 Nuklear 렌더 루프 안에서 호출.
 * win_w/win_h 는 화면 중앙 정렬용 (프레임버퍼 크기).
 * 열려 있지 않으면 아무 일도 하지 않는다.
 * Esc 키는 호출 전에 glfwGetKey 로 감지 → 취소.
 */
void confirm_dialog_draw(struct nk_context *ctx,
                          struct GLFWwindow *win,
                          int win_w, int win_h);

#endif /* TERMEMU_CONFIRM_DIALOG_H */
