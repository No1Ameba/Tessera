#ifndef TERMEMU_UTF8_H
#define TERMEMU_UTF8_H

/*
 * UTF-8 유틸리티
 *
 * vt_parser 의 인라인 디코더와 중복되지 않도록,
 * 이 모듈은 "터미널 렌더링에 필요한 상위 연산"을 제공한다.
 *
 *   - utf8_encode      : 코드 포인트 → UTF-8 바이트열
 *   - utf8_decode_one  : 버퍼에서 코드 포인트 1개 디코드
 *   - utf8_char_width  : 터미널 열(column) 너비 (0/1/2)
 *   - utf8_str_width   : UTF-8 문자열의 총 열 너비
 *   - utf8_cp_count    : UTF-8 문자열의 코드 포인트 수
 */

#include <stddef.h>
#include <stdint.h>

/*
 * 코드 포인트를 UTF-8 바이트열로 인코딩한다.
 * buf  : 최소 4바이트 이상의 버퍼.
 * 반환 : 기록된 바이트 수 (1–4). 유효하지 않은 코드 포인트면 0.
 */
int utf8_encode(uint32_t codepoint, char buf[4]);

/*
 * 버퍼 앞에서 코드 포인트 1개를 디코드한다.
 * out  : 디코드된 코드 포인트 (NULL 가능).
 * 반환 : 소비된 바이트 수. 유효하지 않으면 1(오류 바이트 1개 건너뜀).
 * 유효하지 않은 시퀀스는 U+FFFD 를 *out 에 저장한다.
 */
int utf8_decode_one(const char *buf, size_t len, uint32_t *out);

/*
 * 코드 포인트의 터미널 열 너비를 반환한다.
 *   0 : 결합 문자(combining), NUL, 제어 문자
 *   1 : 일반 문자 (ASCII, 라틴 등)
 *   2 : 전각 문자 — CJK, 한글, 이모지 등
 */
int utf8_char_width(uint32_t codepoint);

/* NUL-terminated UTF-8 문자열의 총 터미널 열 너비 */
size_t utf8_str_width(const char *str);

/* NUL-terminated UTF-8 문자열의 코드 포인트 수 */
size_t utf8_cp_count(const char *str);

#endif /* TERMEMU_UTF8_H */
