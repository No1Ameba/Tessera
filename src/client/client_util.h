#ifndef TESSERA_CLIENT_UTIL_H
#define TESSERA_CLIENT_UTIL_H

/* 클라이언트 잡다한 무상태 헬퍼(전역 의존 없음). */

/* 모노토닉 시계(밀리초). 더블/트리플 클릭 타이밍 판정 등에 사용. */
long now_ms_mono(void);

/* URL 을 OS 기본 핸들러(xdg-open / macOS open)로 자식 프로세스로 분리 실행한다. */
void open_url(const char *url);

#endif /* TESSERA_CLIENT_UTIL_H */
