/*
 * test_ipc.c — IPC 소켓 + 메시지 프레이밍 + 서버 핸들러 테스트
 *
 * 단독 빌드:
 *   gcc -std=c11 -Wall -I src/common -I src/platform -I src/daemon \
 *       src/platform/posix/ipc_posix.c \
 *       src/daemon/session.c \
 *       src/daemon/ipc_server.c \
 *       src/platform/posix/pty_posix.c \
 *       tests/test_ipc.c \
 *       -lutil -lpthread -o /tmp/test_ipc && /tmp/test_ipc
 */

#define _GNU_SOURCE

#include "ipc.h"
#include "ipc_proto.h"
#include "ipc_server.h"
#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ─── 미니 테스트 프레임워크 ─────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", msg); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s  (line %d)\n", msg, __LINE__); \
            g_fail++; \
        } \
    } while (0)

#define GROUP(name) printf("\n[%s]\n", name)

/* ─── 헬퍼: 클라이언트 연결 ─────────────────────────────────────────────── */

static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* 서버가 준비될 때까지 재시도 (최대 500ms) */
    for (int i = 0; i < 10; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        usleep(50 * 1000);
    }
    close(fd);
    return -1;
}

/* ─── 헬퍼: 메시지 전송/수신 ────────────────────────────────────────────── */

static int client_send(int fd, ipc_msg_type_t type,
                        const void *payload, uint16_t plen) {
    ipc_msg_header_t hdr = IPC_HEADER_INIT(type, plen);
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return -1;
    if (plen > 0 && payload)
        if (write(fd, payload, plen) != (ssize_t)plen) return -1;
    return 0;
}

static int client_recv(int fd, ipc_msg_header_t *out_hdr,
                        uint8_t *out_payload, size_t payload_max,
                        int timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0) return -1;

    if (read(fd, out_hdr, sizeof(*out_hdr)) != (ssize_t)sizeof(*out_hdr))
        return -1;

    if (out_hdr->payload_len > 0) {
        if (out_hdr->payload_len > payload_max) return -1;
        struct pollfd pfd2 = { fd, POLLIN, 0 };
        if (poll(&pfd2, 1, timeout_ms) <= 0) return -1;
        if (read(fd, out_payload, out_hdr->payload_len)
                != (ssize_t)out_hdr->payload_len)
            return -1;
    }
    return 0;
}

/* ─── 서버 스레드 ────────────────────────────────────────────────────────── */

typedef struct {
    ipc_server_t      *srv;
    session_manager_t *mgr;
} server_ctx_t;

static void *server_thread(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    ipc_server_run(ctx->srv);
    return NULL;
}

/* 테스트용 서버 구성: listen 까지 완료 후 스레드 시작 */
static ipc_server_t *start_test_server(session_manager_t *mgr,
                                        pthread_t *tid, server_ctx_t *ctx) {
    ipc_server_t *srv = ipc_server_create(mgr);
    if (!srv) return NULL;
    if (ipc_server_listen(srv) < 0) { ipc_server_destroy(srv); return NULL; }

    ctx->srv = srv;
    ctx->mgr = mgr;
    pthread_create(tid, NULL, server_thread, ctx);
    return srv;
}

/* ─── 테스트 케이스 ──────────────────────────────────────────────────────── */

static void test_socket_path(void) {
    GROUP("ipc_socket_path");

    char buf[IPC_SOCKET_PATH_MAX];
    int ret = ipc_socket_path(buf, sizeof(buf));
    ASSERT(ret == 0,        "path 생성 성공");
    ASSERT(buf[0] == '/',   "절대 경로");
    ASSERT(strstr(buf, "tessera") != NULL, "경로에 'tessera' 포함");

    char small[4];
    ret = ipc_socket_path(small, sizeof(small));
    ASSERT(ret == -1, "버퍼 너무 작으면 -1");
}

static void test_listen_accept(void) {
    GROUP("listen / connect / accept");

    char path[IPC_SOCKET_PATH_MAX];
    snprintf(path, sizeof(path), "/tmp/tessera_test_%d.sock", (int)getpid());

    int lfd = ipc_listen_socket(path);
    ASSERT(lfd >= 0, "listen_socket 성공");

    /* 클라이언트 연결 */
    int cfd_client = connect_to(path);
    ASSERT(cfd_client >= 0, "클라이언트 connect 성공");

    int cfd_server = ipc_accept_client(lfd);
    ASSERT(cfd_server >= 0, "서버 accept 성공");

    close(cfd_client);
    close(cfd_server);
    ipc_close_socket(lfd, path);
    ASSERT(access(path, F_OK) != 0, "소켓 파일 삭제됨");
}

static void test_hello_handshake(void) {
    GROUP("HELLO 핸드셰이크");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    /* 소켓 경로 얻기 */
    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));

    int cfd = connect_to(path);
    ASSERT(cfd >= 0, "클라이언트 연결");

    /* HELLO 전송 */
    ipc_payload_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.client_pid = (uint32_t)getpid();
    strncpy(hello.client_version, "0.1.0", sizeof(hello.client_version) - 1);
    client_send(cfd, IPC_MSG_HELLO, &hello, sizeof(hello));

    /* HELLO_ACK 수신 */
    ipc_msg_header_t resp_hdr;
    uint8_t resp_buf[256];
    int ret = client_recv(cfd, &resp_hdr, resp_buf, sizeof(resp_buf), 1000);
    ASSERT(ret == 0,                       "HELLO_ACK 수신");
    ASSERT(resp_hdr.type == IPC_MSG_HELLO_ACK, "타입 = HELLO_ACK");

    const ipc_payload_hello_ack_t *ack = (const ipc_payload_hello_ack_t *)resp_buf;
    ASSERT(ack->daemon_pid > 0,            "daemon_pid 유효");

    close(cfd);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

static void test_session_create_list(void) {
    GROUP("세션 생성 / 목록 조회");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));

    int cfd = connect_to(path);
    ASSERT(cfd >= 0, "연결");

    /* SESSION_CREATE */
    ipc_payload_session_create_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, "work", sizeof(req.name) - 1);
    client_send(cfd, IPC_MSG_SESSION_CREATE, &req, sizeof(req));

    ipc_msg_header_t hdr;
    uint8_t buf[512];
    int ret = client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(ret == 0,                           "SESSION_CREATED 수신");
    ASSERT(hdr.type == IPC_MSG_SESSION_CREATED, "타입 = SESSION_CREATED");

    const ipc_payload_session_created_t *created =
        (const ipc_payload_session_created_t *)buf;
    ASSERT(created->session_id > 0,            "session_id 발급");
    ASSERT(strcmp(created->name, "work") == 0, "이름 일치");

    uint32_t session_id = created->session_id;

    /* SESSION_LIST */
    client_send(cfd, IPC_MSG_SESSION_LIST, NULL, 0);
    ret = client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(ret == 0,                          "SESSION_LIST_R 수신");
    ASSERT(hdr.type == IPC_MSG_SESSION_LIST_R, "타입 = SESSION_LIST_R");
    ASSERT(hdr.payload_len == sizeof(ipc_session_info_t), "세션 1개 목록");

    const ipc_session_info_t *info = (const ipc_session_info_t *)buf;
    ASSERT(info->session_id == session_id, "목록 id 일치");

    /* 중복 이름 → ERROR */
    client_send(cfd, IPC_MSG_SESSION_CREATE, &req, sizeof(req));
    ret = client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(ret == 0,                   "중복 이름 ERROR 수신");
    ASSERT(hdr.type == IPC_MSG_ERROR,  "타입 = ERROR");

    close(cfd);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

static void test_window_pane_create(void) {
    GROUP("윈도우 / 페인 생성 + PTY 스폰");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));
    int cfd = connect_to(path);
    ASSERT(cfd >= 0, "연결");

    ipc_msg_header_t hdr;
    uint8_t buf[512];

    /* 세션 생성 */
    ipc_payload_session_create_t sreq;
    memset(&sreq, 0, sizeof(sreq));
    strncpy(sreq.name, "test", sizeof(sreq.name) - 1);
    client_send(cfd, IPC_MSG_SESSION_CREATE, &sreq, sizeof(sreq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    uint32_t sid = ((ipc_payload_session_created_t *)buf)->session_id;
    ASSERT(sid > 0, "세션 id 발급");

    /* 윈도우 생성 */
    ipc_payload_window_create_t wreq;
    memset(&wreq, 0, sizeof(wreq));
    wreq.session_id = sid;
    strncpy(wreq.name, "main", sizeof(wreq.name) - 1);
    client_send(cfd, IPC_MSG_WINDOW_CREATE, &wreq, sizeof(wreq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(hdr.type == IPC_MSG_WINDOW_CREATED, "WINDOW_CREATED 수신");
    uint32_t wid = ((ipc_payload_window_created_t *)buf)->window_id;
    ASSERT(wid > 0, "window_id 발급");

    /* 페인 생성 + PTY 스폰 */
    ipc_payload_pane_create_t preq;
    memset(&preq, 0, sizeof(preq));
    preq.session_id = sid;
    preq.window_id  = wid;
    preq.cols = 80;
    preq.rows = 24;
    client_send(cfd, IPC_MSG_PANE_CREATE, &preq, sizeof(preq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(hdr.type == IPC_MSG_PANE_CREATED, "PANE_CREATED 수신");

    const ipc_payload_pane_created_t *pcreated =
        (const ipc_payload_pane_created_t *)buf;
    ASSERT(pcreated->pane_id > 0, "pane_id 발급");
    ASSERT(pcreated->pid > 0,     "child pid 발급");

    uint32_t pane_id = pcreated->pane_id;

    /* PTY 리사이즈 */
    ipc_payload_pane_resize_t rreq;
    memset(&rreq, 0, sizeof(rreq));
    rreq.session_id = sid;
    rreq.window_id  = wid;
    rreq.pane_id    = pane_id;
    rreq.cols = 120;
    rreq.rows = 40;
    client_send(cfd, IPC_MSG_PANE_RESIZE, &rreq, sizeof(rreq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(hdr.type == IPC_MSG_OK, "PANE_RESIZE → OK");

    close(cfd);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

/* 세션 + window/pane 구성 헬퍼 — 성공 시 pane_id 반환, 실패 시 0. */
static uint32_t make_pane(int cfd, uint32_t sid, uint32_t wid,
                           uint16_t cols, uint16_t rows) {
    ipc_msg_header_t hdr;
    uint8_t buf[512];
    ipc_payload_pane_create_t preq;
    memset(&preq, 0, sizeof(preq));
    preq.session_id = sid;
    preq.window_id  = wid;
    preq.cols = cols;
    preq.rows = rows;
    client_send(cfd, IPC_MSG_PANE_CREATE, &preq, sizeof(preq));
    if (client_recv(cfd, &hdr, buf, sizeof(buf), 1000) < 0) return 0;
    if (hdr.type != IPC_MSG_PANE_CREATED) return 0;
    return ((ipc_payload_pane_created_t *)buf)->pane_id;
}

static uint32_t make_window(int cfd, uint32_t sid, const char *name) {
    ipc_msg_header_t hdr;
    uint8_t buf[512];
    ipc_payload_window_create_t wreq;
    memset(&wreq, 0, sizeof(wreq));
    wreq.session_id = sid;
    strncpy(wreq.name, name, sizeof(wreq.name) - 1);
    client_send(cfd, IPC_MSG_WINDOW_CREATE, &wreq, sizeof(wreq));
    if (client_recv(cfd, &hdr, buf, sizeof(buf), 1000) < 0) return 0;
    if (hdr.type != IPC_MSG_WINDOW_CREATED) return 0;
    return ((ipc_payload_window_created_t *)buf)->window_id;
}

/* PANE_RESIZE 를 보내고 OK 를 소비한다. */
static int send_resize(int cfd, uint32_t sid, uint32_t wid, uint32_t pid,
                        uint16_t cols, uint16_t rows) {
    ipc_msg_header_t hdr;
    uint8_t buf[512];
    ipc_payload_pane_resize_t rreq;
    memset(&rreq, 0, sizeof(rreq));
    rreq.session_id = sid;
    rreq.window_id  = wid;
    rreq.pane_id    = pid;
    rreq.cols = cols;
    rreq.rows = rows;
    client_send(cfd, IPC_MSG_PANE_RESIZE, &rreq, sizeof(rreq));
    if (client_recv(cfd, &hdr, buf, sizeof(buf), 1000) < 0) return -1;
    return hdr.type == IPC_MSG_OK ? 0 : -1;
}

static void test_attach_multi_window(void) {
    GROUP("ATTACH_R — 다중 window + layout blob");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));
    int cfd = connect_to(path);
    ASSERT(cfd >= 0, "연결");

    ipc_msg_header_t hdr;
    uint8_t buf[IPC_MAX_PAYLOAD_LEN];

    /* 세션 + window 2개, 각 window 에 pane 1개 */
    ipc_payload_session_create_t sreq;
    memset(&sreq, 0, sizeof(sreq));
    strncpy(sreq.name, "multi", sizeof(sreq.name) - 1);
    client_send(cfd, IPC_MSG_SESSION_CREATE, &sreq, sizeof(sreq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    uint32_t sid = ((ipc_payload_session_created_t *)buf)->session_id;

    uint32_t w1 = make_window(cfd, sid, "one");
    uint32_t w2 = make_window(cfd, sid, "two");
    ASSERT(w1 > 0 && w2 > 0 && w1 != w2, "window 2개 생성");

    uint32_t p1 = make_pane(cfd, sid, w1, 80, 24);
    uint32_t p2 = make_pane(cfd, sid, w2, 80, 24);
    ASSERT(p1 > 0 && p2 > 0, "pane 2개 생성");

    /* 각 window 에 서로 다른 layout blob 업로드 */
    uint8_t lay[sizeof(ipc_payload_window_layout_t) + 8];
    ipc_payload_window_layout_t *lreq = (ipc_payload_window_layout_t *)lay;
    memset(lay, 0, sizeof(lay));
    lreq->session_id = sid;
    lreq->window_id  = w1;
    lreq->blob_len   = 4;
    memcpy(lay + sizeof(*lreq), "AAAA", 4);
    client_send(cfd, IPC_MSG_WINDOW_LAYOUT, lay, (uint16_t)(sizeof(*lreq) + 4));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);

    memset(lay, 0, sizeof(lay));
    lreq->session_id = sid;
    lreq->window_id  = w2;
    lreq->blob_len   = 6;
    memcpy(lay + sizeof(*lreq), "BBBBBB", 6);
    client_send(cfd, IPC_MSG_WINDOW_LAYOUT, lay, (uint16_t)(sizeof(*lreq) + 6));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);

    /* attach → 두 window 와 각자의 blob 이 모두 와야 한다 */
    ipc_payload_session_attach_t areq = { .session_id = sid };
    client_send(cfd, IPC_MSG_SESSION_ATTACH, &areq, sizeof(areq));
    ASSERT(client_recv(cfd, &hdr, buf, sizeof(buf), 1000) == 0, "ATTACH_R 수신");
    ASSERT(hdr.type == IPC_MSG_SESSION_ATTACH_R, "타입 ATTACH_R");

    const ipc_payload_session_attach_r_t *resp =
        (const ipc_payload_session_attach_r_t *)buf;
    ASSERT(resp->pane_count == 2,   "pane_count = 2");
    ASSERT(resp->window_count == 2, "window_count = 2");

    size_t pane_sz = (size_t)resp->pane_count * sizeof(ipc_attach_pane_info_t);
    const ipc_attach_window_info_t *warr =
        (const ipc_attach_window_info_t *)(buf + sizeof(*resp) + pane_sz);
    size_t win_sz = (size_t)resp->window_count * sizeof(ipc_attach_window_info_t);
    const uint8_t *blobs = buf + sizeof(*resp) + pane_sz + win_sz;

    ASSERT(warr[0].window_id == w1 && warr[1].window_id == w2,
           "window 순서 보존");
    ASSERT(strcmp(warr[0].name, "one") == 0 && strcmp(warr[1].name, "two") == 0,
           "window 이름 전달");
    ASSERT(warr[0].blob_len == 4 && warr[1].blob_len == 6,
           "window 별 blob 길이");
    ASSERT(memcmp(blobs, "AAAA", 4) == 0, "첫 window blob 내용");
    ASSERT(memcmp(blobs + 4, "BBBBBB", 6) == 0, "둘째 window blob 이 뒤에 연접");

    /* pane 이 각자 자기 window 에 매핑돼 있어야 한다 */
    const ipc_attach_pane_info_t *parr =
        (const ipc_attach_pane_info_t *)(buf + sizeof(*resp));
    int ok = 0;
    for (uint32_t i = 0; i < resp->pane_count; i++) {
        if (parr[i].pane_id == p1 && parr[i].window_id == w1) ok++;
        if (parr[i].pane_id == p2 && parr[i].window_id == w2) ok++;
    }
    ASSERT(ok == 2, "pane→window 매핑 정확");

    close(cfd);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

static void test_pane_size_min_clamp(void) {
    GROUP("PANE_RESIZE — 다중 클라이언트 최소 크기 클램프");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));

    int c1 = connect_to(path);
    ASSERT(c1 >= 0, "클라이언트 1 연결");

    ipc_msg_header_t hdr;
    uint8_t buf[512];

    ipc_payload_session_create_t sreq;
    memset(&sreq, 0, sizeof(sreq));
    strncpy(sreq.name, "clamp", sizeof(sreq.name) - 1);
    client_send(c1, IPC_MSG_SESSION_CREATE, &sreq, sizeof(sreq));
    client_recv(c1, &hdr, buf, sizeof(buf), 1000);
    uint32_t sid = ((ipc_payload_session_created_t *)buf)->session_id;
    uint32_t wid = make_window(c1, sid, "w");
    uint32_t pid = make_pane(c1, sid, wid, 80, 24);
    ASSERT(pid > 0, "pane 생성");

    /* 클라이언트 1: 120x40 요청 → 혼자이므로 그대로 적용 */
    ASSERT(send_resize(c1, sid, wid, pid, 120, 40) == 0, "c1 resize OK");
    session_t *s = session_find_by_id(&mgr, sid);
    window_t  *w = s ? window_find_by_id(s, wid) : NULL;
    pane_t    *p = w ? pane_find_by_id(w, pid) : NULL;
    ASSERT(p != NULL, "pane 조회");
    ASSERT(p && p->cols == 120 && p->rows == 40, "단일 클라이언트: 요청 그대로");

    /* 클라이언트 2 가 더 작은 크기를 요청하면 최소값으로 클램프 */
    int c2 = connect_to(path);
    ASSERT(c2 >= 0, "클라이언트 2 연결");
    ASSERT(send_resize(c2, sid, wid, pid, 80, 24) == 0, "c2 resize OK");
    ASSERT(p && p->cols == 80 && p->rows == 24, "두 클라이언트: 최소값 적용");

    /* 큰 쪽이 더 키워도 작은 쪽이 남아 있으면 그대로 */
    ASSERT(send_resize(c1, sid, wid, pid, 200, 60) == 0, "c1 재확대 OK");
    ASSERT(p && p->cols == 80 && p->rows == 24, "작은 클라이언트가 상한을 유지");

    /* 축은 독립적으로 최소화된다 (c2 가 넓지만 낮은 경우) */
    ASSERT(send_resize(c2, sid, wid, pid, 300, 10) == 0, "c2 폭↑ 높이↓");
    ASSERT(p && p->cols == 200 && p->rows == 10, "cols/rows 각각 최소");

    /* 작은 클라이언트가 나가면 남은 클라이언트 크기로 복귀 */
    close(c2);
    for (int i = 0; i < 40 && p && p->rows != 60; i++) usleep(25 * 1000);
    ASSERT(p && p->cols == 200 && p->rows == 60, "detach 후 남은 크기로 복귀");

    close(c1);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

static void test_pty_io(void) {
    GROUP("PTY_INPUT → PTY_OUTPUT");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));
    int cfd = connect_to(path);
    ASSERT(cfd >= 0, "연결");

    ipc_msg_header_t hdr;
    uint8_t buf[4096];

    /* 세션 + 윈도우 + 페인 생성 */
    ipc_payload_session_create_t sreq;
    memset(&sreq, 0, sizeof(sreq));
    strncpy(sreq.name, "io_test", sizeof(sreq.name) - 1);
    client_send(cfd, IPC_MSG_SESSION_CREATE, &sreq, sizeof(sreq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    uint32_t sid = ((ipc_payload_session_created_t *)buf)->session_id;

    ipc_payload_window_create_t wreq;
    memset(&wreq, 0, sizeof(wreq));
    wreq.session_id = sid;
    strncpy(wreq.name, "w1", sizeof(wreq.name) - 1);
    client_send(cfd, IPC_MSG_WINDOW_CREATE, &wreq, sizeof(wreq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    uint32_t wid = ((ipc_payload_window_created_t *)buf)->window_id;

    ipc_payload_pane_create_t preq;
    memset(&preq, 0, sizeof(preq));
    preq.session_id = sid;
    preq.window_id  = wid;
    preq.cols = 80;
    preq.rows = 24;
    client_send(cfd, IPC_MSG_PANE_CREATE, &preq, sizeof(preq));
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    uint32_t pane_id = ((ipc_payload_pane_created_t *)buf)->pane_id;
    ASSERT(pane_id > 0, "pane 생성 확인");

    /* 초기 프롬프트 flush */
    usleep(300 * 1000);
    struct pollfd pf = { cfd, POLLIN, 0 };
    while (poll(&pf, 1, 50) > 0 && (pf.revents & POLLIN))
        client_recv(cfd, &hdr, buf, sizeof(buf), 100);

    /* echo 명령 입력 */
    const char *cmd = "echo IPC_TEST_OK\n";
    uint8_t inp_buf[sizeof(ipc_payload_pty_data_t) + 32];
    ipc_payload_pty_data_t *inp = (ipc_payload_pty_data_t *)inp_buf;
    inp->pane_id  = pane_id;
    inp->data_len = (uint16_t)strlen(cmd);
    memcpy(inp_buf + sizeof(*inp), cmd, strlen(cmd));
    client_send(cfd, IPC_MSG_PTY_INPUT, inp_buf,
                (uint16_t)(sizeof(*inp) + strlen(cmd)));

    /* PTY_OUTPUT 수신 (최대 1s, 여러 청크) */
    int found = 0;
    for (int i = 0; i < 20 && !found; i++) {
        struct pollfd pf2 = { cfd, POLLIN, 0 };
        if (poll(&pf2, 1, 100) <= 0) continue;

        if (client_recv(cfd, &hdr, buf, sizeof(buf) - 1, 200) != 0) continue;
        if (hdr.type != IPC_MSG_PTY_OUTPUT) continue;

        /* 데이터 헤더 뒤의 실제 텍스트 확인 */
        uint8_t *text = buf + sizeof(ipc_payload_pty_data_t);
        size_t text_len = hdr.payload_len - sizeof(ipc_payload_pty_data_t);
        if (hdr.payload_len >= sizeof(ipc_payload_pty_data_t) && text_len > 0) {
            text[text_len] = '\0';
            if (strstr((char *)text, "IPC_TEST_OK")) found = 1;
        }
    }
    ASSERT(found, "PTY_OUTPUT에서 echo 결과 수신");

    close(cfd);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

static void test_error_handling(void) {
    GROUP("에러 처리: 없는 session_id");

    session_manager_t mgr;
    session_manager_init(&mgr);

    pthread_t tid;
    server_ctx_t ctx;
    ipc_server_t *srv = start_test_server(&mgr, &tid, &ctx);
    ASSERT(srv != NULL, "서버 시작");
    if (!srv) return;

    char path[IPC_SOCKET_PATH_MAX];
    ipc_socket_path(path, sizeof(path));
    int cfd = connect_to(path);
    ASSERT(cfd >= 0, "연결");

    /* 없는 session_id로 윈도우 생성 시도 */
    ipc_payload_window_create_t wreq;
    memset(&wreq, 0, sizeof(wreq));
    wreq.session_id = 9999;
    strncpy(wreq.name, "x", sizeof(wreq.name) - 1);
    client_send(cfd, IPC_MSG_WINDOW_CREATE, &wreq, sizeof(wreq));

    ipc_msg_header_t hdr;
    uint8_t buf[512];
    client_recv(cfd, &hdr, buf, sizeof(buf), 1000);
    ASSERT(hdr.type == IPC_MSG_ERROR, "ERROR 응답 수신");

    const ipc_payload_error_t *err = (const ipc_payload_error_t *)buf;
    ASSERT(err->error_code == IPC_ERR_SESSION_NOT_FOUND,
           "에러 코드 = SESSION_NOT_FOUND");

    close(cfd);
    ipc_server_shutdown(srv);
    pthread_join(tid, NULL);
    ipc_server_destroy(srv);
    session_manager_destroy(&mgr);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== IPC 서버 테스트 ===\n");

    test_socket_path();
    test_listen_accept();
    test_hello_handshake();
    test_session_create_list();
    test_window_pane_create();
    test_attach_multi_window();
    test_pane_size_min_clamp();
    test_pty_io();
    test_error_handling();

    printf("\n결과: %d 통과 / %d 실패\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
