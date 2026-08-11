#ifndef TESSERA_TEST_COMPAT_H
#define TESSERA_TEST_COMPAT_H

/*
 * 테스트 전용 플랫폼 shim — 스레드와 sleep 만 감싼다.
 *
 * 제품 코드는 스레드를 쓰지 않으므로(데몬/클라이언트 모두 단일 이벤트 루프)
 * 공용 플랫폼 레이어에 넣지 않고 테스트에만 둔다.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>       /* _getpid */

#define getpid _getpid

typedef HANDLE test_thread_t;
#define test_sleep_ms(ms) Sleep(ms)

/* 스레드 본문의 시그니처를 POSIX 쪽과 맞추기 위한 어댑터. */
typedef void *(*test_thread_fn)(void *);

typedef struct { test_thread_fn fn; void *arg; } test_thread_start_t;

static DWORD WINAPI test_thread_trampoline(LPVOID p) {
    test_thread_start_t *s = (test_thread_start_t *)p;
    s->fn(s->arg);
    return 0;
}

/* 시작 인자는 스레드가 끝날 때까지 살아 있어야 하므로 정적 슬롯을 쓴다.
 * 테스트는 한 번에 서버 스레드 하나만 띄운다. */
static test_thread_start_t g_test_thread_slot;

static int test_thread_create(test_thread_t *t, test_thread_fn fn, void *arg) {
    g_test_thread_slot.fn  = fn;
    g_test_thread_slot.arg = arg;
    *t = CreateThread(NULL, 0, test_thread_trampoline, &g_test_thread_slot, 0, NULL);
    return *t ? 0 : -1;
}

static int test_thread_join(test_thread_t t) {
    if (!t) return -1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

#else /* !_WIN32 */

#include <pthread.h>
#include <unistd.h>

typedef pthread_t test_thread_t;
#define test_sleep_ms(ms) usleep((ms) * 1000)

typedef void *(*test_thread_fn)(void *);

static int test_thread_create(test_thread_t *t, test_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static int test_thread_join(test_thread_t t) {
    return pthread_join(t, NULL);
}

#endif /* !_WIN32 */

#endif /* TESSERA_TEST_COMPAT_H */
