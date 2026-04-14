/*
 * termemu-bridge — stdin/stdout ↔ Unix socket 양방향 바이트 릴레이
 *
 * 원격 세션 동기화에 사용:
 *   ssh user@host termemu-bridge
 *
 * 로컬 termemu 클라이언트가 SSH를 통해 이 바이너리와 통신하면,
 * bridge가 로컬 daemon의 Unix 소켓으로 메시지를 중계한다.
 */

#define _GNU_SOURCE

#include "../platform/ipc.h"
#include "../common/ipc_proto.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int main(void)
{
    /* daemon Unix 소켓에 연결 */
    char path[IPC_SOCKET_PATH_MAX];
    if (ipc_socket_path(path, sizeof path) < 0) {
        fprintf(stderr, "termemu-bridge: socket path error\n");
        return 1;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    /* stdin ↔ socket 양방향 릴레이 */
    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = sock,         .events = POLLIN },
    };

    uint8_t buf[65536];
    for (;;) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* stdin → socket */
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) break;
            if (write_all(sock, buf, (size_t)n) < 0) break;
        }

        /* socket → stdout */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(sock, buf, sizeof buf);
            if (n <= 0) break;
            if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0) break;
        }

        /* 에러/종료 */
        if ((fds[0].revents | fds[1].revents) & (POLLHUP | POLLERR))
            break;
    }

    close(sock);
    return 0;
}
