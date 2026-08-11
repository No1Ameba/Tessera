/*
 * pty_win.c — Windows ConPTY 기반 PTY 백엔드.
 * tessera_pty.h 계약을 forkpty(pty_posix.c) 와 동일한 의미로 구현한다.
 *
 * pty_t 매핑(Windows):
 *   hpcon = HPCON, hproc = 자식 프로세스 HANDLE,
 *   hin   = 자식 stdin 으로 쓰는 파이프 write 핸들,
 *   hout  = 자식 stdout/err 에서 읽는 파이프 read 핸들,
 *   hev   = hout 의 overlapped read 완료를 기다리는 이벤트,
 *   master_fd = hout 을 감싼 CRT fd (데몬 이벤트 루프 등록용).
 *
 * 논블로킹 read 는 PeekNamedPipe 로 흉내낸다(POSIX 의 O_NONBLOCK 대응).
 *
 * ⚠️ CreatePipe 를 쓰지 않는 이유: 익명 파이프는 overlapped I/O 를 지원하지 않아
 *    IOCP(event_loop_win.c)에 등록할 수 없다. 그래서 hout 만큼은 고유 이름의
 *    Named Pipe 를 FILE_FLAG_OVERLAPPED 로 만들어 쓴다.
 */
#include "../tessera_pty.h"

#include <windows.h>
#include <errno.h>
#include <io.h>       /* _open_osfhandle, _close */
#include <fcntl.h>    /* _O_RDONLY */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int pty_set_nonblocking(int fd) {
    (void)fd;  /* Windows: 논블로킹은 pty_read 의 PeekNamedPipe 로 처리 */
    return 0;
}

/*
 * 한쪽 끝을 overlapped 로 열 수 있는 파이프 쌍을 만든다.
 *
 * CreateNamedPipeW 로 서버 끝을, CreateFileW 로 클라이언트 끝을 만들어 잇는다
 * (CreateFileW 가 성공하는 순간 연결되므로 ConnectNamedPipe 는 불필요).
 *
 * @param server_inbound  1 이면 서버 끝이 읽기(우리가 읽음), 0 이면 쓰기.
 * @param overlapped      서버 끝에 FILE_FLAG_OVERLAPPED 를 건다.
 */
static int make_pipe(HANDLE *server, HANDLE *client,
                     int server_inbound, int overlapped)
{
    static volatile LONG serial = 0;
    wchar_t name[128];
    swprintf(name, sizeof name / sizeof name[0],
             L"\\\\.\\pipe\\tessera-pty-%lu-%ld",
             (unsigned long)GetCurrentProcessId(),
             (long)InterlockedIncrement(&serial));

    DWORD mode = (server_inbound ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND)
               | (overlapped ? FILE_FLAG_OVERLAPPED : 0);

    HANDLE s = CreateNamedPipeW(name, mode,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                1, 65536, 65536, 0, NULL);
    if (s == INVALID_HANDLE_VALUE) return -1;

    HANDLE c = CreateFileW(name,
                           server_inbound ? GENERIC_WRITE : GENERIC_READ,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (c == INVALID_HANDLE_VALUE) { CloseHandle(s); return -1; }

    *server = s;
    *client = c;
    return 0;
}

int pty_spawn(pty_t *out, const char *shell, uint16_t cols, uint16_t rows) {
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof *out);
    out->master_fd = -1;
    out->child_pid = -1;

    /* stdin: 우리가 쓰는 쪽(inWrite)이 서버, ConPTY 가 읽는 쪽(inRead)이 클라이언트.
     * stdout: 우리가 읽는 쪽(outRead)이 서버 — 여기에만 overlapped 가 필요하다. */
    HANDLE inRead = NULL, inWrite = NULL, outRead = NULL, outWrite = NULL;
    if (make_pipe(&inWrite, &inRead, /*server_inbound=*/0, /*overlapped=*/0) != 0)
        return -1;
    if (make_pipe(&outRead, &outWrite, /*server_inbound=*/1, /*overlapped=*/1) != 0) {
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

    /* overlapped read 완료 대기용 이벤트 (수동 리셋). */
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* hout 을 CRT fd 로 감싼다 — 데몬이 evloop_add(master_fd) 로 감시한다.
     * 이후 핸들 소유권은 CRT 로 넘어가므로 CloseHandle 로 닫으면 안 된다. */
    int mfd = _open_osfhandle((intptr_t)outRead, _O_RDONLY);
    if (!ev || mfd < 0) {
        if (ev) CloseHandle(ev);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        ClosePseudoConsole(hpc);
        CloseHandle(inWrite);
        if (mfd >= 0) _close(mfd); else CloseHandle(outRead);
        return -1;
    }

    out->hpcon     = hpc;
    out->hproc     = pi.hProcess;
    out->hin       = inWrite;
    out->hout      = outRead;
    out->hev       = ev;
    out->master_fd = mfd;
    out->child_pid = (int)pi.dwProcessId;
    return 0;
}

ssize_t pty_read(pty_t *pty, void *buf, size_t len) {
    if (!pty || !pty->hout) { errno = EBADF; return -1; }
    HANDLE h = (HANDLE)pty->hout;

    DWORD avail = 0;
    /* 논블로킹: 데이터가 없으면 0, 파이프가 닫혔으면(자식 종료) -1(EOF). */
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
        return -1;
    if (avail == 0) return 0;
    DWORD toread = (avail < (DWORD)len) ? avail : (DWORD)len;

    /* hout 은 FILE_FLAG_OVERLAPPED 로 열려 있으므로 OVERLAPPED 없이 ReadFile 을
     * 부르면 안 된다. hEvent 의 최하위 비트를 세우면 이 I/O 의 완료 패킷이
     * IOCP 로 가지 않는다(OVERLAPPED 문서화 규약) — 데몬이 같은 핸들을 IOCP 에
     * 물려 둔 상태에서 event_loop_win.c 의 readiness 통지와 섞이지 않게 한다.
     * 그 대신 완료는 이벤트를 직접 기다려 확인한다. */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ResetEvent((HANDLE)pty->hev);
    ov.hEvent = (HANDLE)((ULONG_PTR)pty->hev | 1);

    if (!ReadFile(h, buf, toread, NULL, &ov) &&
        GetLastError() != ERROR_IO_PENDING)
        return -1;

    /* 위에서 avail 바이트가 있음을 확인했으므로 사실상 즉시 완료된다. */
    if (WaitForSingleObject((HANDLE)pty->hev, INFINITE) != WAIT_OBJECT_0)
        return -1;

    DWORD got = 0;
    if (!GetOverlappedResult(h, &ov, &got, FALSE))
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
    if (pty->hev)   { CloseHandle((HANDLE)pty->hev);  pty->hev  = NULL; }
    /* hout 은 _open_osfhandle 로 CRT 에 소유권을 넘겼으므로 _close 로만 닫는다
     * (CloseHandle 을 같이 부르면 이중 해제가 된다). */
    if (pty->master_fd >= 0) { _close(pty->master_fd); pty->master_fd = -1; }
    pty->hout = NULL;
    pty->child_pid = -1;
}
