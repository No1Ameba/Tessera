/*
 * event_loop_win.c — Windows IOCP 기반 이벤트 루프 (스켈레톤, 미구현).
 *
 * ⚠️ 미구현: 데몬을 Windows 에서 돌리려면 이 파일을 IOCP(I/O Completion Ports)로
 *    구현해야 한다. epoll(readiness)과 IOCP(completion)는 모델이 반대라 단순 치환이
 *    아니다:
 *      - epoll: "이 fd 를 읽을 수 있음" 알림 → 그때 read
 *      - IOCP:  미리 overlapped ReadFile 을 걸어둠 → 완료되면 GetQueuedCompletionStatus
 *    따라서 이 어댑터는 evloop_add 시 각 핸들에 대해 overlapped 읽기를 선발행하고,
 *    evloop_wait(GetQueuedCompletionStatus)에서 완료를 EV_READ readiness 로 변환해
 *    호출자(ipc_server.c)에게 넘겨야 한다. 난제: 핸들↔fd 매핑, per-connection
 *    OVERLAPPED + 부분 읽기 버퍼, 파이프 broken → EV_HANGUP 매핑, 그리고 ipc.h
 *    (Named Pipe)·tessera_pty.h(ConPTY) 의 HANDLE↔fd 브릿지 통일.
 *
 * 현재는 심볼만 제공하는 실패 스텁이다(플랫폼 라이브러리 링크용). 데몬 실행 파일은
 * 아직 Windows 빌드 대상이 아니다.
 */
#include "../event_loop.h"

#include <windows.h>
#include <stdlib.h>

struct event_loop {
    HANDLE iocp;   /* CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...) */
    /* TODO: 핸들↔fd 매핑 테이블, 연결별 OVERLAPPED + 읽기 버퍼 */
};

event_loop_t *evloop_create(void) {
    /* TODO: el->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0); */
    return NULL;  /* 미구현 */
}

void evloop_destroy(event_loop_t *el) {
    if (!el) return;
    if (el->iocp) CloseHandle(el->iocp);
    free(el);
}

int evloop_add(event_loop_t *el, int fd, uint32_t interest) {
    (void)el; (void)fd; (void)interest;
    return -1;  /* TODO: 핸들을 iocp 에 연결 + overlapped 읽기 선발행 */
}

int evloop_del(event_loop_t *el, int fd) {
    (void)el; (void)fd;
    return -1;  /* TODO: 대기 중 overlapped 취소 + 매핑 제거 */
}

int evloop_wait(event_loop_t *el, ev_ready_t *out, int max, int timeout_ms) {
    (void)el; (void)out; (void)max; (void)timeout_ms;
    return -1;  /* TODO: GetQueuedCompletionStatus → EV_READ readiness 변환 */
}
