/*
 * tessera-bridge — stdin/stdout ↔ Unix socket 양방향 바이트 릴레이
 *
 * 원격 세션 동기화에 사용:
 *   ssh user@host tessera-bridge
 *
 * 로컬 tessera 클라이언트가 SSH를 통해 이 바이너리와 통신하면,
 * bridge가 로컬 daemon의 Unix 소켓으로 메시지를 중계한다.
 */

#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include "../platform/ipc.h"
#include "../common/ipc_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <poll.h>
#  include <unistd.h>
#endif

/* 소켓 쪽 완전 쓰기 (부분 쓰기 재시도 포함). */
static int sock_write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = ipc_write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (ipc_wait_writable(fd, 500) <= 0) return -1;
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/*
 * 데몬에 연결한다. 실패 시 -1.
 */
static int connect_daemon(void)
{
    char path[IPC_SOCKET_PATH_MAX];
    if (ipc_socket_path(path, sizeof path) < 0) {
        fprintf(stderr, "tessera-bridge: socket path error\n");
        return -1;
    }
    int sock = ipc_connect(path);
    if (sock < 0) perror("connect");
    return sock;
}

#ifdef _WIN32

/*
 * Windows: stdin(콘솔/파이프 핸들)과 데몬 파이프는 한 번에 기다릴 수 있는
 * 종류가 아니라 poll 로 묶을 수 없다. 방향마다 스레드를 하나씩 두는 쪽이
 * 이식 흉내를 내는 것보다 단순하고 정확하다.
 */
typedef struct { int sock; volatile LONG *done; } relay_ctx_t;

/* stdin → socket */
static DWORD WINAPI stdin_to_sock(LPVOID arg)
{
    relay_ctx_t *ctx = arg;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    uint8_t buf[65536];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(in, buf, sizeof buf, &n, NULL) || n == 0) break;
        if (sock_write_all(ctx->sock, buf, n) < 0) break;
    }
    InterlockedExchange(ctx->done, 1);
    return 0;
}

int main(void)
{
    int sock = connect_daemon();
    if (sock < 0) return 1;

    volatile LONG done = 0;
    relay_ctx_t ctx = { sock, &done };
    HANDLE th = CreateThread(NULL, 0, stdin_to_sock, &ctx, 0, NULL);
    if (!th) { ipc_close_conn(sock); return 1; }

    /* socket → stdout (메인 스레드) */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    uint8_t buf[65536];
    while (!done) {
        int r = ipc_wait_readable(sock, 100);
        if (r < 0) break;
        if (r == 0) continue;               /* 타임아웃 — done 을 다시 확인 */
        ssize_t n = ipc_read(sock, buf, sizeof buf);
        if (n == 0) break;                  /* 데몬 종료 */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        DWORD wrote = 0;
        if (!WriteFile(out, buf, (DWORD)n, &wrote, NULL)) break;
    }

    ipc_close_conn(sock);
    /* stdin 스레드는 ReadFile 에서 막혀 있을 수 있다. 프로세스가 곧 끝나므로
     * 잠깐만 기다리고 정리는 OS 에 맡긴다. */
    WaitForSingleObject(th, 200);
    CloseHandle(th);
    return 0;
}

#else /* !_WIN32 */

int main(void)
{
    int sock = connect_daemon();
    if (sock < 0) return 1;

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
            if (sock_write_all(sock, buf, (size_t)n) < 0) break;
        }

        /* socket → stdout */
        if (fds[1].revents & POLLIN) {
            ssize_t n = ipc_read(sock, buf, sizeof buf);
            if (n <= 0) break;
            if (write(STDOUT_FILENO, buf, (size_t)n) < 0) break;
        }

        /* 에러/종료 */
        if ((fds[0].revents | fds[1].revents) & (POLLHUP | POLLERR))
            break;
    }

    ipc_close_conn(sock);
    return 0;
}

#endif /* !_WIN32 */
