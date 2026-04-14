#define _GNU_SOURCE

#include "../ipc.h"
#include "../../common/ipc_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ─── 소켓 경로 ──────────────────────────────────────────────────────────── */

int ipc_socket_path(char *buf, size_t buflen) {
    uid_t uid = getuid();
    int n = snprintf(buf, buflen, IPC_SOCKET_PATH_FMT, (unsigned)uid);
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

int ipc_accept_client(int listen_fd) {
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);

    int cfd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
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
