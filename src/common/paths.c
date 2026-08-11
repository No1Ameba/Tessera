#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>          /* _mkdir */
#  include <process.h>         /* _getpid */
#  define PATH_SEP '\\'
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define PATH_SEP '/'
#endif

int tessera_config_dir(char *buf, size_t buflen)
{
    int n;
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !base[0]) return -1;
    n = snprintf(buf, buflen, "%s\\tessera", base);
#else
    /* TODO: $XDG_CONFIG_HOME 존중. 지금 바꾸면 클라이언트/데몬 양쪽 호출부를
     * 모두 함께 옮겨야 해서 별도 작업으로 남긴다. */
    const char *base = getenv("HOME");
    if (!base || !base[0]) return -1;
    n = snprintf(buf, buflen, "%s/.config/tessera", base);
#endif
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}

int tessera_config_path(const char *rel, char *buf, size_t buflen)
{
    char dir[512];
    if (tessera_config_dir(dir, sizeof dir) != 0) return -1;
    int n = snprintf(buf, buflen, "%s%c%s", dir, PATH_SEP, rel ? rel : "");
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}

/* 디렉토리 하나를 만든다. 이미 있으면 성공. */
static int mkdir_one(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) return 0;
    return (GetLastError() == ERROR_ALREADY_EXISTS ||
            GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? 0 : -1;
#else
    if (mkdir(path, 0755) == 0) return 0;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
#endif
}

int tessera_mkdir_p(const char *path)
{
    if (!path || !path[0]) return -1;

    char tmp[512];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;

    /* 구분자를 하나씩 잘라 올라가며 상위부터 만든다.
     * 선두(POSIX 의 "/", Windows 의 "C:\")는 건너뛴다. */
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char save = *p;
        *p = '\0';
        /* "C:" 같은 드라이브 지정자만 남은 경우는 만들 것이 없다. */
        if (!(p == tmp + 2 && tmp[1] == ':') && mkdir_one(tmp) != 0) {
            *p = save;
            return -1;
        }
        *p = save;
    }
    return mkdir_one(tmp);
}

int tessera_temp_path(const char *prefix, char *buf, size_t buflen)
{
    int n;
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    DWORD len = GetTempPathA((DWORD)sizeof tmpdir, tmpdir);
    if (len == 0 || len >= sizeof tmpdir) return -1;
    /* GetTempPath 는 후행 백슬래시를 포함한다. */
    n = snprintf(buf, buflen, "%s%s-%lu.tmp", tmpdir, prefix ? prefix : "tessera",
                 (unsigned long)GetCurrentProcessId());
#else
    n = snprintf(buf, buflen, "/tmp/%s-%d.tmp", prefix ? prefix : "tessera",
                 (int)getpid());
#endif
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}
