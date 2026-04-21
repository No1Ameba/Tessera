#define _GNU_SOURCE

#include "ipc_server.h"
#include "session.h"
#include "../common/config.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── 전역 종료 플래그 ───────────────────────────────────────────────────── */

static ipc_server_t *g_server = NULL;

static void sig_handler(int sig) {
    (void)sig;
    if (g_server) ipc_server_shutdown(g_server);
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
        if (daemon(0, 0) < 0) {
            perror("daemon");
            return 1;
        }
    }

    /* 시그널 핸들러 등록 */
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

    /* 설정 로드 — ~/.config/termemu/config.json 의 daemon 섹션 반영 */
    {
        termemu_config_t cfg;
        config_defaults(&cfg);
        const char *home = getenv("HOME");
        if (home) {
            char cfg_path[512];
            snprintf(cfg_path, sizeof cfg_path,
                     "%s/.config/termemu/config.json", home);
            config_load_file(cfg_path, &cfg);  /* 실패 시 defaults 유지 */
        }
        ipc_server_configure(srv, cfg.autosave_interval,
                              cfg.session_idle_timeout);
        if (!daemonize)
            fprintf(stderr,
                    "[termemu-daemon] config: autosave=%ds, idle_timeout=%ds\n",
                    cfg.autosave_interval, cfg.session_idle_timeout);
    }

    if (!daemonize)
        fprintf(stderr, "[termemu-daemon] 시작 (pid=%d)\n", (int)getpid());

    /* 이전 세션 스냅샷이 있으면 복원 (크래시 복구) */
    int restored = ipc_server_restore_sessions(srv);
    if (!daemonize && restored > 0)
        fprintf(stderr, "[termemu-daemon] %d 개 세션 복원\n", restored);

    /* 메인 이벤트 루프 (SIGTERM/SIGINT 시 반환) */
    ipc_server_run(srv);

    /* 정리 */
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);

    if (!daemonize)
        fprintf(stderr, "[termemu-daemon] 종료\n");

    return 0;
}
