/*
 * event_loop_posix.c — epoll 기반 이벤트 루프 (readiness 모델).
 * 데몬의 다중 fd 감시를 event_loop.h 계약으로 감싼다.
 */
#define _GNU_SOURCE
#include "../event_loop.h"

#include <sys/epoll.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define EVLOOP_BATCH_MAX 64

struct event_loop {
    int epoll_fd;
};

static uint32_t to_epoll(uint32_t interest) {
    uint32_t e = 0;
    /* EV_ACCEPT: listen 소켓은 연결이 도착하면 그냥 readable 해지므로 EV_READ 와
     * 같다. 구분이 필요한 쪽은 Windows(ConnectNamedPipe) 뿐이다. */
    if (interest & (EV_READ | EV_ACCEPT)) e |= EPOLLIN;
    if (interest & EV_EDGE) e |= EPOLLET;
    return e;
}

event_loop_t *evloop_create(void) {
    event_loop_t *el = calloc(1, sizeof *el);
    if (!el) return NULL;
    el->epoll_fd = epoll_create1(0);
    if (el->epoll_fd < 0) { free(el); return NULL; }
    return el;
}

void evloop_destroy(event_loop_t *el) {
    if (!el) return;
    if (el->epoll_fd >= 0) close(el->epoll_fd);
    free(el);
}

int evloop_add(event_loop_t *el, int fd, uint32_t interest) {
    if (!el) { errno = EINVAL; return -1; }
    struct epoll_event ev;
    ev.events  = to_epoll(interest);
    ev.data.fd = fd;
    return epoll_ctl(el->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int evloop_del(event_loop_t *el, int fd) {
    if (!el) { errno = EINVAL; return -1; }
    return epoll_ctl(el->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int evloop_mod(event_loop_t *el, int fd, uint32_t interest) {
    if (!el) { errno = EINVAL; return -1; }
    struct epoll_event ev;
    ev.events  = to_epoll(interest);
    ev.data.fd = fd;
    return epoll_ctl(el->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int evloop_wait(event_loop_t *el, ev_ready_t *out, int max, int timeout_ms) {
    if (!el || !out || max <= 0) { errno = EINVAL; return -1; }
    if (max > EVLOOP_BATCH_MAX) max = EVLOOP_BATCH_MAX;

    struct epoll_event evs[EVLOOP_BATCH_MAX];
    int n = epoll_wait(el->epoll_fd, evs, max, timeout_ms);
    if (n < 0) return -1;  /* errno 보존 (EINTR 는 호출자가 재시도) */

    for (int i = 0; i < n; i++) {
        out[i].fd = evs[i].data.fd;
        uint32_t r = 0;
        if (evs[i].events & EPOLLIN)  r |= EV_READ;
        if (evs[i].events & EPOLLHUP) r |= EV_HANGUP;
        if (evs[i].events & EPOLLERR) r |= EV_ERROR;
        out[i].revents = r;
    }
    return n;
}
