/*
 * ipc_win.c — Windows Named Pipe 기반 IPC 백엔드.
 *
 * ⚠️ 검증 상태 + 범위 한계: 이 파일은 Linux/WSL 에서 컴파일 검증되지 않았다(MSVC
 *    필요). 더 중요한 것은, 데몬(ipc_server.c)이 epoll + 논블로킹 fd 이벤트 루프에
 *    강하게 결합돼 있어 Windows 에서 다중 클라이언트를 실제로 서비스하려면 데몬을
 *    IOCP(또는 WaitForMultipleObjects) 로 포팅해야 한다는 점이다. 그 포팅 전까지
 *    아래 구현은 ipc.h 계약(listen/accept/close)에 Named Pipe 를 매핑한 최소
 *    골격이며, HANDLE↔CRT fd 브릿지에 _open_osfhandle 을 사용한다.
 *    accept 모델(단일 인스턴스 반환)은 데몬 포팅 시 재설계 대상이다.
 */
#include "../ipc.h"
#include "../../common/ipc_proto.h"

#include <windows.h>
#include <errno.h>
#include <io.h>       /* _open_osfhandle, _close */
#include <fcntl.h>    /* _O_RDWR */
#include <stdio.h>    /* snprintf */
#include <string.h>
#include <lmcons.h>   /* UNLEN */

int ipc_socket_path(char *buf, size_t buflen) {
    /* Windows: 파이프 이름 \\.\pipe\tessera-<username> */
    char user[UNLEN + 1] = "user";
    DWORD n = (DWORD)sizeof user;
    GetUserNameA(user, &n);
    int r = snprintf(buf, buflen, IPC_PIPE_NAME_FMT, user);
    if (r < 0 || (size_t)r >= buflen) return -1;
    return 0;
}

int ipc_listen_socket(const char *path) {
    if (!path) return -1;
    HANDLE h = CreateNamedPipeA(
        path,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        65536, 65536, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = _open_osfhandle((intptr_t)h, _O_RDWR);
    if (fd < 0) { CloseHandle(h); return -1; }
    return fd;
}

int ipc_accept_client(int listen_fd) {
    HANDLE h = (HANDLE)_get_osfhandle(listen_fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    /* 클라이언트 연결 대기(간이 blocking). 실 데몬에서는 overlapped +
     * IOCP 로 대체해야 한다. */
    BOOL ok = ConnectNamedPipe(h, NULL);
    if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) return -1;
    /* 이 인스턴스를 클라이언트 통신 fd 로 반환한다. 다음 연결용 새 인스턴스는
     * 호출자가 다시 ipc_listen_socket() 으로 생성해야 한다(간이 모델). */
    return listen_fd;
}

void ipc_close_socket(int fd, const char *path) {
    (void)path;  /* Named Pipe 는 파일 unlink 불필요 */
    if (fd >= 0) {
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (h != INVALID_HANDLE_VALUE)
            DisconnectNamedPipe(h);
        _close(fd);
    }
}

/* ─── 연결 I/O ──────────────────────────────────────────────────────────── */

/*
 * 파이프가 FILE_FLAG_OVERLAPPED 로 열려 있으므로 모든 I/O 에 OVERLAPPED 가
 * 필요하다. 완료 대기용 이벤트를 fd 별로 하나씩 캐시해 재사용한다.
 *
 * hEvent 의 최하위 비트를 세우면 이 I/O 의 완료 패킷이 IOCP 로 가지 않는다
 * (OVERLAPPED 문서화 규약). 데몬이 같은 핸들을 event_loop_win.c 의 IOCP 에
 * 물려 둔 상태에서 readiness 통지와 섞이는 것을 막는다.
 *
 * 전제: 한 fd 에 대한 I/O 는 한 번에 하나만 진행된다(데몬/클라이언트 모두
 * 단일 이벤트 루프에서 직렬로 호출).
 */
#define IPC_MAX_CONN 64

static struct { int fd; HANDLE ev; } g_ev[IPC_MAX_CONN];

static HANDLE conn_event(int fd) {
    int free_slot = -1;
    for (int i = 0; i < IPC_MAX_CONN; i++) {
        if (g_ev[i].ev && g_ev[i].fd == fd) return g_ev[i].ev;
        if (!g_ev[i].ev && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);   /* 수동 리셋 */
    if (!ev) return NULL;
    g_ev[free_slot].fd = fd;
    g_ev[free_slot].ev = ev;
    return ev;
}

static void drop_conn_event(int fd) {
    for (int i = 0; i < IPC_MAX_CONN; i++) {
        if (g_ev[i].ev && g_ev[i].fd == fd) {
            CloseHandle(g_ev[i].ev);
            g_ev[i].ev = NULL;
            g_ev[i].fd = 0;
            return;
        }
    }
}

/* overlapped I/O 를 걸고 완료를 기다린다. 반환: 전송 바이트, -1 오류. */
static ssize_t overlapped_io(int fd, void *buf, size_t len, int is_write,
                             int timeout_ms)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    HANDLE ev = conn_event(fd);
    if (!ev) { errno = ENOMEM; return -1; }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ResetEvent(ev);
    ov.hEvent = (HANDLE)((ULONG_PTR)ev | 1);

    BOOL ok = is_write
        ? WriteFile(h, buf, (DWORD)len, NULL, &ov)
        : ReadFile(h, buf, (DWORD)len, NULL, &ov);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
            return 0;                       /* 상대측 종료 → EOF */
        if (err != ERROR_IO_PENDING) { errno = EIO; return -1; }
    }

    if (WaitForSingleObject(ev, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms)
            != WAIT_OBJECT_0) {
        /* 시간 초과: 취소하고 그때까지 전송된 만큼을 돌려준다.
         * 호출자(write_all_retry)가 남은 분량을 이어서 보낸다. */
        CancelIoEx(h, &ov);
        DWORD done = 0;
        GetOverlappedResult(h, &ov, &done, TRUE);
        if (done == 0) { errno = EAGAIN; return -1; }
        return (ssize_t)done;
    }

    DWORD done = 0;
    if (!GetOverlappedResult(h, &ov, &done, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
            err == ERROR_HANDLE_EOF)
            return 0;
        errno = EIO;
        return -1;
    }
    return (ssize_t)done;
}

ssize_t ipc_read(int fd, void *buf, size_t len) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    /* 논블로킹 시맨틱: 대기 중인 바이트가 없으면 EAGAIN. */
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
            return 0;                       /* EOF */
        errno = EIO;
        return -1;
    }
    if (avail == 0) { errno = EAGAIN; return -1; }
    if ((size_t)avail < len) len = avail;

    return overlapped_io(fd, buf, len, /*is_write=*/0, /*timeout_ms=*/-1);
}

ssize_t ipc_write(int fd, const void *buf, size_t len) {
    /* 파이프 버퍼가 가득 차면 상대가 읽어갈 때까지 최대 500ms 기다린다
     * (POSIX 쪽 write_all_retry 의 poll 타임아웃과 같은 값). */
    return overlapped_io(fd, (void *)buf, len, /*is_write=*/1, 500);
}

int ipc_wait_writable(int fd, int timeout_ms) {
    (void)fd; (void)timeout_ms;
    /* 실제 대기는 ipc_write 내부의 overlapped 완료 대기에서 이뤄진다. */
    return 1;
}

void ipc_close_conn(int fd) {
    if (fd < 0) return;
    drop_conn_event(fd);
    _close(fd);
}
