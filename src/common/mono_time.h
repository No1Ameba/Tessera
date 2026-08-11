#ifndef TESSERA_MONO_TIME_H
#define TESSERA_MONO_TIME_H

#include <stdint.h>

/*
 * 모노토닉 시계(밀리초).
 *
 * 벽시계가 아니라 단조 증가만 보장하는 시계다 — NTP 보정이나 사용자의 시계 변경에
 * 영향을 받지 않아야 하는 타임아웃/블링크/더블클릭 판정에 쓴다. 기준점(0)은
 * 플랫폼마다 다르므로 절대값에 의미를 두지 말고 항상 차이만 사용할 것.
 *
 * POSIX 는 clock_gettime(CLOCK_MONOTONIC), Windows 는 QueryPerformanceCounter 로
 * 구현된다. clock_gettime 은 MSVC 에 없으므로 호출부에서 직접 쓰지 말 것.
 */
int64_t tessera_mono_ms(void);

#endif /* TESSERA_MONO_TIME_H */
