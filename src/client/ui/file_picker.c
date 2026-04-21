#define _POSIX_C_SOURCE 200809L

#include "file_picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── 헬퍼 ─────────────────────────────────────────────────────────────────── */

/* 실행파일이 PATH 에 있는지 확인 (access 로 일반적 위치 스캔은 생략하고
 * `command -v` 를 popen 으로 돌려 판정). */
static int tool_exists(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

/* 문자열 안에 셸 메타문자가 있으면 ' 로 감싸고 내부 ' 는 이스케이프.
 * 결과는 buf 에 작성된 길이 반환 (널 포함 X). */
static size_t shell_quote(const char *s, char *buf, size_t cap)
{
    if (!s) { if (cap) buf[0] = '\0'; return 0; }
    size_t p = 0;
    if (p < cap) buf[p++] = '\'';
    for (const char *c = s; *c; c++) {
        if (*c == '\'') {
            /* ' 를 '\'' 로 치환 */
            if (p + 4 >= cap) break;
            buf[p++] = '\''; buf[p++] = '\\'; buf[p++] = '\''; buf[p++] = '\'';
        } else {
            if (p + 1 >= cap) break;
            buf[p++] = *c;
        }
    }
    if (p < cap) buf[p++] = '\'';
    if (p < cap) buf[p] = '\0';
    else if (cap) buf[cap - 1] = '\0';
    return p;
}

/* popen 으로 명령 실행 후 stdout 한 줄을 out_path 에 쓴다.
 * 반환: 1=성공, 0=사용자 취소(EOF), -1=실행 실패. */
static int run_picker_cmd(const char *cmd, char *out_path, size_t out_size)
{
    if (out_size == 0) return -1;
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    out_path[0] = '\0';
    if (!fgets(out_path, (int)out_size, fp)) {
        /* 사용자가 취소한 경우 zenity 는 종료 코드 1 + EOF */
        pclose(fp);
        return 0;
    }
    /* 개행 제거 */
    size_t n = strlen(out_path);
    while (n > 0 && (out_path[n-1] == '\n' || out_path[n-1] == '\r'))
        out_path[--n] = '\0';

    int rc = pclose(fp);
    if (rc == -1) return -1;
    return n > 0 ? 1 : 0;
}

/* ── zenity 구현 ──────────────────────────────────────────────────────────── */

static int try_zenity_open(char *out, size_t out_size,
                            const char *title, const char *filter)
{
    if (!tool_exists("zenity")) return -1;

    char cmd[1024];
    char qt[256] = {0}, qf[128] = {0};
    shell_quote(title ? title : "Open", qt, sizeof qt);
    if (filter) shell_quote(filter, qf, sizeof qf);

    if (filter)
        snprintf(cmd, sizeof cmd,
                  "zenity --file-selection --title=%s --file-filter=%s 2>/dev/null",
                  qt, qf);
    else
        snprintf(cmd, sizeof cmd,
                  "zenity --file-selection --title=%s 2>/dev/null", qt);

    return run_picker_cmd(cmd, out, out_size);
}

static int try_zenity_save(char *out, size_t out_size,
                            const char *title,
                            const char *default_name, const char *filter)
{
    if (!tool_exists("zenity")) return -1;

    char cmd[1536];
    char qt[256] = {0}, qn[256] = {0}, qf[128] = {0};
    shell_quote(title ? title : "Save", qt, sizeof qt);
    if (default_name) shell_quote(default_name, qn, sizeof qn);
    if (filter)       shell_quote(filter, qf, sizeof qf);

    /* --save --confirm-overwrite; --filename 은 기본 파일명 제안 */
    int pos = snprintf(cmd, sizeof cmd,
        "zenity --file-selection --save --confirm-overwrite --title=%s",
        qt);
    if (default_name && pos < (int)sizeof cmd)
        pos += snprintf(cmd + pos, sizeof cmd - pos, " --filename=%s", qn);
    if (filter && pos < (int)sizeof cmd)
        pos += snprintf(cmd + pos, sizeof cmd - pos, " --file-filter=%s", qf);
    if (pos < (int)sizeof cmd)
        snprintf(cmd + pos, sizeof cmd - pos, " 2>/dev/null");

    return run_picker_cmd(cmd, out, out_size);
}

/* ── kdialog 구현 ─────────────────────────────────────────────────────────── */

static int try_kdialog_open(char *out, size_t out_size,
                             const char *title, const char *filter)
{
    if (!tool_exists("kdialog")) return -1;

    char cmd[1024];
    char qt[256] = {0}, qf[128] = {0};
    shell_quote(title ? title : "Open", qt, sizeof qt);
    if (filter) shell_quote(filter, qf, sizeof qf);

    /* kdialog: --getopenfilename <startdir> <filter> --title <t> */
    if (filter)
        snprintf(cmd, sizeof cmd,
                  "kdialog --getopenfilename . %s --title %s 2>/dev/null",
                  qf, qt);
    else
        snprintf(cmd, sizeof cmd,
                  "kdialog --getopenfilename . --title %s 2>/dev/null", qt);

    return run_picker_cmd(cmd, out, out_size);
}

static int try_kdialog_save(char *out, size_t out_size,
                             const char *title,
                             const char *default_name, const char *filter)
{
    if (!tool_exists("kdialog")) return -1;

    char cmd[1024];
    char qt[256] = {0}, qn[256] = {0}, qf[128] = {0};
    shell_quote(title ? title : "Save", qt, sizeof qt);
    shell_quote(default_name ? default_name : "", qn, sizeof qn);
    if (filter) shell_quote(filter, qf, sizeof qf);

    if (filter)
        snprintf(cmd, sizeof cmd,
                  "kdialog --getsavefilename %s %s --title %s 2>/dev/null",
                  qn, qf, qt);
    else
        snprintf(cmd, sizeof cmd,
                  "kdialog --getsavefilename %s --title %s 2>/dev/null",
                  qn, qt);

    return run_picker_cmd(cmd, out, out_size);
}

/* ── 공개 API ─────────────────────────────────────────────────────────────── */

int file_picker_open(char *out_path, size_t out_size,
                      const char *title, const char *filter)
{
    int r = try_zenity_open(out_path, out_size, title, filter);
    if (r != -1) return r;
    r = try_kdialog_open(out_path, out_size, title, filter);
    if (r != -1) return r;
    return -1;
}

int file_picker_save(char *out_path, size_t out_size,
                      const char *title,
                      const char *default_name,
                      const char *filter)
{
    int r = try_zenity_save(out_path, out_size, title, default_name, filter);
    if (r != -1) return r;
    r = try_kdialog_save(out_path, out_size, title, default_name, filter);
    if (r != -1) return r;
    return -1;
}
