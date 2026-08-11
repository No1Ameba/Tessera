#include "mono_time.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int64_t tessera_mono_ms(void)
{
    /* 주파수는 부팅 시 고정되므로 한 번만 읽는다. */
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);

    LARGE_INTEGER ctr;
    QueryPerformanceCounter(&ctr);

    /* (ctr * 1000) / freq 를 그대로 쓰면 긴 uptime 에서 int64 가 넘칠 수 있어
     * 몫과 나머지를 나눠 계산한다. */
    int64_t sec = ctr.QuadPart / freq.QuadPart;
    int64_t rem = ctr.QuadPart % freq.QuadPart;
    return sec * 1000 + (rem * 1000) / freq.QuadPart;
}

#else

#include <time.h>

int64_t tessera_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif
