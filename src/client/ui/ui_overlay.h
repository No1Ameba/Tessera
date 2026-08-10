#ifndef TESSERA_UI_OVERLAY_H
#define TESSERA_UI_OVERLAY_H

/*
 * Nuklear 오버레이(설정/확인/컨텍스트 메뉴 등) 의 rect 계산 헬퍼.
 *
 * 확인 팝업(`confirm_dialog.c`) 에서 이미 검증된 "비율 + min/max clamp" 패턴을
 * 다른 오버레이와 공유한다. Nuklear 자체에는 화면 기준 배치/클램프 기능이 없으므로
 * 호출측에서 `nk_rect(...)` 에 넘길 값을 직접 계산해야 한다.
 */

/*
 * 화면 중앙에 정렬된 오버레이 rect 를 계산한다.
 * 너비/높이는 `win_w * ratio_w` / `win_h * ratio_h` 를 기준으로 min/max 로 클램프하며,
 * 화면 마진(20px) 을 보장해 창 테두리에 너무 붙지 않도록 한다.
 */
void ui_overlay_centered_rect(float win_w, float win_h,
                               float ratio_w, float ratio_h,
                               float min_w, float max_w,
                               float min_h, float max_h,
                               float *out_x, float *out_y,
                               float *out_w, float *out_h);

/*
 * 앵커 좌표(예: 마우스 클릭 위치) 에 띄울 팝업의 rect 를 계산한다.
 * 선호는 앵커 기준 오른쪽·아래 방향이지만 화면 밖으로 넘치면 위/왼쪽으로 뒤집어
 * 항상 화면 안에 들어오도록 한다. `want_w`/`want_h` 가 화면보다 크면 화면에 맞춰 축소.
 */
void ui_overlay_popup_at(float anchor_x, float anchor_y,
                          float want_w, float want_h,
                          float win_w, float win_h,
                          float *out_x, float *out_y,
                          float *out_w, float *out_h);

#endif /* TESSERA_UI_OVERLAY_H */
