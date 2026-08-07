#define _GNU_SOURCE
#include "client_util.h"

#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#endif

long now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void open_url(const char *url)
{
    if (!url || !*url) return;
#ifndef _WIN32
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
            close(devnull);
        }
#ifdef __APPLE__
        execlp("open", "open", url, (char*)NULL);
#else
        execlp("xdg-open", "xdg-open", url, (char*)NULL);
#endif
        _exit(127);
    }
    /* SIGCHLD는 부모의 기존 핸들러가 reap (없으면 좀비; 짧은 수명이므로 OK) */
#endif
}
