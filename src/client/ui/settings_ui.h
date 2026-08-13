#ifndef TESSERA_SETTINGS_UI_H
#define TESSERA_SETTINGS_UI_H

/*
 * Nuklear 기반 설정 오버레이 UI
 *
 * Ctrl+comma 토글 시 터미널 위에 표시된다.
 * 변경 사항은 config.json / theme.json 에 저장된다.
 */

struct nk_context;

#include "../../common/config.h"

/*
 * 설정 패널을 그린다.
 * win_w / win_h: 현재 프레임버퍼 크기 (반응형 rect 계산에 사용).
 * @return  1  설정이 저장됨 (Save & Apply)
 *          0  변경 없음
 *         -1  Close 요청 (창 닫히기 클릭 또는 Close 버튼)
 */
int settings_ui_draw(struct nk_context *ctx,
                      tessera_config_t *cfg,
                      tessera_theme_t *theme,
                      const char *cfg_path,
                      const char *theme_path,
                      float win_w, float win_h);

/* ── 단축키 직접 입력(캡처) ─────────────────────────────────────────────── */

/*
 * 단축키 행의 "Set" 을 누르면 캡처 모드가 되고, 다음에 누른 키 조합이 그대로
 * 바인딩이 된다. 문자열("Alt+equal")을 손으로 적는 것보다 정확하고, 특정 조합이
 * 실제로 앱까지 도달하는지도 바로 확인된다.
 *
 * 캡처 중에는 키 입력이 Nuklear 로 가면 안 되므로, 호출측 key_callback 이
 * settings_ui_is_capturing() 을 먼저 확인하고 settings_ui_capture_key() 로
 * 넘겨야 한다.
 */
int  settings_ui_is_capturing(void);

/*
 * 캡처 중인 바인딩에 키를 기록한다.
 * 모디파이어 단독 키는 무시하고 계속 기다린다. Escape 는 취소.
 * @return 1 캡처가 끝났거나 취소됨, 0 계속 대기 중.
 */
int  settings_ui_capture_key(int glfw_key, unsigned int mod_flags);

/* 설정창이 닫힐 때 캡처 상태를 정리한다. */
void settings_ui_cancel_capture(void);

#endif /* TESSERA_SETTINGS_UI_H */
