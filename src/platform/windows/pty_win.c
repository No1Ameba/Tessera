/*
 * pty_win.c — Windows ConPTY 기반 PTY 백엔드.
 * tessera_pty.h 계약을 forkpty(pty_posix.c) 와 동일한 의미로 구현한다.
 *
 * ⚠️ 검증 상태: 이 파일은 Linux/WSL 개발 환경에서 컴파일 검증되지 않았다.
 *    MSVC(Windows 10 1809+ SDK, ConPTY 필요) 빌드/CI 에서 검증해야 한다.
 *    또한 데몬 이벤트 루프(ipc_server.c)가 epoll 기반이므로, Windows 에서
 *    데몬 전체가 동작하려면 IOCP/WaitForMultipleObjects 로의 포팅이 별도로
 *    필요하다(이 파일 범위 밖).
 *
 * pty_t 매핑(Windows):
 *   hpcon = HPCON, hproc = 자식 프로세스 HANDLE,
 *   hin   = 자식 stdin 으로 쓰는 파이프 write 핸들,
 *   hout  = 자식 stdout/err 에서 읽는 파이프 read 핸들.
 * 논블로킹 read 는 PeekNamedPipe 로 흉내낸다(POSIX 의 O_NONBLOCK 대응).
 */
#include "../tessera_pty.h"

#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int pty_set_nonblocking(int fd) {
    (void)fd;  /* Windows: 논블로킹은 pty_read 의 PeekNamedPipe 로 처리 */
    return 0;
}

int pty_spawn(pty_t *out, const char *shell, uint16_t cols, uint16_t rows) {
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof *out);
    out->master_fd = -1;
    out->child_pid = -1;

    HANDLE inRead = NULL, inWrite = NULL, outRead = NULL, outWrite = NULL;
    if (!CreatePipe(&inRead, &inWrite, NULL, 0)) return -1;
    if (!CreatePipe(&outRead, &outWrite, NULL, 0)) {
        CloseHandle(inRead); CloseHandle(inWrite);
        return -1;
    }

    COORD size = { (SHORT)cols, (SHORT)rows };
    HPCON hpc = NULL;
    HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &hpc);
    /* ConPTY 가 inRead/outWrite 를 복제하므로 우리 쪽 원본은 닫는다. */
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        CloseHandle(inWrite); CloseHandle(outRead);
        return -1;
    }

    /* STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    if (!si.lpAttributeList ||
        !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof hpc, NULL, NULL)) {
        free(si.lpAttributeList);
        ClosePseudoConsole(hpc);
        CloseHandle(inWrite); CloseHandle(outRead);
        return -1;
    }

    /* 셸 결정: 인자 → %COMSPEC% → cmd.exe */
    wchar_t cmdline[512];
    if (shell && shell[0]) {
        if (MultiByteToWideChar(CP_UTF8, 0, shell, -1, cmdline, 512) == 0)
            wcscpy(cmdline, L"cmd.exe");
    } else {
        DWORD n = GetEnvironmentVariableW(L"COMSPEC", cmdline, 512);
        if (n == 0 || n >= 512) wcscpy(cmdline, L"cmd.exe");
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                             &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (!ok) {
        ClosePseudoConsole(hpc);
        CloseHandle(inWrite); CloseHandle(outRead);
        return -1;
    }
    CloseHandle(pi.hThread);

    out->hpcon     = hpc;
    out->hproc     = pi.hProcess;
    out->hin       = inWrite;
    out->hout      = outRead;
    out->child_pid = (int)pi.dwProcessId;
    return 0;
}

ssize_t pty_read(pty_t *pty, void *buf, size_t len) {
    if (!pty || !pty->hout) { errno = EBADF; return -1; }
    DWORD avail = 0;
    /* 논블로킹: 데이터가 없으면 0, 파이프가 닫혔으면(자식 종료) -1(EOF). */
    if (!PeekNamedPipe((HANDLE)pty->hout, NULL, 0, NULL, &avail, NULL))
        return -1;
    if (avail == 0) return 0;
    DWORD toread = (avail < (DWORD)len) ? avail : (DWORD)len;
    DWORD got = 0;
    if (!ReadFile((HANDLE)pty->hout, buf, toread, &got, NULL))
        return -1;
    return (ssize_t)got;
}

ssize_t pty_write(pty_t *pty, const void *buf, size_t len) {
    if (!pty || !pty->hin) { errno = EBADF; return -1; }
    DWORD wrote = 0;
    if (!WriteFile((HANDLE)pty->hin, buf, (DWORD)len, &wrote, NULL))
        return -1;
    return (ssize_t)wrote;
}

int pty_resize(pty_t *pty, uint16_t cols, uint16_t rows) {
    if (!pty || !pty->hpcon) { errno = EBADF; return -1; }
    COORD size = { (SHORT)cols, (SHORT)rows };
    return SUCCEEDED(ResizePseudoConsole((HPCON)pty->hpcon, size)) ? 0 : -1;
}

void pty_close(pty_t *pty, int *exit_status) {
    if (!pty) return;

    if (pty->hproc) {
        /* stdin 파이프를 닫아 셸에 EOF 를 주고 100ms 대기, 그래도 살아있으면
         * TerminateProcess (POSIX 의 SIGTERM→SIGKILL 순서에 대응). */
        if (pty->hin) { CloseHandle((HANDLE)pty->hin); pty->hin = NULL; }
        if (WaitForSingleObject((HANDLE)pty->hproc, 100) != WAIT_OBJECT_0)
            TerminateProcess((HANDLE)pty->hproc, 1);
        WaitForSingleObject((HANDLE)pty->hproc, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess((HANDLE)pty->hproc, &code);
        if (exit_status) *exit_status = (int)code;
        CloseHandle((HANDLE)pty->hproc);
        pty->hproc = NULL;
    } else if (exit_status) {
        *exit_status = -1;
    }

    if (pty->hpcon) { ClosePseudoConsole((HPCON)pty->hpcon); pty->hpcon = NULL; }
    if (pty->hin)   { CloseHandle((HANDLE)pty->hin);  pty->hin  = NULL; }
    if (pty->hout)  { CloseHandle((HANDLE)pty->hout); pty->hout = NULL; }
    pty->child_pid = -1;
}
