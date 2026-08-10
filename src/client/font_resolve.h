#ifndef TESSERA_FONT_RESOLVE_H
#define TESSERA_FONT_RESOLVE_H

#include <stddef.h>
#include "renderer/font.h"

/*
 * fontconfig(fc-match) 로 폰트 이름/패턴을 실제 파일 경로로 해석한다.
 * 절대경로가 주어지고 존재하면 그대로, fc-match 성공 시 그 경로를 buf 에 기록해
 * 반환, 실패 시 하드코딩 폴백 목록을 시도하고, 그것도 없으면 name 을 반환한다.
 */
const char *resolve_font_path(const char *name, char *buf, size_t bufsz);

/*
 * 주 monospace 폰트에 없는 CJK(특히 한글 U+AC00–U+D7A3) 코드포인트를 위해
 * fontconfig `:lang=ko/ja/zh` 로 커버 폰트를 자동 발견해 폴백 face 로 부착한다.
 * 주 폰트 자신과 중복은 건너뛴다.
 */
void add_cjk_fallbacks(font_face_t *font, const char *primary_path);

#endif /* TESSERA_FONT_RESOLVE_H */
