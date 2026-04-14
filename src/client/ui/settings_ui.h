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
 * @return 1 if settings were modified and saved, 0 otherwise.
 */
int settings_ui_draw(struct nk_context *ctx,
                      termemu_config_t *cfg,
                      termemu_theme_t *theme,
                      const char *cfg_path,
                      const char *theme_path);

#endif /* TERMEMU_SETTINGS_UI_H */
