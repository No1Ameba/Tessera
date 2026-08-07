/*
 * fs_watch_win.c — ReadDirectoryChangesW 기반 파일 변경 감시(설정 핫 리로드용).
 * fs_watch_posix.c(inotify) 와 동일한 계약을 구현한다.
 *
 * ⚠️ 검증 상태: Linux/WSL 개발 환경에서 컴파일 검증되지 않음. MSVC(Windows)
 *    빌드/CI 에서 검증 필요. 단, fs_watch 는 클라이언트(GLFW, 크로스플랫폼)에서만
 *    쓰이므로 데몬 epoll 블로커의 영향을 받지 않는다.
 *
 * 감시 파일마다 그 부모 디렉터리를 overlapped ReadDirectoryChangesW 로 감시하고,
 * poll 시 완료된 변경 알림의 파일명이 감시 대상 basename 과 일치하면 1 을 반환한다.
 */
#include "../fs_watch.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_WATCHES 8

typedef struct {
    HANDLE     dir;                 /* 감시 대상 파일의 부모 디렉터리 핸들 */
    OVERLAPPED ov;                  /* 비동기 ReadDirectoryChangesW 용 */
    wchar_t    fname[MAX_PATH];     /* 감시할 파일 basename (매칭용) */
    DWORD      buf[1024];           /* FILE_NOTIFY_INFORMATION 버퍼(4KB, DWORD 정렬) */
    BOOL       pending;             /* ReadDirectoryChangesW 발행됨 */
} watch_t;

typedef struct {
    watch_t w[MAX_WATCHES];
    int     count;
} fs_watch_ctx_t;

static BOOL issue_read(watch_t *w)
{
    ResetEvent(w->ov.hEvent);
    BOOL ok = ReadDirectoryChangesW(
        w->dir, w->buf, sizeof w->buf, FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_SIZE,
        NULL, &w->ov, NULL);
    w->pending = ok;
    return ok;
}

void *fs_watch_create(void)
{
    return calloc(1, sizeof(fs_watch_ctx_t));  /* 실패 시 NULL */
}

int fs_watch_add(void *handle, const char *path)
{
    fs_watch_ctx_t *ctx = handle;
    if (!ctx || !path || ctx->count >= MAX_WATCHES) return -1;

    /* UTF-8 → UTF-16 */
    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) == 0)
        return -1;

    /* 디렉터리와 basename 분리 (마지막 '\\' 또는 '/'). */
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, wpath, MAX_PATH);
    dir[MAX_PATH - 1] = L'\0';
    wchar_t *sep = wcsrchr(dir, L'\\');
    wchar_t *fwd = wcsrchr(dir, L'/');
    if (fwd && (!sep || fwd > sep)) sep = fwd;

    const wchar_t *base;
    if (sep) { *sep = L'\0'; base = wpath + (sep - dir) + 1; }
    else     { wcscpy(dir, L"."); base = wpath; }

    watch_t *w = &ctx->w[ctx->count];
    memset(w, 0, sizeof *w);
    wcsncpy(w->fname, base, MAX_PATH - 1);

    w->dir = CreateFileW(dir, FILE_LIST_DIRECTORY,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (w->dir == INVALID_HANDLE_VALUE) return -1;

    w->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);  /* manual-reset */
    if (!w->ov.hEvent) { CloseHandle(w->dir); return -1; }

    if (!issue_read(w)) {
        CloseHandle(w->ov.hEvent);
        CloseHandle(w->dir);
        return -1;
    }

    ctx->count++;
    return 0;
}

int fs_watch_poll(void *handle)
{
    fs_watch_ctx_t *ctx = handle;
    if (!ctx) return -1;

    int changed = 0;
    for (int i = 0; i < ctx->count; i++) {
        watch_t *w = &ctx->w[i];
        if (!w->pending) { issue_read(w); continue; }

        /* 완료 여부를 논블로킹으로 확인. */
        if (WaitForSingleObject(w->ov.hEvent, 0) != WAIT_OBJECT_0)
            continue;

        DWORD bytes = 0;
        if (GetOverlappedResult(w->dir, &w->ov, &bytes, FALSE) && bytes > 0) {
            BYTE *p = (BYTE *)w->buf;
            for (;;) {
                FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)p;
                size_t n = fni->FileNameLength / sizeof(wchar_t);
                if (n < MAX_PATH &&
                    _wcsnicmp(fni->FileName, w->fname, n) == 0 &&
                    w->fname[n] == L'\0')
                    changed = 1;
                if (fni->NextEntryOffset == 0) break;
                p += fni->NextEntryOffset;
            }
        }
        issue_read(w);  /* 재무장 */
    }
    return changed;
}

void fs_watch_destroy(void *handle)
{
    fs_watch_ctx_t *ctx = handle;
    if (!ctx) return;
    for (int i = 0; i < ctx->count; i++) {
        watch_t *w = &ctx->w[i];
        if (w->pending && w->dir != INVALID_HANDLE_VALUE) CancelIo(w->dir);
        if (w->ov.hEvent) CloseHandle(w->ov.hEvent);
        if (w->dir && w->dir != INVALID_HANDLE_VALUE) CloseHandle(w->dir);
    }
    free(ctx);
}
