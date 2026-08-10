#ifndef TERMEMU_EVENT_LOOP_H
#define TERMEMU_EVENT_LOOP_H

/*
 * 이벤트 루프 추상화 — 데몬이 여러 fd(클라이언트 소켓 + PTY 마스터)를 동시에
 * 감시하기 위한 플랫폼 독립 인터페이스.
 *
 * 플랫폼별 구현:
 *   POSIX   — posix/event_loop_posix.c  (epoll, readiness 모델)
 *   Windows — windows/event_loop_win.c  (IOCP, completion 모델 — 미구현 스켈레톤)
 *
 * 주의: epoll 은 "준비됨(readiness)" 모델이고 IOCP 는 "완료(completion)" 모델이라
 * 의미가 다르다. 이 인터페이스는 readiness 시맨틱(evloop_wait 이 "읽을 수 있는 fd"
 * 를 돌려줌)을 계약으로 삼으며, Windows 구현은 내부적으로 overlapped I/O 를 미리
 * 걸어두고 완료를 readiness 로 변환하는 어댑터를 두어야 한다(별도 작업).
 */

#include <stdint.h>

/* 관심(interest) / 발생(revents) 플래그. */
#define EV_READ    0x01u   /* 읽기 가능 */
#define EV_HANGUP  0x02u   /* 상대측 종료(HUP) — revents 전용 */
#define EV_ERROR   0x04u   /* 오류 — revents 전용 */
#define EV_EDGE    0x10u   /* 엣지 트리거 요청 — evloop_add interest 전용 */

typedef struct event_loop event_loop_t;

typedef struct {
    int      fd;       /* 이벤트가 발생한 fd */
    uint32_t revents;  /* EV_READ | EV_HANGUP | EV_ERROR 조합 */
} ev_ready_t;

/* 이벤트 루프 생성/파괴. 실패 시 NULL. */
event_loop_t *evloop_create(void);
void          evloop_destroy(event_loop_t *el);

/* fd 를 interest(EV_READ 및 선택적 EV_EDGE)로 감시 등록. 0 성공, -1 실패. */
int evloop_add(event_loop_t *el, int fd, uint32_t interest);

/* fd 감시 해제. 0 성공, -1 실패. */
int evloop_del(event_loop_t *el, int fd);

/*
 * 최대 max 개의 준비된 이벤트를 out[] 에 채우고 개수를 반환한다.
 * timeout_ms 경과 시 0. 오류 시 -1(errno 설정; 호출자는 EINTR 를 재시도로 처리).
 */
int evloop_wait(event_loop_t *el, ev_ready_t *out, int max, int timeout_ms);

#endif /* TERMEMU_EVENT_LOOP_H */
