/*
 * event_loop_win.c — Windows IOCP 기반 이벤트 루프.
 *
 * event_loop.h 의 계약은 epoll 과 같은 "readiness"(읽을 수 있음) 시맨틱인데
 * IOCP 는 "completion"(다 읽었음) 모델이라 그대로는 맞지 않는다. 둘을 잇는 표준
 * 기법이 **0바이트 overlapped read** 다:
 *
 *   ReadFile(h, buf, 0, ...) 을 걸어두면 데이터가 도착하는 순간 완료되지만
 *   바이트는 하나도 소비하지 않는다. 즉 완료 = "이제 읽을 수 있다" 로,
 *   readiness 알림과 정확히 같은 의미가 된다.
 *
 * 덕분에 호출자(ipc_server.c)는 기존처럼 fd 에 대고 read() 를 그대로 쓰면 되고,
 * 이 어댑터가 읽기 버퍼를 소유할 필요도 없다.
 *
 * 재무장(re-arm): 완료된 워치는 armed=0 이 되고, 다음 evloop_wait() 진입 시 다시
 * 0바이트 read 를 건다. 호출자가 그 사이에 read() 로 데이터를 비웠다면 다음 도착을
 * 기다리고, 아직 남아 있다면 즉시 완료되어 다시 EV_READ 가 뜬다 — 레벨 트리거와
 * 같은 동작이다.
 *
 * 제약:
 *   - 등록하는 fd 의 HANDLE 은 반드시 FILE_FLAG_OVERLAPPED 로 열려 있어야 한다.
 *     CreatePipe() 가 만드는 익명 파이프는 불가하다(pty_win.c 가 이름 있는
 *     파이프를 쓰는 이유).
 *   - 핸들은 IOCP 에 한 번 묶이면 해제할 수 없다(Windows 제약). evloop_del 은
 *     대기 중인 I/O 를 취소하고 슬롯만 비운다. 같은 fd 번호가 다른 핸들로
 *     재사용되기 전에 반드시 evloop_del 을 거쳐야 한다.
 */
#include "../event_loop.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

#define EVLOOP_MAX_WATCH 256
#define EVLOOP_BATCH_MAX 64

typedef struct {
    int        in_use;
    int        fd;
    HANDLE     h;
    OVERLAPPED ov;
    int        armed;     /* 0바이트 read(또는 connect)가 걸려 있는가 */
    uint32_t   interest;
    DWORD      arm_err;   /* 무장 자체가 실패했을 때의 코드(수동 통지분) */
    char       zero;      /* ReadFile 의 형식상 버퍼(0바이트라 실제 접근 없음) */
} watch_t;

struct event_loop {
    HANDLE  iocp;
    watch_t w[EVLOOP_MAX_WATCH];
};

/* fd → 워치 슬롯. 선형 탐색이지만 감시 대상이 수십 개 규모라 충분하다. */
static watch_t *find_watch(event_loop_t *el, int fd) {
    for (int i = 0; i < EVLOOP_MAX_WATCH; i++)
        if (el->w[i].in_use && el->w[i].fd == fd) return &el->w[i];
    return NULL;
}

/*
 * 0바이트 overlapped read 를 건다.
 * 성공적으로 걸렸거나 즉시 완료되면 0(어느 쪽이든 완료 패킷이 IOCP 에 들어온다).
 * 파이프가 이미 끊겼다면 완료 패킷이 생기지 않으므로 직접 하나 밀어 넣는다.
 */
static int arm_watch(event_loop_t *el, watch_t *w) {
    if (w->armed) return 0;
    memset(&w->ov, 0, sizeof w->ov);

    BOOL  ok;
    DWORD err;
    if (w->interest & EV_ACCEPT) {
        /* 리스너: 0바이트 read 가 아니라 연결 대기를 건다. */
        ok  = ConnectNamedPipe(w->h, &w->ov);
        err = ok ? ERROR_SUCCESS : GetLastError();
        /* CreateFile 이 우리보다 먼저 연결을 끝낸 경우 — 이미 수락된 상태다. */
        if (!ok && err == ERROR_PIPE_CONNECTED) err = ERROR_SUCCESS;
    } else {
        ok  = ReadFile(w->h, &w->zero, 0, NULL, &w->ov);
        err = ok ? ERROR_SUCCESS : GetLastError();
    }

    w->arm_err = ERROR_SUCCESS;
    if (err != ERROR_IO_PENDING) {
        /* 완료 포트를 거치지 않는 경우들 — 직접 완료 패킷을 밀어 넣는다.
         *   ERROR_SUCCESS + EV_ACCEPT : ConnectNamedPipe 가 즉시 끝남
         *   그 밖의 오류(ERROR_BROKEN_PIPE 등)
         * (EV_READ 의 동기 완료는 기본 설정상 완료 패킷이 생기므로 제외.) */
        if (err != ERROR_SUCCESS || (w->interest & EV_ACCEPT)) {
            w->arm_err = err;
            PostQueuedCompletionStatus(el->iocp, 0,
                                        (ULONG_PTR)(w - el->w), &w->ov);
        }
    }
    w->armed = 1;
    return 0;
}

event_loop_t *evloop_create(void) {
    event_loop_t *el = calloc(1, sizeof *el);
    if (!el) return NULL;
    el->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!el->iocp) { free(el); return NULL; }
    return el;
}

void evloop_destroy(event_loop_t *el) {
    if (!el) return;
    for (int i = 0; i < EVLOOP_MAX_WATCH; i++)
        if (el->w[i].in_use && el->w[i].armed)
            CancelIoEx(el->w[i].h, &el->w[i].ov);
    if (el->iocp) CloseHandle(el->iocp);
    free(el);
}

int evloop_add(event_loop_t *el, int fd, uint32_t interest) {
    if (!el || fd < 0) { errno = EINVAL; return -1; }
    if (find_watch(el, fd)) { errno = EEXIST; return -1; }

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    int idx = -1;
    for (int i = 0; i < EVLOOP_MAX_WATCH; i++)
        if (!el->w[i].in_use) { idx = i; break; }
    if (idx < 0) { errno = ENOMEM; return -1; }

    watch_t *w = &el->w[idx];
    memset(w, 0, sizeof *w);
    w->in_use   = 1;
    w->fd       = fd;
    w->h        = h;
    w->interest = interest;

    /* 완료 키로 슬롯 인덱스를 쓴다 — 완료 패킷에서 워치를 O(1) 로 되찾는다. */
    if (!CreateIoCompletionPort(h, el->iocp, (ULONG_PTR)idx, 0)) {
        /* 이미 다른 포트에 묶였거나 overlapped 로 열리지 않은 핸들. */
        w->in_use = 0;
        errno = EINVAL;
        return -1;
    }
    return arm_watch(el, w);
}

int evloop_del(event_loop_t *el, int fd) {
    if (!el) { errno = EINVAL; return -1; }
    watch_t *w = find_watch(el, fd);
    if (!w) { errno = ENOENT; return -1; }
    if (w->armed) CancelIoEx(w->h, &w->ov);
    w->in_use = 0;
    w->armed  = 0;
    return 0;
}

int evloop_mod(event_loop_t *el, int fd, uint32_t interest) {
    if (!el) { errno = EINVAL; return -1; }
    watch_t *w = find_watch(el, fd);
    if (!w) { errno = ENOENT; return -1; }

    /* 무장 방식(ConnectNamedPipe ↔ 0바이트 read)이 달라지므로 걸린 I/O 를
     * 취소하고 다시 건다. 핸들↔IOCP 결합은 그대로 두면 된다(해제 불가이기도 하다). */
    if (w->armed) {
        CancelIoEx(w->h, &w->ov);
        w->armed = 0;
    }
    w->interest = interest;
    return arm_watch(el, w);
}

int evloop_wait(event_loop_t *el, ev_ready_t *out, int max, int timeout_ms) {
    if (!el || !out || max <= 0) { errno = EINVAL; return -1; }
    if (max > EVLOOP_BATCH_MAX) max = EVLOOP_BATCH_MAX;

    /* 지난 회차에 완료된 워치를 다시 무장한다. */
    for (int i = 0; i < EVLOOP_MAX_WATCH; i++)
        if (el->w[i].in_use && !el->w[i].armed)
            arm_watch(el, &el->w[i]);

    OVERLAPPED_ENTRY entries[EVLOOP_BATCH_MAX];
    ULONG got = 0;
    BOOL ok = GetQueuedCompletionStatusEx(
        el->iocp, entries, (ULONG)max, &got,
        timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, FALSE);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == WAIT_TIMEOUT) return 0;
        errno = EIO;
        return -1;
    }

    int n = 0;
    for (ULONG i = 0; i < got; i++) {
        ULONG_PTR key = entries[i].lpCompletionKey;
        if (key >= EVLOOP_MAX_WATCH) continue;
        watch_t *w = &el->w[key];
        /* evloop_del 직후 도착한 늦은 완료는 버린다. */
        if (!w->in_use) continue;

        w->armed = 0;

        uint32_t revents = EV_READ;
        DWORD    err     = w->arm_err;   /* 수동 통지분은 무장 실패 코드를 쓴다 */
        if (err == ERROR_SUCCESS) {
            DWORD bytes = 0;
            if (!GetOverlappedResult(w->h, &w->ov, &bytes, FALSE))
                err = GetLastError();
        }
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
            err == ERROR_HANDLE_EOF)
            revents = EV_HANGUP;
        else if (err != ERROR_SUCCESS && err != ERROR_IO_INCOMPLETE)
            revents = EV_ERROR;

        out[n].fd      = w->fd;
        out[n].revents = revents;
        n++;
    }
    return n;
}
