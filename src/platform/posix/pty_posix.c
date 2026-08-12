/* GNU/POSIX/BSD 확장 활성화: forkpty, setenv, nanosleep, kill */
#define _GNU_SOURCE

#include "../tessera_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* forkpty(3) — POSIX.1-2001 */
#if defined(__linux__)
#  include <pty.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#  include <util.h>
#else
#  include <pty.h>
#endif

/* ─── 내부 헬퍼 ──────────────────────────────────────────────────────────── */

static void build_winsize(struct winsize *ws, uint16_t cols, uint16_t rows) {
    memset(ws, 0, sizeof(*ws));
    ws->ws_col = cols;
    ws->ws_row = rows;
}

/* ─── 공개 API ───────────────────────────────────────────────────────────── */

int pty_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int pty_spawn(pty_t *out, const char *shell, uint16_t cols, uint16_t rows) {
    if (!out) { errno = EINVAL; return -1; }

    struct winsize ws;
    build_winsize(&ws, cols, rows);

    /* shell 결정: 인자 → $SHELL → /bin/sh */
    if (!shell || shell[0] == '\0') {
        shell = getenv("SHELL");
        if (!shell || shell[0] == '\0')
            shell = "/bin/sh";
    }

    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);

    if (pid < 0) {
        /* fork 실패 */
        return -1;
    }

    if (pid == 0) {
        /* ── 자식 프로세스 ── */
        /* 환경 변수 설정 */
        setenv("TERM", "xterm-256color", 1);

        /* 셸 실행 — execvp 는 실패 시 돌아오지 않음 */
        char *argv[] = { (char *)shell, NULL };
        execvp(shell, argv);

        /* execvp 실패 시 즉시 종료 */
        _exit(127);
    }

    /* ── 부모 프로세스 ── */
    out->master_fd  = master_fd;
    out->child_pid  = pid;

    /* 마스터 fd 를 논블로킹으로 설정 */
    if (pty_set_nonblocking(master_fd) == -1) {
        int saved = errno;
        pty_close(out, NULL);
        errno = saved;
        return -1;
    }

    return 0;
}

ssize_t pty_read(pty_t *pty, void *buf, size_t len) {
    if (!pty || pty->master_fd < 0) { errno = EBADF; return -1; }
    ssize_t n = read(pty->master_fd, buf, len);
    if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;  /* 데이터 없음, 논블로킹 */
    return n;
}

ssize_t pty_write(pty_t *pty, const void *buf, size_t len) {
    if (!pty || pty->master_fd < 0) { errno = EBADF; return -1; }
    return write(pty->master_fd, buf, len);
}

int pty_child_alive(const pty_t *pty) {
    if (!pty || pty->child_pid <= 0) return 0;
    /* 데몬은 SIGCHLD 를 SIG_IGN + SA_NOCLDWAIT 로 두어 자식을 자동 수거하므로
     * 좀비가 남지 않는다 → 죽은 pid 는 kill(0) 이 ESRCH 를 낸다. */
    return kill(pty->child_pid, 0) == 0 || errno == EPERM;
}

int pty_resize(pty_t *pty, uint16_t cols, uint16_t rows) {
    if (!pty || pty->master_fd < 0) { errno = EBADF; return -1; }
    struct winsize ws;
    build_winsize(&ws, cols, rows);
    return ioctl(pty->master_fd, TIOCSWINSZ, &ws);
}

void pty_close(pty_t *pty, int *exit_status) {
    if (!pty) return;

    if (pty->child_pid > 0) {
        /* SIGTERM 으로 먼저 요청 */
        kill(pty->child_pid, SIGTERM);

        /* 100ms 대기 후 상태 확인 */
        struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms */
        nanosleep(&ts, NULL);

        int status;
        pid_t result = waitpid(pty->child_pid, &status, WNOHANG);
        if (result == 0) {
            /* 아직 살아 있음 → SIGKILL */
            kill(pty->child_pid, SIGKILL);
            waitpid(pty->child_pid, &status, 0);
        } else {
            status = (result < 0) ? -1 : status;
        }

        if (exit_status) *exit_status = status;
        pty->child_pid = -1;
    } else {
        if (exit_status) *exit_status = -1;
    }

    if (pty->master_fd >= 0) {
        close(pty->master_fd);
        pty->master_fd = -1;
    }
}
