#define _GNU_SOURCE

#include "ipc_server.h"
#include "session.h"
#include "../common/config.h"
#include "../common/paths.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <process.h>     /* _getpid */
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

/* ─── 전역 종료 플래그 ───────────────────────────────────────────────────── */

static ipc_server_t *g_server = NULL;

static void sig_handler(int sig) {
    (void)sig;
    if (g_server) ipc_server_shutdown(g_server);
}

#ifdef _WIN32
/* 콘솔 닫기·로그오프·셧다운은 시그널로 오지 않아 따로 받아야 한다. */
static BOOL WINAPI console_ctrl_handler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_server) ipc_server_shutdown(g_server);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/*
 * 종료 신호 처리 등록.
 *
 * POSIX 는 SIGTERM/SIGINT 에 더해 SIGCHLD 를 SIG_IGN + SA_NOCLDWAIT 로 두어
 * PTY 자식이 좀비로 남지 않게 한다. Windows 에는 좀비 개념이 없어 그 부분이
 * 필요 없고, 대신 콘솔 제어 이벤트를 받는다.
 */
static void install_signal_handlers(void)
{
#ifdef _WIN32
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* SIGCHLD: PTY 자식 프로세스 자동 수거 (좀비 방지) */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = SIG_IGN;
    sa_chld.sa_flags   = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa_chld, NULL);
#endif
}

/*
 * 백그라운드로 분리한다.
 *
 * Windows 에는 fork 기반 daemon(3) 이 없다. 대신 프로세스를 띄우는 쪽이
 * DETACHED_PROCESS 로 만들고, 여기서는 콘솔이 붙어 있으면 떼어낸다.
 * @return 0 성공, -1 실패.
 */
static int detach_from_terminal(void)
{
#ifdef _WIN32
    FreeConsole();   /* 콘솔이 없으면 실패하지만 무해하다 */
    return 0;
#else
    return daemon(0, 0);
#endif
}

/* ─── 사용법 ─────────────────────────────────────────────────────────────── */

static void usage(const char *progname) {
    fprintf(stderr,
        "사용법: %s [옵션]\n"
        "  --daemon   백그라운드 데몬으로 실행\n"
        "  --help     도움말 출력\n",
        progname);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int daemonize = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            daemonize = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "알 수 없는 인자: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 데몬화 */
    if (daemonize) {
        if (detach_from_terminal() < 0) {
            perror("daemon");
            return 1;
        }
    }

    install_signal_handlers();

    /* 세션 매니저 초기화 */
    session_manager_t mgr;
    session_manager_init(&mgr);

    /* IPC 서버 생성 및 소켓 바인드 */
    ipc_server_t *srv = ipc_server_create(&mgr);
    if (!srv) {
        fprintf(stderr, "ipc_server_create 실패\n");
        return 1;
    }
    g_server = srv;

    if (ipc_server_listen(srv) < 0) {
        perror("ipc_server_listen");
        ipc_server_destroy(srv);
        return 1;
    }

    /* 설정 로드 — <설정 디렉토리>/config.json 의 daemon 섹션 반영 */
    {
        tessera_config_t cfg;
        config_defaults(&cfg);
        char cfg_path[512];
        if (tessera_config_path("config.json", cfg_path, sizeof cfg_path) == 0)
            config_load_file(cfg_path, &cfg);  /* 실패 시 defaults 유지 */
        ipc_server_configure(srv, cfg.autosave_interval,
                              cfg.session_idle_timeout);
        if (!daemonize)
            fprintf(stderr,
                    "[tessera-daemon] config: autosave=%ds, idle_timeout=%ds\n",
                    cfg.autosave_interval, cfg.session_idle_timeout);
    }

    if (!daemonize)
        fprintf(stderr, "[tessera-daemon] 시작 (pid=%d)\n", (int)getpid());

    /* 이전 세션 스냅샷이 있으면 복원 (크래시 복구) */
    int restored = ipc_server_restore_sessions(srv);
    if (!daemonize && restored > 0)
        fprintf(stderr, "[tessera-daemon] %d 개 세션 복원\n", restored);

    /* 메인 이벤트 루프 (SIGTERM/SIGINT 시 반환) */
    ipc_server_run(srv);

    /* 정리 */
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);

    if (!daemonize)
        fprintf(stderr, "[tessera-daemon] 종료\n");

    return 0;
}
