/* GNU/POSIX/BSD 확장 활성화: usleep */
#define _GNU_SOURCE

/*
 * test_pty.c — PTY 추상화 레이어 테스트
 *
 * 단독 빌드:
 *   gcc -std=c11 -Wall -I src/platform \
 *       src/platform/posix/pty_posix.c tests/test_pty.c \
 *       -lutil -o /tmp/test_pty && /tmp/test_pty
 */

#include "termemu_pty.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── 미니 테스트 프레임워크 ─────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", msg); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s  (line %d)\n", msg, __LINE__); \
            g_fail++; \
        } \
    } while (0)

#define GROUP(name) printf("\n[%s]\n", name)

/* ─── 헬퍼 ──────────────────────────────────────────────────────────────── */

/*
 * PTY 마스터 fd 에서 최대 timeout_ms 동안 데이터를 읽어 buf 에 쌓는다.
 * 반환값: 읽은 총 바이트 수.
 */
static ssize_t pty_read_timeout(pty_t *pty, char *buf, size_t buflen,
                                int timeout_ms) {
    size_t total = 0;
    struct pollfd pfd = { pty->master_fd, POLLIN, 0 };

    while (total < buflen - 1) {
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) break;                /* 타임아웃 또는 오류 */
        if (!(pfd.revents & POLLIN)) break;

        ssize_t n = pty_read(pty, buf + total, buflen - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        timeout_ms = 50;  /* 첫 데이터 도착 후 짧게 대기 */
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

/* ─── 테스트 케이스 ──────────────────────────────────────────────────────── */

static void test_spawn_close(void) {
    GROUP("pty_spawn / pty_close");

    pty_t pty;
    int ret = pty_spawn(&pty, "/bin/sh", 80, 24);
    ASSERT(ret == 0,          "spawn 성공");
    ASSERT(pty.master_fd > 0, "master_fd 유효");
    ASSERT(pty.child_pid > 0, "child_pid 유효");

    int status;
    pty_close(&pty, &status);
    ASSERT(pty.master_fd == -1, "close 후 master_fd = -1");
    ASSERT(pty.child_pid == -1, "close 후 child_pid = -1");
}

static void test_spawn_default_shell(void) {
    GROUP("pty_spawn: shell=NULL → $SHELL 또는 /bin/sh");

    pty_t pty;
    int ret = pty_spawn(&pty, NULL, 80, 24);
    ASSERT(ret == 0,          "NULL shell 스폰 성공");
    ASSERT(pty.child_pid > 0, "child_pid 유효");
    pty_close(&pty, NULL);
    ASSERT(pty.master_fd == -1, "close 후 master_fd = -1");
}

static void test_write_read(void) {
    GROUP("pty_write / pty_read");

    pty_t pty;
    int ret = pty_spawn(&pty, "/bin/sh", 80, 24);
    ASSERT(ret == 0, "spawn 성공");

    if (ret != 0) return;

    /* 셸 초기화 대기 (프롬프트 출력) */
    char buf[4096];
    pty_read_timeout(&pty, buf, sizeof(buf), 300);

    /* echo 명령으로 출력 유도 */
    const char *cmd = "echo TERMEMU_TEST_OK\n";
    ssize_t written = pty_write(&pty, cmd, strlen(cmd));
    ASSERT(written > 0, "write 성공");

    /* 출력 읽기 */
    ssize_t n = pty_read_timeout(&pty, buf, sizeof(buf), 500);
    ASSERT(n > 0, "read: 데이터 수신");
    ASSERT(strstr(buf, "TERMEMU_TEST_OK") != NULL, "read: echo 출력 확인");

    pty_close(&pty, NULL);
}

static void test_resize(void) {
    GROUP("pty_resize");

    pty_t pty;
    int ret = pty_spawn(&pty, "/bin/sh", 80, 24);
    ASSERT(ret == 0, "spawn 성공");

    if (ret != 0) return;

    ret = pty_resize(&pty, 120, 40);
    ASSERT(ret == 0, "resize(120×40) 성공");

    ret = pty_resize(&pty, 40, 10);
    ASSERT(ret == 0, "resize(40×10) 성공");

    pty_close(&pty, NULL);
}

static void test_nonblocking(void) {
    GROUP("논블로킹 read: 데이터 없으면 0 반환");

    pty_t pty;
    int ret = pty_spawn(&pty, "/bin/sh", 80, 24);
    ASSERT(ret == 0, "spawn 성공");

    if (ret != 0) return;

    /* 셸 초기화 flush */
    char buf[4096];
    pty_read_timeout(&pty, buf, sizeof(buf), 300);

    /* 아무 명령도 보내지 않고 즉시 read → EAGAIN → 0 */
    usleep(50 * 1000);  /* 50ms */
    ssize_t n = pty_read(&pty, buf, sizeof(buf));
    ASSERT(n == 0, "EAGAIN → 0 반환");

    pty_close(&pty, NULL);
}

static void test_double_close(void) {
    GROUP("pty_close 이중 호출 안전성");

    pty_t pty;
    int ret = pty_spawn(&pty, "/bin/sh", 80, 24);
    ASSERT(ret == 0, "spawn 성공");

    pty_close(&pty, NULL);
    pty_close(&pty, NULL);  /* 이중 호출 — 크래시 없어야 함 */
    ASSERT(1, "이중 close 크래시 없음");
}

static void test_null_safety(void) {
    GROUP("NULL 안전성");

    ASSERT(pty_spawn(NULL, "/bin/sh", 80, 24) == -1, "spawn(NULL) → -1");

    pty_t pty = { .master_fd = -1, .child_pid = -1 };
    ASSERT(pty_read(&pty, NULL, 0) == -1,       "read(bad fd) → -1");
    ASSERT(pty_write(&pty, NULL, 0) == -1,      "write(bad fd) → -1");
    ASSERT(pty_resize(&pty, 80, 24) == -1,      "resize(bad fd) → -1");

    pty_close(NULL, NULL);   /* 크래시 없어야 함 */
    ASSERT(1, "close(NULL) 크래시 없음");
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== PTY 추상화 테스트 ===\n");

    test_spawn_close();
    test_spawn_default_shell();
    test_write_read();
    test_resize();
    test_nonblocking();
    test_double_close();
    test_null_safety();

    printf("\n결과: %d 통과 / %d 실패\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
