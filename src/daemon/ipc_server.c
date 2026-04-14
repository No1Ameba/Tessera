#define _GNU_SOURCE

#include "ipc_server.h"
#include "session.h"
#include "../common/ipc_proto.h"
#include "../platform/ipc.h"
#include "../platform/termemu_pty.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* ─── 상수 ───────────────────────────────────────────────────────────────── */

#define IPC_MAX_CLIENTS   16
#define IPC_MAX_PANES     64
#define IPC_EPOLL_EVENTS  32

/* 수신 버퍼: 헤더 + 최대 페이로드 */
#define IPC_RBUF_SIZE  (sizeof(ipc_msg_header_t) + IPC_MAX_PAYLOAD_LEN)

/* ─── 내부 타입 ──────────────────────────────────────────────────────────── */

typedef struct {
    int     fd;           /* -1: 빈 슬롯 */
    uint8_t rbuf[IPC_RBUF_SIZE];
    size_t  rbuf_len;
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
} ipc_pane_slot_t;

struct ipc_server {
    int                 listen_fd;
    int                 epoll_fd;
    char                sock_path[IPC_SOCKET_PATH_MAX];
    session_manager_t  *session_mgr;
    ipc_client_slot_t   clients[IPC_MAX_CLIENTS];
    ipc_pane_slot_t     panes[IPC_MAX_PANES];
    volatile int        running;
    int                 ever_had_client;  /* 첫 클라이언트 연결 후 1로 설정 */
};

/* ─── 헬퍼: fd → 슬롯 조회 ───────────────────────────────────────────────── */

static ipc_client_slot_t *client_by_fd(ipc_server_t *srv, int fd) {
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
        if (srv->clients[i].fd == fd) return &srv->clients[i];
    return NULL;
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
    for (int i = 0; i < IPC_MAX_PANES; i++)
        if (srv->panes[i].pty_fd < 0) return &srv->panes[i];
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

/* ─── 헬퍼: epoll 등록/해제 ──────────────────────────────────────────────── */

static int epoll_add(ipc_server_t *srv, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    return epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_del(ipc_server_t *srv, int fd) {
    return epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
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
                epoll_del(srv, slot->pty_fd);
                slot->pty_fd = -1;
            }
        }
    }

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
            epoll_del(srv, slot->pty_fd);
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
    epoll_add(srv, pty.master_fd, EPOLLIN | EPOLLET);

    ipc_payload_pane_created_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.session_id = s->id;
    resp.window_id  = w->id;
    resp.pane_id    = p->id;
    resp.pid        = (int32_t)pty.child_pid;
    send_msg(client_fd, IPC_MSG_PANE_CREATED, &resp, sizeof(resp));
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
        epoll_del(srv, slot->pty_fd);
        pty_t pty = { .master_fd = slot->pty_fd, .child_pid = (pid_t)p->pid };
        pty_close(&pty, NULL);
        slot->pty_fd = -1;
    }

    pane_destroy(w, p);
    send_ok(client_fd);
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

    pane_resize(p, req->cols, req->rows);

    ipc_pane_slot_t *slot = pane_by_id(srv, p->id);
    if (slot && slot->pty_fd >= 0) {
        pty_t pty = { .master_fd = slot->pty_fd, .child_pid = (pid_t)p->pid };
        pty_resize(&pty, req->cols, req->rows);
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

    /* 응답 전송: 헤더 + pane 배열 */
    ipc_payload_session_attach_r_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.session_id = s->id;
    resp.pane_count = (uint32_t)pane_count;
    strncpy(resp.session_name, s->name, sizeof(resp.session_name) - 1);

    size_t arr_sz = (size_t)pane_count * sizeof(ipc_attach_pane_info_t);
    uint16_t total = (uint16_t)(sizeof(resp) + arr_sz);
    uint8_t buf[sizeof(ipc_msg_header_t) + sizeof(resp) + sizeof(panes)];
    ipc_msg_header_t hdr = IPC_HEADER_INIT(IPC_MSG_SESSION_ATTACH_R, total);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &resp, sizeof(resp));
    memcpy(buf + sizeof(hdr) + sizeof(resp), panes, arr_sz);
    write(client_fd, buf, sizeof(hdr) + total);

    /* PTY 링 버퍼 replay는 여기서 하지 않는다.
     * 이유: recv_until()이 ATTACH_R을 기다리는 동안 PTY_OUTPUT이 도착하면
     * 클라이언트의 pane_slot이 아직 생성 전이라 데이터가 드롭된다.
     *
     * 대신 클라이언트가 pane_slot 생성 후 PANE_RESIZE를 보내면
     * SIGWINCH로 원격 앱이 화면을 다시 그린다. */
    (void)0; /* 링 버퍼 인프라는 유지 — 향후 별도 replay 메시지로 활용 */
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
        case IPC_MSG_WINDOW_CREATE:
            handle_window_create(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_WINDOW_DESTROY:
            handle_window_destroy(srv, client_fd, payload, hdr->payload_len);
            break;
        case IPC_MSG_WINDOW_FOCUS:
            handle_window_focus(srv, client_fd, payload, hdr->payload_len);
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
    epoll_del(srv, c->fd);
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

        /* 모든 연결된 클라이언트에 브로드캐스트 */
        for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
            int cfd = srv->clients[i].fd;
            if (cfd < 0) continue;

            ipc_msg_header_t hdr = IPC_HEADER_INIT(
                IPC_MSG_PTY_OUTPUT,
                (uint16_t)(sizeof(data_hdr) + n));

            write(cfd, &hdr,      sizeof(hdr));
            write(cfd, &data_hdr, sizeof(data_hdr));
            write(cfd, chunk,     (size_t)n);
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
        epoll_del(srv, slot->pty_fd);
        pty_close(&pty, NULL);
        slot->pty_fd = -1;

        /* session 트리에서 pane 제거 */
        session_t *s = session_find_by_id(srv->session_mgr, slot->session_id);
        window_t  *w = s ? window_find_by_id(s, slot->window_id) : NULL;
        pane_t    *p = w ? pane_find_by_id(w, slot->pane_id)    : NULL;
        if (p) pane_destroy(w, p);
    }
}

/* ─── 공개 API ───────────────────────────────────────────────────────────── */

ipc_server_t *ipc_server_create(session_manager_t *session_mgr) {
    if (!session_mgr) return NULL;

    ipc_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->listen_fd   = -1;
    srv->epoll_fd    = -1;
    srv->session_mgr = session_mgr;
    srv->running     = 0;

    for (int i = 0; i < IPC_MAX_CLIENTS; i++) srv->clients[i].fd    = -1;
    for (int i = 0; i < IPC_MAX_PANES;   i++) srv->panes[i].pty_fd  = -1;

    return srv;
}

int ipc_server_listen(ipc_server_t *srv) {
    if (!srv) return -1;

    if (ipc_socket_path(srv->sock_path, sizeof(srv->sock_path)) < 0)
        return -1;

    srv->listen_fd = ipc_listen_socket(srv->sock_path);
    if (srv->listen_fd < 0) return -1;

    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) {
        ipc_close_socket(srv->listen_fd, srv->sock_path);
        srv->listen_fd = -1;
        return -1;
    }

    if (epoll_add(srv, srv->listen_fd, EPOLLIN) < 0) {
        close(srv->epoll_fd);
        ipc_close_socket(srv->listen_fd, srv->sock_path);
        srv->listen_fd = srv->epoll_fd = -1;
        return -1;
    }

    return 0;
}

int ipc_server_run(ipc_server_t *srv) {
    if (!srv || srv->listen_fd < 0 || srv->epoll_fd < 0) return -1;

    srv->running = 1;
    struct epoll_event events[IPC_EPOLL_EVENTS];

    while (srv->running) {
        int nev = epoll_wait(srv->epoll_fd, events, IPC_EPOLL_EVENTS, 200);
        if (nev < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        for (int i = 0; i < nev; i++) {
            int fd = events[i].data.fd;

            if (fd == srv->listen_fd) {
                /* 신규 클라이언트 연결 */
                int cfd = ipc_accept_client(srv->listen_fd);
                if (cfd < 0) continue;

                ipc_client_slot_t *slot = client_empty_slot(srv);
                if (!slot) {
                    close(cfd);
                    continue;
                }
                slot->fd       = cfd;
                slot->rbuf_len = 0;
                srv->ever_had_client = 1;
                epoll_add(srv, cfd, EPOLLIN | EPOLLET);

            } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                /* PTY 또는 클라이언트 오류/종료 */
                ipc_client_slot_t *c = client_by_fd(srv, fd);
                if (c) {
                    client_remove(srv, c);
                } else {
                    ipc_pane_slot_t *ps = pane_by_pty_fd(srv, fd);
                    if (ps) {
                        epoll_del(srv, ps->pty_fd);
                        ps->pty_fd = -1;
                    }
                }

            } else if (events[i].events & EPOLLIN) {
                ipc_client_slot_t *c = client_by_fd(srv, fd);
                if (c) {
                    client_read(srv, c);
                } else {
                    /* PTY 출력 데이터 */
                    pty_output_read(srv, fd);
                }
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

void ipc_server_destroy(ipc_server_t *srv) {
    if (!srv) return;

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

    if (srv->epoll_fd >= 0)  close(srv->epoll_fd);
    if (srv->listen_fd >= 0) ipc_close_socket(srv->listen_fd, srv->sock_path);

    free(srv);
}
