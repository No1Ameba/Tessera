#ifndef TERMEMU_SETTINGS_UI_H
#define TERMEMU_SETTINGS_UI_H

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
                      termemu_config_t *cfg,
                      termemu_theme_t *theme,
                      const char *cfg_path,
                      const char *theme_path,
                      float win_w, float win_h);

#endif /* TERMEMU_SETTINGS_UI_H */
