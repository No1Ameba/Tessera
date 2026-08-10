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
