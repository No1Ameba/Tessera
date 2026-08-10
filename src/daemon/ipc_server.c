#define _GNU_SOURCE

#include "ipc_server.h"
#include "session.h"
#include "../common/ipc_proto.h"
#include "../common/session_file.h"
#include <dirent.h>
#include "../platform/ipc.h"
#include "../platform/tessera_pty.h"
#include "../platform/event_loop.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ─── 상수 ───────────────────────────────────────────────────────────────── */

#define IPC_MAX_CLIENTS   16
#define IPC_MAX_PANES     64
#define IPC_LOOP_EVENTS   32   /* evloop_wait 배치 크기 */

/* 세션 수명/자동 저장 기본값 (config 미설정 시). 단위: 초. */
#define IPC_SERVER_DEFAULT_IDLE_SEC     (5 * 60)
#define IPC_SERVER_DEFAULT_AUTOSAVE_SEC (5 * 60)

/* 수신 버퍼: 헤더 + 최대 페이로드 */
#define IPC_RBUF_SIZE  (sizeof(ipc_msg_header_t) + IPC_MAX_PAYLOAD_LEN)

/* ─── 내부 타입 ──────────────────────────────────────────────────────────── */

typedef struct {
    int      fd;           /* -1: 빈 슬롯 */
    uint32_t session_id;   /* 0 = 아직 attach/create 안 함 */
    uint8_t  rbuf[IPC_RBUF_SIZE];
    size_t   rbuf_len;
} ipc_client_slot_t;

#define PANE_RING_SIZE  (256 * 1024)  /* PTY 출력 링 버퍼 크기 (attach 시 replay) */

typedef struct {
    int      pty_fd;      /* -1: 빈 슬롯 */
    uint32_t pane_id;
    uint32_t session_id;
    uint32_t window_id;

    /* PTY 출력 링 버퍼 — attach 시 재전송용 */
    uint8_t  ring[PANE_RING_SIZE];
    size_t   ring_head;   /* 다음 쓰기 위치 */
    size_t   ring_count;  /* 저장된 바이트 수 */

    /*
     * 클라이언트별로 요청한 크기. 인덱스는 srv->clients 슬롯 인덱스와 같다.
     * PTY 는 하나뿐이므로 여러 클라이언트가 붙으면 최소값으로 클램프한다
     * (tmux 기본 동작). 더 큰 클라이언트에는 남는 영역이 여백으로 보인다.
     * 0 = 해당 클라이언트가 이 pane 에 대해 요청한 적 없음.
     */
    uint16_t req_cols[IPC_MAX_CLIENTS];
    uint16_t req_rows[IPC_MAX_CLIENTS];
} ipc_pane_slot_t;

struct ipc_server {
    int                 listen_fd;
    event_loop_t       *evloop;
    char                sock_path[IPC_SOCKET_PATH_MAX];
    session_manager_t  *session_mgr;
    ipc_client_slot_t   clients[IPC_MAX_CLIENTS];
    ipc_pane_slot_t     panes[IPC_MAX_PANES];
    volatile int        running;
    int                 ever_had_client;  /* 첫 클라이언트 연결 후 1로 설정 */

    /* 설정값 (config.json 에서 로드). 기본값은 ipc_server_create 에서 세팅. */
    int                 autosave_interval_sec;    /* 0=비활성 */
    int                 session_idle_timeout_sec; /* 0=즉시 destroy */
};

static int write_all_retry(int fd, const void *buf, size_t len);

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 세션에 남은 pane 총 개수 (모든 윈도우 합) */
static size_t session_total_panes(const session_t *s) {
    size_t n = 0;
    for (const window_t *w = s->windows; w; w = w->next)
        n += w->pane_count;
    return n;
}

static void delete_session_snapshot(const char *sess_name);

/* 세션에 pane 이 하나도 남지 않았으면 세션을 destroy 한다.
 * (셸이 모두 종료되었거나 사용자가 모든 pane 을 닫은 경우.) */
static void session_destroy_if_empty(ipc_server_t *srv, session_t *s) {
    if (!s) return;
    if (session_total_panes(s) > 0) return;
    /* 디스크 스냅샷도 제거하여 재시작 시 복원되지 않도록 */
    delete_session_snapshot(s->name);
    /* PTY 슬롯 정리는 이미 호출자가 처리했으므로 session_destroy 만 호출 */
    session_destroy(srv->session_mgr, s);
}

/* 클라이언트 슬롯의 session_id 를 변경하고 세션별 attach_count 를 갱신한다.
 * new_sid == 0 은 detach 의미 (연결 종료 등). */
static void client_set_session(ipc_server_t *srv,
                                ipc_client_slot_t *c, uint32_t new_sid)
{
    if (!c) return;
    if (c->session_id == new_sid) return;

    if (c->session_id != 0) {
        session_t *old = session_find_by_id(srv->session_mgr, c->session_id);
        if (old && old->attach_count > 0) {
            old->attach_count--;
            if (old->attach_count == 0)
                old->last_detach_ms = mono_ms();
        }
    }

    c->session_id = new_sid;

    if (new_sid != 0) {
        session_t *ns = session_find_by_id(srv->session_mgr, new_sid);
        if (ns) {
            ns->attach_count++;
            /* attach 중에는 idle 타이머 의미 없음 — 0으로 리셋 */
            ns->last_detach_ms = 0;
        }
    }
}

/* ─── 헬퍼: fd → 슬롯 조회 ───────────────────────────────────────────────── */

static ipc_client_slot_t *client_by_fd(ipc_server_t *srv, int fd) {
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
        if (srv->clients[i].fd == fd) return &srv->clients[i];
    return NULL;
}

/* 클라이언트 슬롯 인덱스 (pane 별 요청 크기 테이블의 키). 없으면 -1. */
static int client_index_by_fd(ipc_server_t *srv, int fd) {
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
        if (srv->clients[i].fd == fd) return i;
    return -1;
}

static ipc_client_slot_t *client_empty_slot(ipc_server_t *srv) {
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
        if (srv->clients[i].fd < 0) return &srv->clients[i];
    return NULL;
}

static ipc_pane_slot_t *pane_by_pty_fd(ipc_server_t *srv, int pty_fd) {
    for (int i = 0; i < IPC_MAX_PANES; i++)
        if (srv->panes[i].pty_fd == pty_fd) return &srv->panes[i];
    return NULL;
}

static ipc_pane_slot_t *pane_by_id(ipc_server_t *srv, uint32_t pane_id) {
    for (int i = 0; i < IPC_MAX_PANES; i++)
        if (srv->panes[i].pty_fd >= 0 && srv->panes[i].pane_id == pane_id)
            return &srv->panes[i];
    return NULL;
}

static ipc_pane_slot_t *pane_empty_slot(ipc_server_t *srv) {
    for (int i = 0; i < IPC_MAX_PANES; i++) {
        if (srv->panes[i].pty_fd < 0) {
            /* 슬롯은 재사용되므로 이전 pane 의 크기 요청을 반드시 지운다.
             * 남아 있으면 새 pane 이 엉뚱한 크기로 클램프된다. */
            memset(srv->panes[i].req_cols, 0, sizeof(srv->panes[i].req_cols));
            memset(srv->panes[i].req_rows, 0, sizeof(srv->panes[i].req_rows));
            return &srv->panes[i];
        }
    }
    return NULL;
}

/* ─── 헬퍼: 메시지 전송 ──────────────────────────────────────────────────── */

static int send_msg(int fd, ipc_msg_type_t type,
                    const void *payload, uint16_t plen) {
    ipc_msg_header_t hdr = IPC_HEADER_INIT(type, plen);
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return -1;
    if (plen > 0 && payload)
        if (write(fd, payload, plen) != (ssize_t)plen) return -1;
    return 0;
}

static int send_ok(int fd) {
    return send_msg(fd, IPC_MSG_OK, NULL, 0);
}

static int send_error(int fd, ipc_msg_type_t req_type, ipc_error_code_t code,
                      const char *msg) {
    ipc_payload_error_t err;
    memset(&err, 0, sizeof(err));
    err.request_type = (uint32_t)req_type;
    err.error_code   = (uint32_t)code;
    if (msg) strncpy(err.message, msg, sizeof(err.message) - 1);
    return send_msg(fd, IPC_MSG_ERROR, &err, sizeof(err));
}

/* ─── 헬퍼: 이벤트 루프 등록/해제 ────────────────────────────────────────── */

static int loop_add(ipc_server_t *srv, int fd, uint32_t interest) {
    return evloop_add(srv->evloop, fd, interest);
}

static int loop_del(ipc_server_t *srv, int fd) {
    return evloop_del(srv->evloop, fd);
}

/* ─── 메시지 핸들러 ──────────────────────────────────────────────────────── */

static void handle_hello(ipc_server_t *srv, int client_fd,
                         const uint8_t *payload, uint16_t plen) {
    (void)payload; (void)plen;

    ipc_payload_hello_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.daemon_pid    = (uint32_t)getpid();
    ack.session_count = (uint32_t)srv->session_mgr->count;
    strncpy(ack.daemon_version, "0.1.0", sizeof(ack.daemon_version) - 1);

    send_msg(client_fd, IPC_MSG_HELLO_ACK, &ack, sizeof(ack));
}

static void handle_session_create(ipc_server_t *srv, int client_fd,
                                  const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_session_create_t)) {
        send_error(client_fd, IPC_MSG_SESSION_CREATE,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_session_create_t *req =
        (const ipc_payload_session_create_t *)payload;

    /* 이름 중복 확인 */
    if (session_find_by_name(srv->session_mgr, req->name)) {
        send_error(client_fd, IPC_MSG_SESSION_CREATE,
                   IPC_ERR_NAME_CONFLICT, "session name already exists");
        return;
    }

    session_t *s = session_create(srv->session_mgr, req->name);
    if (!s) {
        send_error(client_fd, IPC_MSG_SESSION_CREATE,
                   IPC_ERR_LIMIT_REACHED, "session_create failed");
        return;
    }

    ipc_payload_session_created_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.session_id = s->id;
    strncpy(resp.name, s->name, sizeof(resp.name) - 1);
    send_msg(client_fd, IPC_MSG_SESSION_CREATED, &resp, sizeof(resp));

    /* 생성자 클라이언트는 암묵적으로 attach */
    client_set_session(srv, client_by_fd(srv, client_fd), s->id);
}

static void handle_session_destroy(ipc_server_t *srv, int client_fd,
                                   const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_session_destroy_t)) {
        send_error(client_fd, IPC_MSG_SESSION_DESTROY,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_session_destroy_t *req =
        (const ipc_payload_session_destroy_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_SESSION_DESTROY,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }

    /* 세션 내 모든 pane 슬롯의 pty_fd 를 epoll 에서 제거 */
    for (window_t *w = s->windows; w; w = w->next) {
        for (pane_t *p = w->panes; p; p = p->next) {
            ipc_pane_slot_t *slot = pane_by_id(srv, p->id);
            if (slot) {
                loop_del(srv, slot->pty_fd);
                slot->pty_fd = -1;
            }
        }
    }

    delete_session_snapshot(s->name);
    session_destroy(srv->session_mgr, s);
    send_ok(client_fd);
}

static void handle_session_list(ipc_server_t *srv, int client_fd) {
    uint32_t count = (uint32_t)srv->session_mgr->count;
    if (count == 0) {
        send_msg(client_fd, IPC_MSG_SESSION_LIST_R, NULL, 0);
        return;
    }

    ipc_session_info_t *infos = calloc(count, sizeof(*infos));
    if (!infos) {
        send_error(client_fd, IPC_MSG_SESSION_LIST,
                   IPC_ERR_UNKNOWN, "out of memory");
        return;
    }

    uint32_t i = 0;
    for (session_t *s = srv->session_mgr->head; s && i < count;
         s = s->next, i++) {
        infos[i].session_id   = s->id;
        infos[i].window_count = (uint32_t)s->window_count;
        strncpy(infos[i].name, s->name, sizeof(infos[i].name) - 1);
    }

    send_msg(client_fd, IPC_MSG_SESSION_LIST_R,
             infos, (uint16_t)(i * sizeof(*infos)));
    free(infos);
}

static void handle_window_create(ipc_server_t *srv, int client_fd,
                                  const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_window_create_t)) {
        send_error(client_fd, IPC_MSG_WINDOW_CREATE,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_window_create_t *req =
        (const ipc_payload_window_create_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_WINDOW_CREATE,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }

    window_t *w = window_create(s, req->name);
    if (!w) {
        send_error(client_fd, IPC_MSG_WINDOW_CREATE,
                   IPC_ERR_LIMIT_REACHED, "window_create failed");
        return;
    }

    ipc_payload_window_created_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.session_id = s->id;
    resp.window_id  = w->id;
    strncpy(resp.name, w->name, sizeof(resp.name) - 1);
    send_msg(client_fd, IPC_MSG_WINDOW_CREATED, &resp, sizeof(resp));
}

static void handle_window_destroy(ipc_server_t *srv, int client_fd,
                                   const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_window_ref_t)) {
        send_error(client_fd, IPC_MSG_WINDOW_DESTROY,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_window_ref_t *req =
        (const ipc_payload_window_ref_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_WINDOW_DESTROY,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }

    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) {
        send_error(client_fd, IPC_MSG_WINDOW_DESTROY,
                   IPC_ERR_WINDOW_NOT_FOUND, "window not found");
        return;
    }

    /* 윈도우 내 모든 pane PTY 정리 */
    for (pane_t *p = w->panes; p; p = p->next) {
        ipc_pane_slot_t *slot = pane_by_id(srv, p->id);
        if (slot) {
            loop_del(srv, slot->pty_fd);
            pty_t pty = { .master_fd = slot->pty_fd, .child_pid = (pid_t)p->pid };
            pty_close(&pty, NULL);
            slot->pty_fd = -1;
        }
    }

    window_destroy(s, w);
    send_ok(client_fd);
}

static void handle_window_focus(ipc_server_t *srv, int client_fd,
                                 const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_window_ref_t)) {
        send_error(client_fd, IPC_MSG_WINDOW_FOCUS,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_window_ref_t *req =
        (const ipc_payload_window_ref_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_WINDOW_FOCUS,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }
    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) {
        send_error(client_fd, IPC_MSG_WINDOW_FOCUS,
                   IPC_ERR_WINDOW_NOT_FOUND, "window not found");
        return;
    }

    window_set_active(s, w);
    send_ok(client_fd);
}

/* 클라이언트가 layout tree blob 을 업로드한다. 데몬은 저장만 한다. */
static void handle_window_layout(ipc_server_t *srv, int client_fd,
                                  const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_window_layout_t)) {
        send_error(client_fd, IPC_MSG_WINDOW_LAYOUT,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_window_layout_t *req =
        (const ipc_payload_window_layout_t *)payload;
    if (plen < sizeof(*req) + req->blob_len) {
        send_error(client_fd, IPC_MSG_WINDOW_LAYOUT,
                   IPC_ERR_INVALID_MSG, "blob truncated");
        return;
    }
    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) { send_error(client_fd, IPC_MSG_WINDOW_LAYOUT,
                          IPC_ERR_SESSION_NOT_FOUND, "session not found"); return; }
    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) { send_error(client_fd, IPC_MSG_WINDOW_LAYOUT,
                          IPC_ERR_WINDOW_NOT_FOUND, "window not found"); return; }

    free(w->layout_blob);
    w->layout_blob = NULL;
    w->layout_blob_len = 0;
    if (req->blob_len > 0) {
        w->layout_blob = malloc(req->blob_len);
        if (w->layout_blob) {
            memcpy(w->layout_blob, payload + sizeof(*req), req->blob_len);
            w->layout_blob_len = req->blob_len;
        }
    }
    send_ok(client_fd);
}

static void handle_pane_create(ipc_server_t *srv, int client_fd,
                                const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_pane_create_t)) {
        send_error(client_fd, IPC_MSG_PANE_CREATE,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_pane_create_t *req =
        (const ipc_payload_pane_create_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_PANE_CREATE,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }
    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) {
        send_error(client_fd, IPC_MSG_PANE_CREATE,
                   IPC_ERR_WINDOW_NOT_FOUND, "window not found");
        return;
    }

    ipc_pane_slot_t *slot = pane_empty_slot(srv);
    if (!slot) {
        send_error(client_fd, IPC_MSG_PANE_CREATE,
                   IPC_ERR_LIMIT_REACHED, "max panes reached");
        return;
    }

    pane_t *p = pane_create(w, req->cols, req->rows);
    if (!p) {
        send_error(client_fd, IPC_MSG_PANE_CREATE,
                   IPC_ERR_LIMIT_REACHED, "pane_create failed");
        return;
    }

    /* PTY 스폰 */
    pty_t pty;
    if (pty_spawn(&pty, NULL, req->cols, req->rows) < 0) {
        pane_destroy(w, p);
        send_error(client_fd, IPC_MSG_PANE_CREATE,
                   IPC_ERR_PTY_SPAWN_FAILED, "pty_spawn failed");
        return;
    }

    /* pane_t 슬롯에 기록 */
    p->pty_fd = pty.master_fd;
    p->pid    = (int)pty.child_pid;

    /* ipc_server 내부 슬롯 기록 */
    slot->pty_fd     = pty.master_fd;
    slot->pane_id    = p->id;
    slot->session_id = s->id;
    slot->window_id  = w->id;

    /* PTY fd 를 epoll 에 등록 */
    loop_add(srv, pty.master_fd, EV_READ | EV_EDGE);

    ipc_payload_pane_created_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.session_id = s->id;
    resp.window_id  = w->id;
    resp.pane_id    = p->id;
    resp.pid        = (int32_t)pty.child_pid;
    send_msg(client_fd, IPC_MSG_PANE_CREATED, &resp, sizeof(resp));

    /* split 이라면 다른 attach 된 클라이언트들에 NOTIFY 브로드캐스트 */
    if (req->parent_pane_id != 0) {
        ipc_payload_pane_split_notify_t notify;
        memset(&notify, 0, sizeof(notify));
        notify.session_id     = s->id;
        notify.window_id      = w->id;
        notify.parent_pane_id = req->parent_pane_id;
        notify.new_pane_id    = p->id;
        notify.cols           = req->cols;
        notify.rows           = req->rows;
        notify.direction      = req->direction;
        notify.ratio          = req->ratio;

        ipc_msg_header_t nhdr = IPC_HEADER_INIT(IPC_MSG_PANE_SPLIT_NOTIFY,
                                                 sizeof(notify));
        for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
            int cfd = srv->clients[i].fd;
            if (cfd < 0 || cfd == client_fd) continue;
            write(cfd, &nhdr,   sizeof(nhdr));
            write(cfd, &notify, sizeof(notify));
        }
    }
}

static void handle_pane_destroy(ipc_server_t *srv, int client_fd,
                                 const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_pane_ref_t)) {
        send_error(client_fd, IPC_MSG_PANE_DESTROY,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_pane_ref_t *req =
        (const ipc_payload_pane_ref_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_PANE_DESTROY,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }
    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) {
        send_error(client_fd, IPC_MSG_PANE_DESTROY,
                   IPC_ERR_WINDOW_NOT_FOUND, "window not found");
        return;
    }
    pane_t *p = pane_find_by_id(w, req->pane_id);
    if (!p) {
        send_error(client_fd, IPC_MSG_PANE_DESTROY,
                   IPC_ERR_PANE_NOT_FOUND, "pane not found");
        return;
    }

    ipc_pane_slot_t *slot = pane_by_id(srv, p->id);
    if (slot) {
        loop_del(srv, slot->pty_fd);
        pty_t pty = { .master_fd = slot->pty_fd, .child_pid = (pid_t)p->pid };
        pty_close(&pty, NULL);
        slot->pty_fd = -1;
    }

    /* 다른 클라이언트들에 pane 제거를 알린다 (shell-exit 경로와 동일 메시지) */
    ipc_payload_pane_ref_t nref = { .session_id = s->id,
                                     .window_id  = w->id,
                                     .pane_id    = p->id };
    ipc_msg_header_t nhdr = IPC_HEADER_INIT(IPC_MSG_PANE_EXITED, sizeof(nref));
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        int cfd = srv->clients[i].fd;
        if (cfd < 0 || cfd == client_fd) continue;
        write(cfd, &nhdr, sizeof(nhdr));
        write(cfd, &nref, sizeof(nref));
    }

    pane_destroy(w, p);
    send_ok(client_fd);

    session_destroy_if_empty(srv, s);
}

/* ── pane 크기 협상 (다중 클라이언트 최소 클램프) ─────────────────────────── */

/*
 * 이 pane 에 대해 클라이언트들이 요청한 크기의 최소값.
 * 요청이 하나도 없으면 두 출력 모두 0 으로 둔다.
 */
static void pane_min_size(const ipc_pane_slot_t *slot,
                           uint16_t *out_cols, uint16_t *out_rows)
{
    uint16_t mc = 0, mr = 0;
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        uint16_t c = slot->req_cols[i], r = slot->req_rows[i];
        if (c == 0 || r == 0) continue;   /* 요청 없음 */
        if (mc == 0 || c < mc) mc = c;
        if (mr == 0 || r < mr) mr = r;
    }
    *out_cols = mc;
    *out_rows = mr;
}

/* 최소 크기를 실제 pane/PTY 에 적용한다. 변화가 없으면 아무 일도 하지 않는다. */
static void pane_apply_min_size(ipc_server_t *srv, ipc_pane_slot_t *slot)
{
    uint16_t cols, rows;
    pane_min_size(slot, &cols, &rows);
    if (cols == 0 || rows == 0) return;   /* 붙어 있는 클라이언트 없음 */

    session_t *s = session_find_by_id(srv->session_mgr, slot->session_id);
    if (!s) return;
    window_t *w = window_find_by_id(s, slot->window_id);
    if (!w) return;
    pane_t *p = pane_find_by_id(w, slot->pane_id);
    if (!p) return;
    if (p->cols == cols && p->rows == rows) return;

    pane_resize(p, cols, rows);
    if (slot->pty_fd >= 0) {
        pty_t pty = { .master_fd = slot->pty_fd, .child_pid = (pid_t)p->pid };
        pty_resize(&pty, cols, rows);
    }
}

/* 떠나는 클라이언트의 크기 요청을 모두 지우고 각 pane 을 재협상한다. */
static void pane_drop_client_sizes(ipc_server_t *srv, int client_idx)
{
    if (client_idx < 0 || client_idx >= IPC_MAX_CLIENTS) return;
    for (int i = 0; i < IPC_MAX_PANES; i++) {
        ipc_pane_slot_t *slot = &srv->panes[i];
        if (slot->pty_fd < 0) continue;   /* 비어 있는 슬롯 */
        if (slot->req_cols[client_idx] == 0 && slot->req_rows[client_idx] == 0)
            continue;
        slot->req_cols[client_idx] = 0;
        slot->req_rows[client_idx] = 0;
        /* 가장 작던 클라이언트가 빠졌다면 남은 클라이언트 크기로 늘어난다. */
        pane_apply_min_size(srv, slot);
    }
}

static void handle_pane_resize(ipc_server_t *srv, int client_fd,
                                const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_pane_resize_t)) {
        send_error(client_fd, IPC_MSG_PANE_RESIZE,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_pane_resize_t *req =
        (const ipc_payload_pane_resize_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_PANE_RESIZE,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }
    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) {
        send_error(client_fd, IPC_MSG_PANE_RESIZE,
                   IPC_ERR_WINDOW_NOT_FOUND, "window not found");
        return;
    }
    pane_t *p = pane_find_by_id(w, req->pane_id);
    if (!p) {
        send_error(client_fd, IPC_MSG_PANE_RESIZE,
                   IPC_ERR_PANE_NOT_FOUND, "pane not found");
        return;
    }

    /* 요청은 "이 클라이언트가 원하는 크기" 로만 기록하고, 실제 PTY 크기는
     * 붙어 있는 모든 클라이언트 요청의 최소값으로 정한다. */
    ipc_pane_slot_t *slot = pane_by_id(srv, p->id);
    if (slot) {
        int ci = client_index_by_fd(srv, client_fd);
        if (ci >= 0) {
            slot->req_cols[ci] = req->cols;
            slot->req_rows[ci] = req->rows;
        }
        pane_apply_min_size(srv, slot);
    } else {
        /* 슬롯이 아직 없으면(스폰 전) 요청값을 그대로 반영해 둔다. */
        pane_resize(p, req->cols, req->rows);
    }

    send_ok(client_fd);
}

static void handle_pane_focus(ipc_server_t *srv, int client_fd,
                               const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_pane_ref_t)) {
        send_error(client_fd, IPC_MSG_PANE_FOCUS,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_pane_ref_t *req =
        (const ipc_payload_pane_ref_t *)payload;

    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_PANE_FOCUS,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }
    window_t *w = window_find_by_id(s, req->window_id);
    if (!w) {
        send_error(client_fd, IPC_MSG_PANE_FOCUS,
                   IPC_ERR_WINDOW_NOT_FOUND, "window not found");
        return;
    }
    pane_t *p = pane_find_by_id(w, req->pane_id);
    if (!p) {
        send_error(client_fd, IPC_MSG_PANE_FOCUS,
                   IPC_ERR_PANE_NOT_FOUND, "pane not found");
        return;
    }

    pane_set_active(w, p);
    send_ok(client_fd);
}

static void handle_pty_input(ipc_server_t *srv, int client_fd,
                              const uint8_t *payload, uint16_t plen) {
    if (plen < sizeof(ipc_payload_pty_data_t)) {
        send_error(client_fd, IPC_MSG_PTY_INPUT,
                   IPC_ERR_INVALID_MSG, "payload too short");
        return;
    }
    const ipc_payload_pty_data_t *hdr =
        (const ipc_payload_pty_data_t *)payload;
    const uint8_t *data = payload + sizeof(ipc_payload_pty_data_t);
    uint16_t data_len   = hdr->data_len;

    if ((size_t)(sizeof(ipc_payload_pty_data_t) + data_len) > plen) {
        send_error(client_fd, IPC_MSG_PTY_INPUT,
                   IPC_ERR_INVALID_MSG, "data_len exceeds payload");
        return;
    }

    ipc_pane_slot_t *slot = pane_by_id(srv, hdr->pane_id);
    if (!slot) {
        send_error(client_fd, IPC_MSG_PTY_INPUT,
                   IPC_ERR_PANE_NOT_FOUND, "pane not found");
        return;
    }

    pty_t pty = { .master_fd = slot->pty_fd, .child_pid = -1 };
    pty_write(&pty, data, data_len);
    /* PTY_INPUT 은 OK 응답 없음 — 비동기 처리 */
}

/* ─── 메시지 디스패처 ────────────────────────────────────────────────────── */

/* ── SESSION_ATTACH ──────────────────────────────────────────────────────── */

static void handle_session_attach(ipc_server_t *srv, int client_fd,
                                   const uint8_t *payload, uint16_t plen)
{
    if (plen < sizeof(ipc_payload_session_attach_t)) {
        send_error(client_fd, IPC_MSG_SESSION_ATTACH,
                   IPC_ERR_INVALID_MSG, "payload too small");
        return;
    }
    const ipc_payload_session_attach_t *req =
        (const ipc_payload_session_attach_t *)payload;
    session_t *s = session_find_by_id(srv->session_mgr, req->session_id);
    if (!s) {
        send_error(client_fd, IPC_MSG_SESSION_ATTACH,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }

    /* pane 목록 수집 */
    ipc_attach_pane_info_t panes[IPC_MAX_PANES];
    int pane_count = 0;
    for (window_t *w = s->windows; w && pane_count < IPC_MAX_PANES; w = w->next) {
        for (pane_t *p = w->panes; p && pane_count < IPC_MAX_PANES; p = p->next) {
            panes[pane_count].pane_id   = p->id;
            panes[pane_count].window_id = w->id;
            panes[pane_count].cols      = p->cols;
            panes[pane_count].rows      = p->rows;
            pane_count++;
        }
    }

    /* window 목록 수집 — 각 window 의 layout blob 을 모두 동봉한다. */
    ipc_attach_window_info_t wins[IPC_MAX_WINDOWS];
    int    win_count = 0;
    size_t blob_total = 0;
    for (window_t *w = s->windows; w && win_count < IPC_MAX_WINDOWS; w = w->next) {
        uint16_t bl = w->layout_blob ? w->layout_blob_len : 0;
        /* 페이로드 상한(UINT16_MAX)을 넘길 blob 은 싣지 않는다 — 해당 window 는
         * 클라이언트에서 flat split 로 폴백된다. */
        if (blob_total + bl > IPC_ATTACH_BLOB_MAX) bl = 0;
        memset(&wins[win_count], 0, sizeof(wins[0]));
        wins[win_count].window_id = w->id;
        strncpy(wins[win_count].name, w->name, sizeof(wins[0].name) - 1);
        wins[win_count].blob_len = bl;
        blob_total += bl;
        win_count++;
    }

    ipc_payload_session_attach_r_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.session_id       = s->id;
    resp.pane_count       = (uint32_t)pane_count;
    resp.window_count     = (uint32_t)win_count;
    resp.active_window_id = s->active_window ? s->active_window->id : 0;
    strncpy(resp.session_name, s->name, sizeof(resp.session_name) - 1);

    size_t pane_sz = (size_t)pane_count * sizeof(ipc_attach_pane_info_t);
    size_t win_sz  = (size_t)win_count  * sizeof(ipc_attach_window_info_t);
    uint16_t total = (uint16_t)(sizeof(resp) + pane_sz + win_sz + blob_total);
    ipc_msg_header_t hdr = IPC_HEADER_INIT(IPC_MSG_SESSION_ATTACH_R, total);

    uint8_t buf[sizeof(hdr) + sizeof(resp) + sizeof(panes) + sizeof(wins)
                + IPC_ATTACH_BLOB_MAX];
    size_t off = 0;
    memcpy(buf + off, &hdr,  sizeof(hdr));  off += sizeof(hdr);
    memcpy(buf + off, &resp, sizeof(resp)); off += sizeof(resp);
    memcpy(buf + off, panes, pane_sz);      off += pane_sz;
    memcpy(buf + off, wins,  win_sz);       off += win_sz;
    /* blob 들은 window_info 배열과 같은 순서로 연접한다. */
    {
        int i = 0;
        for (window_t *w = s->windows; w && i < win_count; w = w->next, i++) {
            if (wins[i].blob_len > 0 && w->layout_blob) {
                memcpy(buf + off, w->layout_blob, wins[i].blob_len);
                off += wins[i].blob_len;
            }
        }
    }
    write_all_retry(client_fd, buf, off);

    /* 이 클라이언트를 세션에 attach 상태로 기록 (수명 관리용) */
    client_set_session(srv, client_by_fd(srv, client_fd), s->id);

    /* replay는 별도 PANE_REPLAY 메시지로 요청 (pane_slot 생성 후) */
}

/* ── PANE_REPLAY — 링 버퍼 재전송 ────────────────────────────────────────── */

/* ── SESSION_SAVE — 세션 스냅샷 (JSON) ───────────────────────────────────── */

static void handle_session_save(ipc_server_t *srv, int client_fd,
                                 const uint8_t *payload, uint16_t plen)
{
    if (plen < sizeof(uint32_t)) {
        send_error(client_fd, IPC_MSG_SESSION_SAVE,
                   IPC_ERR_INVALID_MSG, "payload too small");
        return;
    }
    uint32_t sid = *(const uint32_t *)payload;
    session_t *s = session_find_by_id(srv->session_mgr, sid);
    if (!s) {
        send_error(client_fd, IPC_MSG_SESSION_SAVE,
                   IPC_ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }

    /* session_snapshot_t 생성 */
    session_snapshot_t snap;
    memset(&snap, 0, sizeof snap);
    strncpy(snap.name, s->name, SNAP_NAME_MAX - 1);

    int wi = 0;
    int aw_idx = 0;
    for (window_t *w = s->windows; w && wi < SNAP_MAX_WINDOWS; w = w->next, wi++) {
        snap_window_t *sw = &snap.windows[wi];
        strncpy(sw->name, w->name, SNAP_NAME_MAX - 1);
        if (w == s->active_window) aw_idx = wi;

        int pi = 0;
        int ap_idx = 0;
        for (pane_t *p = w->panes; p && pi < SNAP_MAX_PANES; p = p->next, pi++) {
            snap_pane_t *sp = &sw->panes[pi];
            sp->cols = p->cols;
            sp->rows = p->rows;
            if (p == w->active_pane) ap_idx = pi;

            /* /proc/<pid>/cwd로 작업 디렉토리 읽기 */
            if (p->pid > 0) {
                char proc_path[64];
                snprintf(proc_path, sizeof proc_path, "/proc/%d/cwd", p->pid);
                ssize_t len = readlink(proc_path, sp->cwd, SNAP_CWD_MAX - 1);
                if (len > 0) sp->cwd[len] = '\0';
            }
        }
        sw->pane_count = pi;
        sw->active_pane = ap_idx;
    }
    snap.window_count = wi;
    snap.active_window = aw_idx;

    /* 임시 파일에 JSON 직렬화 후 읽어서 전송 */
    char tmp_path[64];
    snprintf(tmp_path, sizeof tmp_path, "/tmp/tessera-snap-%d.json", (int)getpid());
    if (session_snapshot_save(tmp_path, &snap) != 0) {
        send_error(client_fd, IPC_MSG_SESSION_SAVE,
                   IPC_ERR_UNKNOWN, "snapshot serialize failed");
        return;
    }
    FILE *fp = fopen(tmp_path, "r");
    if (!fp) {
        send_error(client_fd, IPC_MSG_SESSION_SAVE,
                   IPC_ERR_UNKNOWN, "snapshot read failed");
        unlink(tmp_path);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long json_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (json_len <= 0 || json_len > IPC_MAX_PAYLOAD_LEN) {
        fclose(fp);
        unlink(tmp_path);
        send_error(client_fd, IPC_MSG_SESSION_SAVE,
                   IPC_ERR_UNKNOWN, "snapshot too large");
        return;
    }
    char *json_buf = malloc((size_t)json_len);
    if (!json_buf) { fclose(fp); unlink(tmp_path); return; }
    fread(json_buf, 1, (size_t)json_len, fp);
    fclose(fp);
    unlink(tmp_path);

    ipc_msg_header_t hdr = IPC_HEADER_INIT(IPC_MSG_SESSION_SAVE_R, (uint16_t)json_len);
    write(client_fd, &hdr, sizeof hdr);
    write(client_fd, json_buf, (size_t)json_len);
    free(json_buf);
}

/* 재시도 포함 non-blocking 완전 write. 실패 시 -1. */
static int write_all_retry(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, p + sent, len - sent);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            if (poll(&pf, 1, 500) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void handle_pane_replay(ipc_server_t *srv, int client_fd,
                                const uint8_t *payload, uint16_t plen)
{
    if (plen < sizeof(uint32_t)) return;
    uint32_t pane_id = *(const uint32_t *)payload;

    /* pty_fd 상태와 무관하게 pane_id 매치 (클라이언트가 남은 버퍼라도 받을 수 있게). */
    ipc_pane_slot_t *ps = NULL;
    for (int j = 0; j < IPC_MAX_PANES; j++) {
        if (srv->panes[j].pane_id == pane_id && srv->panes[j].ring_count > 0) {
            ps = &srv->panes[j];
            break;
        }
    }
    if (!ps) {
        ipc_msg_header_t ok = IPC_HEADER_INIT(IPC_MSG_OK, 0);
        write_all_retry(client_fd, &ok, sizeof(ok));
        return;
    }

    size_t start = (ps->ring_head + PANE_RING_SIZE - ps->ring_count) % PANE_RING_SIZE;
    size_t remaining = ps->ring_count;
    size_t total_sent = 0;

    while (remaining > 0) {
        size_t chunk = remaining > IPC_PTY_CHUNK_MAX ? IPC_PTY_CHUNK_MAX : remaining;
        uint8_t combined[sizeof(ipc_msg_header_t) + sizeof(ipc_payload_pty_data_t) + IPC_PTY_CHUNK_MAX];
        ipc_payload_pty_data_t dh = { .pane_id = pane_id, .data_len = (uint16_t)chunk };
        uint16_t msg_len = (uint16_t)(sizeof(dh) + chunk);
        ipc_msg_header_t mh = IPC_HEADER_INIT(IPC_MSG_PTY_OUTPUT, msg_len);
        memcpy(combined, &mh, sizeof(mh));
        memcpy(combined + sizeof(mh), &dh, sizeof(dh));
        for (size_t k = 0; k < chunk; k++)
            combined[sizeof(mh) + sizeof(dh) + k] = ps->ring[(start + k) % PANE_RING_SIZE];

        if (write_all_retry(client_fd, combined,
                            sizeof(mh) + sizeof(dh) + chunk) < 0)
            return;
        total_sent += chunk;
        start = (start + chunk) % PANE_RING_SIZE;
        remaining -= chunk;
    }
    (void)total_sent;

    /* 완료 마커 */
    ipc_msg_header_t ok = IPC_HEADER_INIT(IPC_MSG_OK, 0);
    write_all_retry(client_fd, &ok, sizeof(ok));
}

static void dispatch_message(ipc_server_t *srv, int client_fd,
                              const ipc_msg_header_t *hdr,
                              const uint8_t *payload) {
    switch ((ipc_msg_type_t)hdr->type) {
        case IPC_MSG_HELLO:
            handle_hello(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_BYE:
            /* 연결 종료는 이벤트 루프에서 처리 */
            break;
        case IPC_MSG_SESSION_CREATE:
            handle_session_create(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_SESSION_DESTROY:
            handle_session_destroy(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_SESSION_LIST:
            handle_session_list(srv, client_fd);
            break;
        case IPC_MSG_SESSION_ATTACH:
            handle_session_attach(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_PANE_REPLAY:
            handle_pane_replay(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_SESSION_SAVE:
            handle_session_save(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_WINDOW_CREATE:
            handle_window_create(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_WINDOW_DESTROY:
            handle_window_destroy(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_WINDOW_FOCUS:
            handle_window_focus(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_WINDOW_LAYOUT:
            handle_window_layout(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_PANE_CREATE:
            handle_pane_create(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_PANE_DESTROY:
            handle_pane_destroy(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_PANE_RESIZE:
            handle_pane_resize(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_PANE_FOCUS:
            handle_pane_focus(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_PTY_INPUT:
            handle_pty_input(srv, client_fd, payload, hdr->payload_len);
            break;
        default:
            send_error(client_fd, (ipc_msg_type_t)hdr->type,
                       IPC_ERR_INVALID_MSG, "unknown message type");
            break;
    }
}

/* ─── 클라이언트 데이터 수신 ─────────────────────────────────────────────── */

static void client_remove(ipc_server_t *srv, ipc_client_slot_t *c) {
    /* 이 클라이언트의 크기 요청을 걷어내고 남은 클라이언트 기준으로 재협상.
     * (가장 작던 클라이언트가 나가면 pane 이 다시 커진다.) */
    pane_drop_client_sizes(srv, (int)(c - srv->clients));
    /* 세션 attach 해제 (last_detach_ms 갱신) */
    client_set_session(srv, c, 0);
    loop_del(srv, c->fd);
    close(c->fd);
    c->fd       = -1;
    c->rbuf_len = 0;
}

static void client_read(ipc_server_t *srv, ipc_client_slot_t *c) {
    for (;;) {
        size_t space = sizeof(c->rbuf) - c->rbuf_len;
        if (space == 0) break;  /* 버퍼 꽉 참 */

        ssize_t n = read(c->fd, c->rbuf + c->rbuf_len, space);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            client_remove(srv, c);
            return;
        }
        if (n == 0) {
            /* 연결 종료 */
            client_remove(srv, c);
            return;
        }
        c->rbuf_len += (size_t)n;
    }

    /* 버퍼에서 완전한 메시지 추출 */
    while (c->rbuf_len >= sizeof(ipc_msg_header_t)) {
        const ipc_msg_header_t *hdr = (const ipc_msg_header_t *)c->rbuf;
        size_t total = sizeof(ipc_msg_header_t) + hdr->payload_len;

        if (c->rbuf_len < total) break;  /* 페이로드 아직 미도착 */

        const uint8_t *payload = c->rbuf + sizeof(ipc_msg_header_t);
        dispatch_message(srv, c->fd, hdr, payload);

        /* 처리된 부분 제거 */
        size_t remaining = c->rbuf_len - total;
        if (remaining > 0)
            memmove(c->rbuf, c->rbuf + total, remaining);
        c->rbuf_len = remaining;
    }
}

/* ─── PTY 출력 전달 ──────────────────────────────────────────────────────── */

static void pty_output_read(ipc_server_t *srv, int pty_fd) {
    ipc_pane_slot_t *slot = pane_by_pty_fd(srv, pty_fd);
    if (!slot) return;

    uint8_t chunk[IPC_PTY_CHUNK_MAX];
    pty_t pty = { .master_fd = pty_fd, .child_pid = -1 };

    int pty_eof = 0;
    for (;;) {
        ssize_t n = pty_read(&pty, chunk, sizeof(chunk));
        if (n == 0) break;          /* EAGAIN — 현재 읽을 데이터 없음, 정상 */
        if (n < 0)  { pty_eof = 1; break; }  /* EIO — 셸 종료, 실제 EOF */

        /* 링 버퍼에 기록 (attach 시 replay 용) */
        for (ssize_t j = 0; j < n; j++) {
            slot->ring[slot->ring_head] = chunk[j];
            slot->ring_head = (slot->ring_head + 1) % PANE_RING_SIZE;
            if (slot->ring_count < PANE_RING_SIZE) slot->ring_count++;
        }

        /* ipc_payload_pty_data_t 헤더 + 데이터 조합 전송 */
        ipc_payload_pty_data_t data_hdr;
        data_hdr.pane_id  = slot->pane_id;
        data_hdr.data_len = (uint16_t)n;

        /* 모든 연결된 클라이언트에 브로드캐스트 (단일 write로 atomic 전송) */
        {
            ipc_msg_header_t hdr = IPC_HEADER_INIT(
                IPC_MSG_PTY_OUTPUT,
                (uint16_t)(sizeof(data_hdr) + n));

            uint8_t msg[sizeof(hdr) + sizeof(data_hdr) + IPC_PTY_CHUNK_MAX];
            memcpy(msg, &hdr, sizeof(hdr));
            memcpy(msg + sizeof(hdr), &data_hdr, sizeof(data_hdr));
            memcpy(msg + sizeof(hdr) + sizeof(data_hdr), chunk, (size_t)n);
            size_t msg_len = sizeof(hdr) + sizeof(data_hdr) + (size_t)n;

            for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
                int cfd = srv->clients[i].fd;
                if (cfd < 0) continue;

                /* 재시도 루프: non-blocking 소켓에서 partial write 처리 */
                size_t sent = 0;
                while (sent < msg_len) {
                    ssize_t w = write(cfd, msg + sent, msg_len - sent);
                    if (w > 0) { sent += (size_t)w; continue; }
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        /* 잠깐 대기 후 재시도 (bridge/SSH 느린 경우) */
                        struct pollfd pf = { cfd, POLLOUT, 0 };
                        if (poll(&pf, 1, 100) <= 0) break; /* 100ms 타임아웃 */
                        continue;
                    }
                    break; /* 에러 */
                }
            }
        }
    }

    /* PTY EOF — 셸이 종료됨: 클라이언트에 알리고 pane 정리 */
    if (pty_eof) {
        ipc_payload_pane_ref_t ref = { .session_id = slot->session_id,
                                        .window_id  = slot->window_id,
                                        .pane_id    = slot->pane_id };
        ipc_msg_header_t notify = IPC_HEADER_INIT(IPC_MSG_PANE_EXITED,
                                                    sizeof(ref));
        for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
            int cfd = srv->clients[i].fd;
            if (cfd < 0) continue;
            write(cfd, &notify, sizeof(notify));
            write(cfd, &ref,    sizeof(ref));
        }

        /* epoll 제거 + pty 닫기 */
        loop_del(srv, slot->pty_fd);
        pty_close(&pty, NULL);
        slot->pty_fd = -1;

        /* session 트리에서 pane 제거 */
        session_t *s = session_find_by_id(srv->session_mgr, slot->session_id);
        window_t  *w = s ? window_find_by_id(s, slot->window_id) : NULL;
        pane_t    *p = w ? pane_find_by_id(w, slot->pane_id)    : NULL;
        if (p) pane_destroy(w, p);

        /* window 의 마지막 pane 이었으면 빈 window 도 제거한다.
         * 남겨두면 window_count 와 스냅샷에 빈 window 가 쌓인다. */
        if (w && w->pane_count == 0 && s && s->window_count > 0)
            window_destroy(s, w);

        /* 세션의 마지막 pane 이었으면 세션 자체도 제거 (useless empty session 방지) */
        if (s) session_destroy_if_empty(srv, s);
    }
}

/* ─── 공개 API ───────────────────────────────────────────────────────────── */

ipc_server_t *ipc_server_create(session_manager_t *session_mgr) {
    if (!session_mgr) return NULL;

    ipc_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->listen_fd   = -1;
    srv->evloop      = NULL;
    srv->session_mgr = session_mgr;
    srv->running     = 0;
    srv->autosave_interval_sec    = IPC_SERVER_DEFAULT_AUTOSAVE_SEC;
    srv->session_idle_timeout_sec = IPC_SERVER_DEFAULT_IDLE_SEC;

    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        srv->clients[i].fd         = -1;
        srv->clients[i].session_id = 0;
    }
    for (int i = 0; i < IPC_MAX_PANES;   i++) srv->panes[i].pty_fd  = -1;

    return srv;
}

int ipc_server_listen(ipc_server_t *srv) {
    if (!srv) return -1;

    if (ipc_socket_path(srv->sock_path, sizeof(srv->sock_path)) < 0)
        return -1;

    srv->listen_fd = ipc_listen_socket(srv->sock_path);
    if (srv->listen_fd < 0) return -1;

    srv->evloop = evloop_create();
    if (!srv->evloop) {
        ipc_close_socket(srv->listen_fd, srv->sock_path);
        srv->listen_fd = -1;
        return -1;
    }

    /* listen 소켓은 레벨 트리거(EV_READ)로 감시 — accept 루프 단순화. */
    if (loop_add(srv, srv->listen_fd, EV_READ) < 0) {
        evloop_destroy(srv->evloop);
        ipc_close_socket(srv->listen_fd, srv->sock_path);
        srv->listen_fd = -1;
        srv->evloop = NULL;
        return -1;
    }

    return 0;
}

/* 단일 스냅샷을 기반으로 session/window/pane 구조를 재건하고 PTY 를 스폰한다. */
static int restore_one_snapshot(ipc_server_t *srv, const session_snapshot_t *snap)
{
    if (snap->window_count == 0) return -1;

    /* 같은 이름의 세션이 이미 있으면 (데몬이 살아있는 동안 저장된 경우) 건너뜀 */
    if (session_find_by_name(srv->session_mgr, snap->name)) return -1;

    session_t *s = session_create(srv->session_mgr, snap->name);
    if (!s) return -1;

    for (int wi = 0; wi < snap->window_count; wi++) {
        const snap_window_t *sw = &snap->windows[wi];
        window_t *w = window_create(s, sw->name);
        if (!w) continue;

        for (int pi = 0; pi < sw->pane_count; pi++) {
            const snap_pane_t *sp = &sw->panes[pi];
            uint16_t cols = sp->cols > 0 ? sp->cols : 80;
            uint16_t rows = sp->rows > 0 ? sp->rows : 24;

            ipc_pane_slot_t *slot = pane_empty_slot(srv);
            if (!slot) break;

            pane_t *p = pane_create(w, cols, rows);
            if (!p) break;

            pty_t pty;
            if (pty_spawn(&pty, NULL, cols, rows) < 0) {
                pane_destroy(w, p);
                continue;
            }
            p->pty_fd = pty.master_fd;
            p->pid    = (int)pty.child_pid;
            slot->pty_fd     = pty.master_fd;
            slot->pane_id    = p->id;
            slot->session_id = s->id;
            slot->window_id  = w->id;
            slot->ring_head  = 0;
            slot->ring_count = 0;

            loop_add(srv, pty.master_fd, EV_READ | EV_EDGE);

            /* cwd 복원: 셸 프롬프트가 준비된 후 cd + clear 주입 */
            if (sp->cwd[0]) {
                char cmd[SNAP_CWD_MAX + 16];
                int n = snprintf(cmd, sizeof cmd, "cd %s\nclear\n", sp->cwd);
                if (n > 0) pty_write(&pty, (const uint8_t *)cmd, (size_t)n);
            }
        }
    }
    return 0;
}

/* ~/.config/tessera/sessions/<name>.json 경로 조립. home 없으면 -1. */
static int snapshot_path(const char *sess_name, char *out, size_t out_size)
{
    const char *home = getenv("HOME");
    if (!home) return -1;
    int n = snprintf(out, out_size, "%s/.config/tessera/sessions/%s.json",
                     home, sess_name);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

/* 세션이 영구 소멸 시 디스크 스냅샷도 제거 (재시작 후 복원 방지). */
static void delete_session_snapshot(const char *sess_name)
{
    char p[512];
    if (snapshot_path(sess_name, p, sizeof p) == 0)
        unlink(p);
}

/* 모든 활성 세션을 디스크에 저장한다. auto-save 와 shutdown-save 공용. */
static void save_all_sessions(ipc_server_t *srv)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[256];
    snprintf(dir, sizeof dir, "%s/.config/tessera/sessions", home);
    mkdir(dir, 0755);
    for (session_t *s = srv->session_mgr->head; s; s = s->next) {
        session_snapshot_t snap;
        memset(&snap, 0, sizeof snap);
        strncpy(snap.name, s->name, SNAP_NAME_MAX - 1);
        int wi = 0;
        for (window_t *w = s->windows; w && wi < SNAP_MAX_WINDOWS; w = w->next, wi++) {
            snap_window_t *sw = &snap.windows[wi];
            strncpy(sw->name, w->name, SNAP_NAME_MAX - 1);
            int pi = 0;
            for (pane_t *p = w->panes; p && pi < SNAP_MAX_PANES; p = p->next, pi++) {
                sw->panes[pi].cols = p->cols;
                sw->panes[pi].rows = p->rows;
                if (p->pid > 0) {
                    char pp[64];
                    snprintf(pp, sizeof pp, "/proc/%d/cwd", p->pid);
                    ssize_t l = readlink(pp, sw->panes[pi].cwd, SNAP_CWD_MAX - 1);
                    if (l > 0) sw->panes[pi].cwd[l] = '\0';
                }
            }
            sw->pane_count = pi;
        }
        snap.window_count = wi;
        char fpath[512];
        snprintf(fpath, sizeof fpath, "%s/%s.json", dir, s->name);
        session_snapshot_save(fpath, &snap);
    }
}

int ipc_server_restore_sessions(ipc_server_t *srv)
{
    if (!srv) return 0;

    const char *home = getenv("HOME");
    if (!home) return 0;
    char dir[512];
    snprintf(dir, sizeof dir, "%s/.config/tessera/sessions", home);

    DIR *d = opendir(dir);
    if (!d) return 0;

    int restored = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen < 6 || strcmp(nm + nlen - 5, ".json") != 0) continue;

        char fpath[1024];
        snprintf(fpath, sizeof fpath, "%s/%s", dir, nm);

        session_snapshot_t snap;
        memset(&snap, 0, sizeof snap);
        if (session_snapshot_load(fpath, &snap) != 0) continue;

        if (restore_one_snapshot(srv, &snap) == 0) {
            restored++;
            fprintf(stderr, "[tessera-daemon] restored session '%s' (%d windows)\n",
                    snap.name, snap.window_count);
        }
    }
    closedir(d);

    /* 세션이 복원되었으면 ever_had_client 를 올려 "빈 데몬 자동 종료" 를 유예.
     * 클라이언트 한 번도 안 붙어도 auto-shutdown 되지 않도록. */
    if (restored > 0) srv->ever_had_client = 1;
    return restored;
}

int ipc_server_run(ipc_server_t *srv) {
    if (!srv || srv->listen_fd < 0 || !srv->evloop) return -1;

    srv->running = 1;
    ev_ready_t events[IPC_LOOP_EVENTS];

    while (srv->running) {
        int nev = evloop_wait(srv->evloop, events, IPC_LOOP_EVENTS, 200);
        if (nev < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        for (int i = 0; i < nev; i++) {
            int fd = events[i].fd;

            if (fd == srv->listen_fd) {
                /* 신규 클라이언트 연결 */
                int cfd = ipc_accept_client(srv->listen_fd);
                if (cfd < 0) continue;

                ipc_client_slot_t *slot = client_empty_slot(srv);
                if (!slot) {
                    close(cfd);
                    continue;
                }
                slot->fd         = cfd;
                slot->session_id = 0;
                slot->rbuf_len   = 0;
                srv->ever_had_client = 1;
                loop_add(srv, cfd, EV_READ | EV_EDGE);

            } else {
                /* PTY 또는 클라이언트 I/O. HUP/ERR 도 데이터 drain + EOF 정리 경로로
                 * 흘려보낸다 — 셸 exit 시 HANGUP 만 오고 READ 가 재트리거되지
                 * 않아 PANE_EXITED 브로드캐스트가 누락되던 버그 수정. */
                ipc_client_slot_t *c = client_by_fd(srv, fd);
                if (c) {
                    if (events[i].revents & EV_READ)
                        client_read(srv, c);
                    if (events[i].revents & (EV_HANGUP | EV_ERROR))
                        client_remove(srv, c);
                } else {
                    /* PTY fd: pty_output_read 가 pty_read 의 EIO/EAGAIN 를 보고
                     * EOF 여부를 판별, PANE_EXITED 브로드캐스트 + pane_destroy 까지
                     * 일괄 처리한다. HUP/ERR 만 와도 한 번 호출하면 정리됨. */
                    pty_output_read(srv, fd);
                }
            }
        }

        /* ── 자동 저장 (설정값 주기) ── */
        if (srv->autosave_interval_sec > 0) {
            static time_t last_save = 0;
            if (last_save == 0) last_save = time(NULL);
            time_t now = time(NULL);
            if ((now - last_save) >= srv->autosave_interval_sec) {
                last_save = now;
                save_all_sessions(srv);
            }
        }

        /* ── 세션 수명 정책 스윕 ──
         * 모든 클라이언트가 detach 된 후 session_idle_timeout_sec 가 경과하면
         * 세션을 자동으로 destroy 한다. */
        {
            int64_t now_ms = mono_ms();
            int64_t idle_ms = (int64_t)srv->session_idle_timeout_sec * 1000;
            session_t *s = srv->session_mgr->head;
            while (s) {
                session_t *next = s->next;
                if (s->attach_count == 0 && s->last_detach_ms != 0 &&
                    (now_ms - s->last_detach_ms) >= idle_ms) {
                    /* 세션 내 모든 pane 의 PTY fd 를 epoll 에서 제거하고 닫는다 */
                    for (window_t *w = s->windows; w; w = w->next) {
                        for (pane_t *p = w->panes; p; p = p->next) {
                            ipc_pane_slot_t *slot = pane_by_id(srv, p->id);
                            if (slot && slot->pty_fd >= 0) {
                                loop_del(srv, slot->pty_fd);
                                pty_t pt = { .master_fd = slot->pty_fd,
                                             .child_pid = (pid_t)p->pid };
                                pty_close(&pt, NULL);
                                slot->pty_fd = -1;
                            }
                        }
                    }
                    delete_session_snapshot(s->name);
                    session_destroy(srv->session_mgr, s);
                }
                s = next;
            }
        }

        /* 모든 세션 소멸 시 자동 종료 — 단, 한 번이라도 클라이언트가 연결된 후에만 */
        if (srv->ever_had_client && srv->session_mgr->count == 0) {
            int has_clients = 0;
            for (int i = 0; i < IPC_MAX_CLIENTS; i++)
                if (srv->clients[i].fd >= 0) { has_clients = 1; break; }
            if (!has_clients)
                srv->running = 0;
        }
    }

    return 0;
}

void ipc_server_shutdown(ipc_server_t *srv) {
    if (srv) srv->running = 0;
}

void ipc_server_configure(ipc_server_t *srv,
                           int autosave_interval_sec,
                           int session_idle_timeout_sec)
{
    if (!srv) return;
    /* 음수는 "기본값 유지". 0 은 autosave 비활성/세션 즉시 destroy 의 의미로 허용. */
    if (autosave_interval_sec >= 0)
        srv->autosave_interval_sec = autosave_interval_sec;
    if (session_idle_timeout_sec >= 0)
        srv->session_idle_timeout_sec = session_idle_timeout_sec;
}

void ipc_server_destroy(ipc_server_t *srv) {
    if (!srv) return;

    /* 종료 직전 스냅샷 저장 (크래시 복구 데이터 최신화) */
    save_all_sessions(srv);

    /* 클라이언트 fd 정리 */
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd >= 0) {
            close(srv->clients[i].fd);
            srv->clients[i].fd = -1;
        }
    }

    /* PTY fd 정리 */
    for (int i = 0; i < IPC_MAX_PANES; i++) {
        if (srv->panes[i].pty_fd >= 0) {
            close(srv->panes[i].pty_fd);
            srv->panes[i].pty_fd = -1;
        }
    }

    if (srv->evloop)         { evloop_destroy(srv->evloop); srv->evloop = NULL; }
    if (srv->listen_fd >= 0) ipc_close_socket(srv->listen_fd, srv->sock_path);

    free(srv);
}
