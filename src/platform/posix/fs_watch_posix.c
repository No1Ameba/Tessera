/*
 * fs_watch_posix.c — inotify 기반 파일 변경 감시 (설정 핫 리로드용)
 */

#include "../fs_watch.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>

#define MAX_WATCHES 8

typedef struct {
    int inotify_fd;
    int wds[MAX_WATCHES];
    int wd_count;
} fs_watch_ctx_t;

void *fs_watch_create(void)
{
    fs_watch_ctx_t *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;

    ctx->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ctx->inotify_fd < 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

int fs_watch_add(void *handle, const char *path)
{
    fs_watch_ctx_t *ctx = handle;
    if (!ctx || !path || ctx->wd_count >= MAX_WATCHES) return -1;

    int wd = inotify_add_watch(ctx->inotify_fd, path,
                                IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) return -1;

    ctx->wds[ctx->wd_count++] = wd;
    return 0;
}

int fs_watch_poll(void *handle)
{
    fs_watch_ctx_t *ctx = handle;
    if (!ctx) return -1;

    struct pollfd pfd = { ctx->inotify_fd, POLLIN, 0 };
    int ret = poll(&pfd, 1, 0); /* 논블로킹 */
    if (ret <= 0) return 0;

    /* 이벤트 소비 (버퍼 비우기) */
    char buf[4096];
    while (read(ctx->inotify_fd, buf, sizeof buf) > 0)
        ; /* drain */

    return 1;
}

void fs_watch_destroy(void *handle)
{
    fs_watch_ctx_t *ctx = handle;
    if (!ctx) return;

    for (int i = 0; i < ctx->wd_count; i++)
        inotify_rm_watch(ctx->inotify_fd, ctx->wds[i]);

    if (ctx->inotify_fd >= 0) close(ctx->inotify_fd);
    free(ctx);
}
