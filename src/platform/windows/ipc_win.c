/*
 * ipc_win.c — Windows Named Pipe 기반 IPC 백엔드.
 *
 * 핸들은 _open_osfhandle 로 CRT fd 에 실어 ipc.h 의 fd 기반 계약에 맞춘다.
 * 파이프는 FILE_FLAG_OVERLAPPED 로 열리므로(event_loop_win.c 의 IOCP 에 등록해야
 * 한다) 모든 I/O 에 OVERLAPPED 가 필요하다 — CRT 의 _read/_write 를 쓰면 안 되고
 * 호출자는 ipc_read/ipc_write 를 거쳐야 한다.
 *
 * accept 는 POSIX 와 모델이 다르다. 대기하던 인스턴스가 곧 연결이 되므로
 * ipc_accept_client 가 그것을 넘겨주고 다음 연결용 인스턴스를 새로 만든다
 * (ipc.h 의 in/out listen_fd 설명 참고).
 *
 * ⚠️ 알려진 문제: PTY 출력이 많을 때(cmd.exe) 데몬의 클라이언트 처리가 간헐적으로
 *    어긋난다 — tests/test_ipc 가 Windows 에서 비결정적으로 실패하고 드물게 멈춘다.
 *    데몬이 이벤트 루프 안에서 클라이언트로 직접 쓰기 때문에, 파이프 버퍼가 차면
 *    최대 500ms 씩 루프가 멈추는 구조가 원인으로 보인다. 클라이언트별 출력 큐를
 *    두고 쓰기 가능할 때만 흘려보내는 방식이 근본 해법이다.
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
    /* 명시적 오버라이드가 있으면 그대로 쓴다(ipc.h 참고). */
    const char *override = getenv("TESSERA_IPC_PATH");
    if (override && override[0]) {
        int n = snprintf(buf, buflen, "%s", override);
        return (n > 0 && (size_t)n < buflen) ? 0 : -1;
    }

    /* Windows: 파이프 이름 \\.\pipe\tessera-<username> */
    char user[UNLEN + 1] = "user";
    DWORD n = (DWORD)sizeof user;
    GetUserNameA(user, &n);
    int r = snprintf(buf, buflen, IPC_PIPE_NAME_FMT, user);
    if (r < 0 || (size_t)r >= buflen) return -1;
    return 0;
}

/* 다음 인스턴스를 만들려면 파이프 이름이 필요하다. 데몬의 리스너는 하나뿐이다. */
static char g_pipe_name[256];

/* 이름 있는 파이프 인스턴스를 하나 만들어 fd 로 감싼다. */
static int make_instance(const char *path) {
    HANDLE h = CreateNamedPipeA(
        path,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        65536, 65536, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = EIO; return -1; }
    int fd = _open_osfhandle((intptr_t)h, _O_RDWR);
    if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
    return fd;
}

int ipc_listen_socket(const char *path) {
    if (!path) { errno = EINVAL; return -1; }
    int r = snprintf(g_pipe_name, sizeof g_pipe_name, "%s", path);
    if (r < 0 || (size_t)r >= sizeof g_pipe_name) { errno = ENAMETOOLONG; return -1; }
    return make_instance(g_pipe_name);
}

/*
 * Named Pipe 는 대기하던 인스턴스가 곧 연결이다. 연결 자체는 이벤트 루프가
 * EV_ACCEPT(overlapped ConnectNamedPipe)로 이미 완료시켜 두었으므로, 여기서는
 * 소유권만 넘기고 다음 연결용 인스턴스를 새로 만든다.
 */
int ipc_accept_client(int *listen_fd) {
    if (!listen_fd || *listen_fd < 0) { errno = EINVAL; return -1; }

    int accepted = *listen_fd;
    int next = make_instance(g_pipe_name);
    if (next < 0) return -1;   /* 리스너를 유지한 채 실패 — 이번 연결만 포기 */

    *listen_fd = next;
    return accepted;
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

/* ─── 클라이언트 연결 ───────────────────────────────────────────────────── */

int ipc_connect(const char *path) {
    if (!path) { errno = EINVAL; return -1; }

    /* 서버 인스턴스가 overlapped 이므로 클라이언트도 맞춰서 연다
     * (ipc_read/ipc_write 가 OVERLAPPED 로 동작한다). */
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = (GetLastError() == ERROR_FILE_NOT_FOUND) ? ENOENT : EIO;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_RDWR);
    if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
    return fd;
}

int ipc_wait_ready(const char *path, int timeout_ms) {
    if (!path) { errno = EINVAL; return -1; }
    /* 파이프 인스턴스가 생길 때까지 기다린다. WaitNamedPipe 는 파이프가 아직
     * 아예 없으면 즉시 실패하므로, 존재 여부를 폴링하며 재시도한다. */
    for (int waited = 0; timeout_ms < 0 || waited < timeout_ms; waited += 50) {
        if (WaitNamedPipeA(path, 50)) return 1;
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return 1;  /* 있으나 바쁨 */
        Sleep(50);
    }
    return 0;
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

/*
 * 파이프에 지금 당장 쓸 수 있는 바이트 수.
 *
 * POSIX 의 논블로킹 write 는 "들어가는 만큼 쓰고 그 수를 돌려준다". Windows 에는
 * 대응하는 공개 API 가 없어서, 커널이 들고 있는 파이프 정보에서 남은 쓰기 할당량
 * (WriteQuotaAvailable)을 직접 읽는다. 이 값만큼만 쓰면 WriteFile 이 대기 없이
 * 끝나므로 POSIX 와 같은 의미가 된다.
 *
 * ntdll 의 NtQueryInformationFile 은 헤더에 선언이 없어 런타임에 주소를 얻는다.
 * 조회에 실패하면 -1 을 돌려주고, 호출자는 기존 방식(타임아웃 대기)으로 물러난다.
 */
typedef struct {
    ULONG NamedPipeType, NamedPipeConfiguration, MaximumInstances;
    ULONG CurrentInstances, InboundQuota, ReadDataAvailable;
    ULONG OutboundQuota, WriteQuotaAvailable, NamedPipeState, NamedPipeEnd;
} tessera_pipe_local_info_t;

#define TESSERA_FilePipeLocalInformation 24

static LONG pipe_write_space(HANDLE h)
{
    typedef LONG (WINAPI *nt_query_fn)(HANDLE, PVOID, PVOID, ULONG, ULONG);
    static nt_query_fn query;
    static int resolved;

    if (!resolved) {
        resolved = 1;
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) query = (nt_query_fn)(void (*)(void))
                            GetProcAddress(nt, "NtQueryInformationFile");
    }
    if (!query) return -1;

    /* IO_STATUS_BLOCK 은 포인터 2개 크기면 충분하다(선언을 끌어오지 않는다). */
    ULONG_PTR iosb[2] = {0};
    tessera_pipe_local_info_t info;
    memset(&info, 0, sizeof info);
    if (query(h, iosb, &info, (ULONG)sizeof info,
              TESSERA_FilePipeLocalInformation) < 0)
        return -1;
    return (LONG)info.WriteQuotaAvailable;
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
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    /*
     * 논블로킹 시맨틱: 파이프에 들어갈 만큼만 쓰고 그 수를 돌려준다.
     * 이렇게 해야 ipc.h 계약(및 POSIX write)과 의미가 같아지고, 호출자의
     * "부분 쓰기 → 재시도" 루프가 그대로 성립한다.
     *
     * 이 조회 없이 통째로 쓰려 하면 파이프가 찼을 때 완료를 기다리게 되는데,
     * 데몬은 단일 스레드라 그 사이 클라이언트 요청을 못 읽어 교착한다
     * (클라이언트는 응답을 기다리느라 버퍼를 비우지 않는다).
     */
    LONG space = pipe_write_space(h);
    if (space == 0) { errno = EAGAIN; return -1; }
    if (space > 0 && (size_t)space < len) len = (size_t)space;

    /* 할당량 안으로 줄였으므로 대기 없이 끝난다. 조회가 실패했을 때(space<0)를
     * 대비해 타임아웃은 안전망으로 남겨 둔다. */
    return overlapped_io(fd, (void *)buf, len, /*is_write=*/1, 500);
}

int ipc_wait_writable(int fd, int timeout_ms) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    /* POSIX 의 poll(POLLOUT) 대응. 파이프에는 "쓰기 가능" 알림이 없어 남은
     * 할당량이 생길 때까지 폴링할 수밖에 없다(읽기와 달리 0바이트 overlapped
     * write 로는 기다릴 수 없다).
     *
     * 다만 반복 횟수로 시간을 세면 안 된다 — 기본 타이머 해상도에서 Sleep(1) 은
     * 15ms 가 넘으므로 500ms 타임아웃이 실제로는 7초가 된다. 실제 시계로 잰다.
     * 파이프가 완전히 찼을 때만 도달하는 경로라 폴링 주기는 성능에 영향이 없다.
     * 조회가 불가하면(space<0) 예전처럼 즉시 쓰기 가능으로 본다. */
    ULONGLONG deadline = GetTickCount64() + (timeout_ms < 0 ? 0 : (ULONGLONG)timeout_ms);
    for (;;) {
        LONG space = pipe_write_space(h);
        if (space != 0) return 1;   /* >0 여유 있음, <0 조회 불가 → 시도 */
        if (timeout_ms >= 0 && GetTickCount64() >= deadline) return 0;
        Sleep(1);
    }
}

int ipc_wait_readable(int fd, int timeout_ms) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    /* 이미 와 있으면 대기할 필요가 없다. */
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        DWORD perr = GetLastError();
        if (perr == ERROR_BROKEN_PIPE || perr == ERROR_PIPE_NOT_CONNECTED)
            return 1;          /* EOF 도 "읽을 수 있음" — 호출자가 0 을 받는다 */
        errno = EIO;
        return -1;
    }
    if (avail > 0) return 1;
    if (timeout_ms == 0) return 0;

    /*
     * 0바이트 overlapped read 로 기다린다 — 데이터가 도착하는 순간 완료되지만
     * 바이트는 소비하지 않는다(event_loop_win.c 와 같은 기법).
     *
     * 폴링(PeekNamedPipe + Sleep)으로 하면 안 된다. Windows 의 기본 타이머
     * 해상도에서 Sleep(1) 은 실제로 15ms 가 넘게 걸린다. 클라이언트는 매 프레임
     * ipc_client_poll(8ms) 을 부르므로, 1ms 씩 8번 자는 구현은 프레임당 100ms 를
     * 넘겨 체감 프레임률을 10fps 아래로 떨어뜨렸다.
     */
    HANDLE ev = conn_event(fd);
    if (!ev) { errno = ENOMEM; return -1; }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ResetEvent(ev);
    ov.hEvent = (HANDLE)((ULONG_PTR)ev | 1);   /* 완료 패킷을 IOCP 로 보내지 않는다 */

    char zero;
    if (!ReadFile(h, &zero, 0, NULL, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
            return 1;
        if (err != ERROR_IO_PENDING) { errno = EIO; return -1; }
    }

    DWORD w = WaitForSingleObject(ev, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (w != WAIT_OBJECT_0) {
        /* 시간 초과 — 0바이트 읽기라 취소해도 잃을 데이터가 없다. */
        CancelIoEx(h, &ov);
        DWORD done = 0;
        GetOverlappedResult(h, &ov, &done, TRUE);
        return 0;
    }

    DWORD done = 0;
    if (!GetOverlappedResult(h, &ov, &done, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
            err == ERROR_HANDLE_EOF)
            return 1;
        errno = EIO;
        return -1;
    }
    return 1;
}

void ipc_close_conn(int fd) {
    if (fd < 0) return;
    drop_conn_event(fd);
    _close(fd);
}
