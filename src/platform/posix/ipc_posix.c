#define _GNU_SOURCE

#include "../ipc.h"
#include "../../common/ipc_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ─── 소켓 경로 ──────────────────────────────────────────────────────────── */

int ipc_socket_path(char *buf, size_t buflen) {
    /* $XDG_RUNTIME_DIR (per-user, mode 0700, tmpfs) 는 런타임 소켓의 올바른
     * 위치다. world-writable 인 /tmp 는 소켓 파일에 대한 symlink/TOCTOU 경쟁에
     * 노출되므로 XDG_RUNTIME_DIR 를 우선한다. 변수가 없을 때만 /tmp 로 폴백한다
     * (데몬과 클라이언트는 같은 env 를 상속하므로 경로가 일치한다). */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int n;
    if (runtime && runtime[0] == '/') {
        size_t rl = strlen(runtime);
        while (rl > 1 && runtime[rl - 1] == '/') rl--;  /* 후행 '/' 제거 */
        n = snprintf(buf, buflen, "%.*s/tessera-%u.sock",
                     (int)rl, runtime, (unsigned)getuid());
    } else {
        n = snprintf(buf, buflen, IPC_SOCKET_PATH_FMT, (unsigned)getuid());
    }
    if (n < 0 || (size_t)n >= buflen) return -1;
    return 0;
}

/* ─── 서버 소켓 ──────────────────────────────────────────────────────────── */

int ipc_listen_socket(const char *path) {
    if (!path) { errno = EINVAL; return -1; }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* 기존 소켓 파일 제거 (이전 비정상 종료 잔재) */
    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* 소켓 파일 권한: 현재 사용자만 읽기/쓰기 */
    chmod(path, 0600);

    if (listen(fd, 8) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    /* listen_fd 도 논블로킹으로 설정 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

int ipc_accept_client(int *listen_fd) {
    if (!listen_fd) { errno = EINVAL; return -1; }

    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);

    /* POSIX: listen fd 는 그대로 두고 새 fd 만 돌려준다. */
    int cfd = accept(*listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (cfd < 0) return -1;

    int flags = fcntl(cfd, F_GETFL, 0);
    if (flags < 0 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(cfd);
        return -1;
    }

    return cfd;
}

void ipc_close_socket(int fd, const char *path) {
    if (fd >= 0)
        close(fd);
    if (path)
        unlink(path);
}

/* ─── 연결 I/O ──────────────────────────────────────────────────────────── */

/* POSIX 에서는 fd 가 이미 O_NONBLOCK 이므로 그대로 얇게 감싸기만 한다. */

ssize_t ipc_read(int fd, void *buf, size_t len) {
    return read(fd, buf, len);
}

ssize_t ipc_write(int fd, const void *buf, size_t len) {
    return write(fd, buf, len);
}

int ipc_wait_writable(int fd, int timeout_ms) {
    struct pollfd pf = { fd, POLLOUT, 0 };
    int r = poll(&pf, 1, timeout_ms);
    if (r < 0) return -1;
    return r == 0 ? 0 : 1;
}

void ipc_close_conn(int fd) {
    if (fd >= 0) close(fd);
}
