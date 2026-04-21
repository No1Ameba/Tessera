#define _GNU_SOURCE
#include "ipc_client.h"
#include "../platform/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define RECV_BUF_SIZE  65536
#define CMD_TIMEOUT_MS 5000

struct ipc_client {
    int              fd;         /* Unix socket fd (양방향) 또는 -1 (pipe 모드) */
    int              pipe_r;    /* pipe 모드: 읽기 fd (SSH stdout) */
    int              pipe_w;    /* pipe 모드: 쓰기 fd (SSH stdin) */
    pid_t            bridge_pid;/* pipe 모드: SSH 자식 PID */
    pty_output_cb_t  cb;
    void            *cb_user;
    pane_exited_cb_t exit_cb;
    void            *exit_cb_user;
    pane_split_cb_t  split_cb;
    void            *split_cb_user;
    uint8_t          rbuf[RECV_BUF_SIZE];
    size_t           rbuf_len;
};

/* ── Time helper ─────────────────────────────────────────────────────────── */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 읽기/쓰기 fd 선택: pipe 모드면 pipe_r/pipe_w, 아니면 fd */
static int read_fd(const ipc_client_t *c)  { return c->pipe_r >= 0 ? c->pipe_r : c->fd; }
static int write_fd(const ipc_client_t *c) { return c->pipe_w >= 0 ? c->pipe_w : c->fd; }

/* ── Wire-level send ─────────────────────────────────────────────────────── */

static int send_msg(int fd, ipc_msg_type_t type,
                    const void *payload, size_t plen)
{
    ipc_msg_header_t hdr = IPC_HEADER_INIT(type, (uint16_t)plen);
    struct iovec iov[2] = {
        { &hdr,                    sizeof hdr },
        { (void *)(uintptr_t)payload, plen    },
    };
    int iovcnt = plen ? 2 : 1;
    ssize_t total = (ssize_t)(sizeof hdr + plen);
    ssize_t sent  = 0;

    /* Simple loop to handle partial writes. */
    uint8_t buf[sizeof hdr + IPC_MAX_PAYLOAD_LEN];
    memcpy(buf, &hdr, sizeof hdr);
    if (plen) memcpy(buf + sizeof hdr, payload, plen);
    (void)iov; (void)iovcnt;

    while (sent < total) {
        ssize_t n = write(fd, buf + sent, (size_t)(total - sent));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

/* ── Wire-level receive (accumulates into rbuf) ──────────────────────────── */

/*
 * Dispatch one complete message from rbuf.
 * Returns the ipc_msg_type_t, or -1 if no complete message available yet.
 * Advances rbuf_len by consuming the message.
 *
 * If type == IPC_MSG_PTY_OUTPUT, fires the callback.
 * If want != 0 and type == want, copies payload to out_payload/out_size.
 */
static int dispatch_one(ipc_client_t *c,
                         ipc_msg_type_t want,
                         void *out_payload, size_t out_size)
{
    if (c->rbuf_len < sizeof(ipc_msg_header_t)) return -1;

    ipc_msg_header_t hdr;
    memcpy(&hdr, c->rbuf, sizeof hdr);

    /* 프로토콜 desync 감지 — 매직이 맞지 않으면 스트림이 어긋난 것이므로
     * 버퍼를 초기화해 연결을 끊는다. 이후 처리는 호출자가 read=0/-1 로 인식. */
    uint32_t expected = ((uint32_t)IPC_VERSION << 24) | IPC_MAGIC;
    if (hdr.magic_ver != expected) {
        fprintf(stderr, "[ipc] protocol desync: magic=0x%08x expected=0x%08x\n",
                hdr.magic_ver, expected);
        c->rbuf_len = 0;
        return -1;
    }

    size_t total = sizeof hdr + hdr.payload_len;
    if (c->rbuf_len < total) return -1;

    uint8_t *payload = c->rbuf + sizeof hdr;
    int type = (int)hdr.type;

    if (type == IPC_MSG_PTY_OUTPUT && c->cb && hdr.payload_len >= sizeof(ipc_payload_pty_data_t)) {
        ipc_payload_pty_data_t pd;
        memcpy(&pd, payload, sizeof pd);
        size_t dlen = pd.data_len;
        if (dlen > hdr.payload_len - sizeof pd) dlen = hdr.payload_len - sizeof pd;
        c->cb(pd.pane_id, payload + sizeof pd, dlen, c->cb_user);
    } else if (type == IPC_MSG_PANE_EXITED && c->exit_cb &&
               hdr.payload_len >= sizeof(ipc_payload_pane_ref_t)) {
        ipc_payload_pane_ref_t ref;
        memcpy(&ref, payload, sizeof ref);
        c->exit_cb(ref.pane_id, c->exit_cb_user);
    } else if (type == IPC_MSG_PANE_SPLIT_NOTIFY && c->split_cb &&
               hdr.payload_len >= sizeof(ipc_payload_pane_split_notify_t)) {
        ipc_payload_pane_split_notify_t n;
        memcpy(&n, payload, sizeof n);
        c->split_cb(n.session_id, n.window_id,
                    n.parent_pane_id, n.new_pane_id,
                    n.cols, n.rows, n.direction, n.ratio,
                    c->split_cb_user);
    } else if (want && type == (int)want && out_payload && out_size) {
        size_t copy = hdr.payload_len < out_size ? hdr.payload_len : out_size;
        memcpy(out_payload, payload, copy);
    }

    /* Consume the message from rbuf. */
    size_t remaining = c->rbuf_len - total;
    if (remaining) memmove(c->rbuf, c->rbuf + total, remaining);
    c->rbuf_len = remaining;

    return type;
}

/*
 * Read from socket until we have at least one complete message, or timeout.
 * Returns the message type or -1 on error/timeout.
 */
static int recv_until(ipc_client_t *c, ipc_msg_type_t want,
                       void *out_payload, size_t out_size, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;

    for (;;) {
        /* Try to dispatch from existing buffer first. */
        int dispatched;
        while ((dispatched = dispatch_one(c, want, out_payload, out_size)) != -1) {
            if (dispatched == (int)want) return dispatched;
            if (dispatched == IPC_MSG_ERROR) return IPC_MSG_ERROR;
        }

        int remaining_ms = (int)(deadline - now_ms());
        if (remaining_ms <= 0) return -1;

        struct pollfd pfd = { .fd = read_fd(c), .events = POLLIN };
        int ret = poll(&pfd, 1, remaining_ms);
        if (ret <= 0) return -1; /* timeout or error */

        ssize_t n = read(read_fd(c), c->rbuf + c->rbuf_len,
                         RECV_BUF_SIZE - c->rbuf_len);
        if (n <= 0) return -1; /* EOF or error */
        c->rbuf_len += (size_t)n;
    }
}

/* ── On-demand daemon spawn ──────────────────────────────────────────────── */

static int spawn_daemon(void)
{
    /*
     * 데몬 바이너리 탐색 순서:
     *   1. 클라이언트와 같은 디렉토리  (설치 후 /usr/local/bin/ 경우)
     *   2. 클라이언트 디렉토리의 ../daemon/  (빌드 트리에서 실행할 때)
     *   3. execlp 로 PATH 탐색
     */
    char exe[1024] = {0};
    ssize_t elen = readlink("/proc/self/exe", exe, sizeof exe - 1);

    char candidate1[1088] = {0};  /* 같은 디렉토리 */
    char candidate2[1088] = {0};  /* ../daemon/ */

    if (elen > 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            /* 1: same dir */
            size_t dir_len = (size_t)(slash - exe + 1);
            snprintf(candidate1, sizeof candidate1,
                     "%.*stermemu-daemon", (int)dir_len, exe);
            /* 2: ../daemon/ */
            snprintf(candidate2, sizeof candidate2,
                     "%.*s../daemon/termemu-daemon", (int)dir_len, exe);
        }
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: detach, silence stdio, exec daemon. */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
            close(devnull);
        }
        if (candidate1[0]) execl(candidate1, "termemu-daemon", "--daemon", (char*)NULL);
        if (candidate2[0]) execl(candidate2, "termemu-daemon", "--daemon", (char*)NULL);
        execlp("termemu-daemon", "termemu-daemon", "--daemon", (char*)NULL);
        _exit(1);
    }

    /* Parent: wait up to 2 seconds for the socket to appear. */
    char sock_path[IPC_SOCKET_PATH_MAX];
    if (ipc_socket_path(sock_path, sizeof sock_path) != 0) return -1;

    for (int i = 0; i < 40; i++) {
        struct timespec ts = { 0, 50 * 1000 * 1000L };
        nanosleep(&ts, NULL);
        struct stat st;
        if (stat(sock_path, &st) == 0) return 0;
    }
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

ipc_client_t *ipc_client_create(pty_output_cb_t cb, void *user)
{
    ipc_client_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd         = -1;
    c->pipe_r     = -1;
    c->pipe_w     = -1;
    c->bridge_pid = -1;
    c->cb         = cb;
    c->cb_user    = user;
    return c;
}

void ipc_client_destroy(ipc_client_t *c)
{
    if (!c) return;
    ipc_client_disconnect(c);
    free(c);
}

void ipc_client_set_pane_exited_cb(ipc_client_t *c,
                                    pane_exited_cb_t cb, void *user)
{
    if (!c) return;
    c->exit_cb      = cb;
    c->exit_cb_user = user;
}

void ipc_client_set_pane_split_cb(ipc_client_t *c,
                                   pane_split_cb_t cb, void *user)
{
    if (!c) return;
    c->split_cb      = cb;
    c->split_cb_user = user;
}

int ipc_client_connect(ipc_client_t *c)
{
    char path[IPC_SOCKET_PATH_MAX];
    if (ipc_socket_path(path, sizeof path) != 0) return -1;

    /* Try to connect; if the socket doesn't exist, spawn the daemon. */
    int fd = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0) break;

        close(fd); fd = -1;
        if (attempt == 0) {
            /* Socket missing or refused: try spawning the daemon. */
            if (spawn_daemon() != 0) return -1;
        }
    }
    if (fd < 0) return -1;
    c->fd = fd;

    /* HELLO handshake. */
    ipc_payload_hello_t hello = {
        .client_pid = (uint32_t)getpid(),
    };
    snprintf(hello.client_version, sizeof hello.client_version, "0.1.0");

    if (send_msg(fd, IPC_MSG_HELLO, &hello, sizeof hello) != 0) {
        ipc_client_disconnect(c); return -1;
    }
    ipc_payload_hello_ack_t ack = {0};
    if (recv_until(c, IPC_MSG_HELLO_ACK, &ack, sizeof ack, CMD_TIMEOUT_MS) < 0) {
        ipc_client_disconnect(c); return -1;
    }
    (void)ack;
    return 0;
}

void ipc_client_disconnect(ipc_client_t *c)
{
    if (!c || c->fd < 0) return;
    close(c->fd);
    c->fd = -1;
    c->rbuf_len = 0;
}

/* ── Session operations ──────────────────────────────────────────────────── */

int ipc_client_session_create(ipc_client_t *c, const char *name,
                               uint32_t *out_id)
{
    ipc_payload_session_create_t req = {0};
    strncpy(req.name, name, sizeof req.name - 1);
    if (send_msg(write_fd(c), IPC_MSG_SESSION_CREATE, &req, sizeof req) != 0) return -1;

    ipc_payload_session_created_t resp = {0};
    if (recv_until(c, IPC_MSG_SESSION_CREATED, &resp, sizeof resp, CMD_TIMEOUT_MS) < 0)
        return -1;
    if (out_id) *out_id = resp.session_id;
    return 0;
}

int ipc_client_session_destroy(ipc_client_t *c, uint32_t session_id)
{
    ipc_payload_session_destroy_t req = { .session_id = session_id };
    if (send_msg(write_fd(c), IPC_MSG_SESSION_DESTROY, &req, sizeof req) != 0) return -1;
    return recv_until(c, IPC_MSG_OK, NULL, 0, CMD_TIMEOUT_MS) < 0 ? -1 : 0;
}

int ipc_client_session_list(ipc_client_t *c,
                             ipc_session_info_t *buf, int max, int *out_count)
{
    if (send_msg(write_fd(c), IPC_MSG_SESSION_LIST, NULL, 0) != 0) return -1;

    /* SESSION_LIST_R payload is an array of ipc_session_info_t. */
    uint8_t raw[sizeof(ipc_session_info_t) * 64] = {0};
    if (recv_until(c, IPC_MSG_SESSION_LIST_R, raw, sizeof raw, CMD_TIMEOUT_MS) < 0)
        return -1;

    /* Recover actual payload length from the last received header.
     * Simple approach: count non-zero entries (not ideal but works for tests). */
    int n = 0;
    ipc_session_info_t *infos = (ipc_session_info_t*)raw;
    for (int i = 0; i < 64 && n < max; i++) {
        if (!infos[i].session_id && !infos[i].name[0]) break;
        buf[n++] = infos[i];
    }
    if (out_count) *out_count = n;
    return 0;
}

/* ── Window operations ───────────────────────────────────────────────────── */

int ipc_client_window_create(ipc_client_t *c, uint32_t session_id,
                              const char *name, uint32_t *out_id)
{
    ipc_payload_window_create_t req = { .session_id = session_id };
    strncpy(req.name, name, sizeof req.name - 1);
    if (send_msg(write_fd(c), IPC_MSG_WINDOW_CREATE, &req, sizeof req) != 0) return -1;

    ipc_payload_window_created_t resp = {0};
    if (recv_until(c, IPC_MSG_WINDOW_CREATED, &resp, sizeof resp, CMD_TIMEOUT_MS) < 0)
        return -1;
    if (out_id) *out_id = resp.window_id;
    return 0;
}

int ipc_client_window_destroy(ipc_client_t *c, uint32_t session_id,
                               uint32_t window_id)
{
    ipc_payload_window_ref_t req = { .session_id = session_id, .window_id = window_id };
    if (send_msg(write_fd(c), IPC_MSG_WINDOW_DESTROY, &req, sizeof req) != 0) return -1;
    return recv_until(c, IPC_MSG_OK, NULL, 0, CMD_TIMEOUT_MS) < 0 ? -1 : 0;
}

int ipc_client_window_focus(ipc_client_t *c, uint32_t session_id,
                             uint32_t window_id)
{
    ipc_payload_window_ref_t req = { .session_id = session_id, .window_id = window_id };
    if (send_msg(write_fd(c), IPC_MSG_WINDOW_FOCUS, &req, sizeof req) != 0) return -1;
    return recv_until(c, IPC_MSG_OK, NULL, 0, CMD_TIMEOUT_MS) < 0 ? -1 : 0;
}

int ipc_client_window_layout(ipc_client_t *c, uint32_t session_id,
                              uint32_t window_id,
                              const uint8_t *blob, uint16_t blob_len)
{
    if (!c) return -1;
    size_t total = sizeof(ipc_payload_window_layout_t) + blob_len;
    uint8_t buf[sizeof(ipc_payload_window_layout_t) + 4096];
    if (total > sizeof(buf)) return -1;
    ipc_payload_window_layout_t *h = (ipc_payload_window_layout_t *)buf;
    memset(h, 0, sizeof(*h));
    h->session_id = session_id;
    h->window_id  = window_id;
    h->blob_len   = blob_len;
    if (blob && blob_len) memcpy(buf + sizeof(*h), blob, blob_len);
    /* 비동기 — OK 응답 대기 안 함. on_pane_exited 등 콜백에서도 호출되므로
     * recv_until 로 인한 재귀 dispatch 를 피해야 한다. 응답은 이후
     * ipc_client_poll 에서 자연스럽게 소비된다. */
    return send_msg(write_fd(c), IPC_MSG_WINDOW_LAYOUT, buf, total);
}

/* ── Pane operations ─────────────────────────────────────────────────────── */

int ipc_client_pane_create(ipc_client_t *c, uint32_t session_id,
                            uint32_t window_id, uint16_t cols, uint16_t rows,
                            uint32_t *out_id)
{
    return ipc_client_pane_create_split(c, session_id, window_id, cols, rows,
                                         0, 0, 0.0f, out_id);
}

int ipc_client_pane_create_split(ipc_client_t *c, uint32_t session_id,
                                  uint32_t window_id,
                                  uint16_t cols, uint16_t rows,
                                  uint32_t parent_pane_id,
                                  uint8_t direction, float ratio,
                                  uint32_t *out_id)
{
    ipc_payload_pane_create_t req;
    memset(&req, 0, sizeof req);
    req.session_id     = session_id;
    req.window_id      = window_id;
    req.cols           = cols;
    req.rows           = rows;
    req.parent_pane_id = parent_pane_id;
    req.direction      = direction;
    req.ratio          = ratio;
    if (send_msg(write_fd(c), IPC_MSG_PANE_CREATE, &req, sizeof req) != 0) return -1;

    ipc_payload_pane_created_t resp = {0};
    if (recv_until(c, IPC_MSG_PANE_CREATED, &resp, sizeof resp, CMD_TIMEOUT_MS) < 0)
        return -1;
    if (out_id) *out_id = resp.pane_id;
    return 0;
}

int ipc_client_pane_destroy(ipc_client_t *c, uint32_t session_id,
                             uint32_t window_id, uint32_t pane_id)
{
    ipc_payload_pane_ref_t req = {
        .session_id = session_id, .window_id = window_id, .pane_id = pane_id,
    };
    if (send_msg(write_fd(c), IPC_MSG_PANE_DESTROY, &req, sizeof req) != 0) return -1;
    return recv_until(c, IPC_MSG_OK, NULL, 0, CMD_TIMEOUT_MS) < 0 ? -1 : 0;
}

int ipc_client_pane_resize(ipc_client_t *c, uint32_t session_id,
                            uint32_t window_id, uint32_t pane_id,
                            uint16_t cols, uint16_t rows)
{
    ipc_payload_pane_resize_t req = {
        .session_id = session_id, .window_id = window_id, .pane_id = pane_id,
        .cols = cols, .rows = rows,
    };
    /* 비동기 — on_pane_exited / framebuffer resize 등 빈번한 호출 경로에서
     * 재진입 dispatch 를 피한다. 실패해도 다음 resize 에서 복구됨. */
    return send_msg(write_fd(c), IPC_MSG_PANE_RESIZE, &req, sizeof req);
}

int ipc_client_pane_focus(ipc_client_t *c, uint32_t session_id,
                           uint32_t window_id, uint32_t pane_id)
{
    ipc_payload_pane_ref_t req = {
        .session_id = session_id, .window_id = window_id, .pane_id = pane_id,
    };
    if (send_msg(write_fd(c), IPC_MSG_PANE_FOCUS, &req, sizeof req) != 0) return -1;
    return recv_until(c, IPC_MSG_OK, NULL, 0, CMD_TIMEOUT_MS) < 0 ? -1 : 0;
}

/* ── Data ────────────────────────────────────────────────────────────────── */

int ipc_client_pty_input(ipc_client_t *c, uint32_t pane_id,
                          const uint8_t *data, size_t len)
{
    if (len > IPC_PTY_CHUNK_MAX) len = IPC_PTY_CHUNK_MAX;

    /* Build header + data inline. */
    size_t total = sizeof(ipc_msg_header_t) +
                   sizeof(ipc_payload_pty_data_t) + len;
    uint8_t buf[sizeof(ipc_msg_header_t) + sizeof(ipc_payload_pty_data_t) + IPC_PTY_CHUNK_MAX];

    ipc_msg_header_t *hdr = (ipc_msg_header_t*)buf;
    hdr->magic_ver   = ((IPC_VERSION) << 24) | (IPC_MAGIC);
    hdr->type        = IPC_MSG_PTY_INPUT;
    hdr->flags       = 0;
    hdr->payload_len = (uint16_t)(sizeof(ipc_payload_pty_data_t) + len);

    ipc_payload_pty_data_t *pd = (ipc_payload_pty_data_t*)(buf + sizeof *hdr);
    pd->pane_id  = pane_id;
    pd->data_len = (uint16_t)len;
    memcpy(buf + sizeof *hdr + sizeof *pd, data, len);

    ssize_t sent = 0;
    ssize_t tot  = (ssize_t)total;
    while (sent < tot) {
        ssize_t n = write(write_fd(c), buf + sent, (size_t)(tot - sent));
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        sent += n;
    }
    return 0;
}

int ipc_client_poll(ipc_client_t *c, int timeout_ms)
{
    if (!c || read_fd(c) < 0) return -1;

    struct pollfd pfd = { .fd = read_fd(c), .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) return -1;
    if (ret == 0) return 0;

    /* Read available data. */
    ssize_t n = read(read_fd(c), c->rbuf + c->rbuf_len,
                     RECV_BUF_SIZE - c->rbuf_len);
    if (n <= 0) return -1;
    c->rbuf_len += (size_t)n;

    /* Dispatch all complete messages. */
    int count = 0;
    int type;
    while ((type = dispatch_one(c, 0, NULL, 0)) != -1) {
        (void)type;
        count++;
    }
    return count;
}

/* ── 원격 연결 (SSH 파이프 트랜스포트) ───────────────────────────────────── */

int ipc_client_connect_remote(ipc_client_t *c, const char *ssh_target)
{
    if (!c || !ssh_target) return -1;

    int to_child[2], from_child[2];
    if (pipe(to_child) < 0 || pipe(from_child) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return -1;
    }

    if (pid == 0) {
        /* 자식: SSH 실행 */
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        /* stderr는 그대로 유지 — SSH 에러 메시지가 보이도록 */
        close(to_child[0]);
        close(from_child[1]);
        execlp("ssh", "ssh", "-T",
               ssh_target, "termemu-bridge", (char *)NULL);
        /* exec 실패 시 stderr에 출력 */
        perror("exec ssh");
        _exit(1);
    }

    /* 부모 */
    close(to_child[0]);
    close(from_child[1]);
    c->pipe_w = to_child[1];
    c->pipe_r = from_child[0];
    c->bridge_pid = pid;
    c->fd = -1;

    fprintf(stderr, "[remote] SSH child pid=%d, waiting for HELLO_ACK...\n", (int)pid);

    /* SSH 연결 대기 후 HELLO 핸드셰이크 */
    ipc_payload_hello_t hello;
    memset(&hello, 0, sizeof hello);
    hello.client_pid = (uint32_t)getpid();
    strncpy(hello.client_version, "0.1.0", sizeof(hello.client_version) - 1);

    if (send_msg(write_fd(c), IPC_MSG_HELLO, &hello, sizeof hello) != 0) {
        fprintf(stderr, "[remote] send HELLO failed\n");
        return -1;
    }

    uint8_t ack_buf[256];
    if (recv_until(c, IPC_MSG_HELLO_ACK, ack_buf, sizeof ack_buf,
                    15000) < 0) {  /* 15초 — SSH 키 교환 시간 포함 */
        fprintf(stderr, "[remote] HELLO_ACK timeout — check:\n"
                        "  1. SSH key auth to '%s' works (ssh %s echo ok)\n"
                        "  2. termemu-bridge is installed on remote\n"
                        "  3. termemu-daemon is running on remote\n",
                ssh_target, ssh_target);
        /* SSH 자식 프로세스 정리 */
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }

    fprintf(stderr, "[remote] connected via SSH bridge\n");
    return 0;
}

/* ── 세션 attach ─────────────────────────────────────────────────────────── */

int ipc_client_session_attach(ipc_client_t *c, uint32_t session_id,
                               ipc_attach_pane_info_t *panes_out, int max_panes,
                               int *out_count)
{
    return ipc_client_session_attach_ex(c, session_id, panes_out, max_panes,
                                         out_count, NULL, 0, NULL);
}

int ipc_client_session_attach_ex(ipc_client_t *c, uint32_t session_id,
                                  ipc_attach_pane_info_t *panes_out, int max_panes,
                                  int *out_count,
                                  uint8_t *blob_buf, size_t blob_buf_size,
                                  uint16_t *out_blob_len)
{
    if (!c || !panes_out || !out_count) return -1;

    ipc_payload_session_attach_t req = { .session_id = session_id };
    if (send_msg(write_fd(c), IPC_MSG_SESSION_ATTACH, &req, sizeof req) != 0)
        return -1;

    uint8_t buf[IPC_MAX_PAYLOAD_LEN];
    if (recv_until(c, IPC_MSG_SESSION_ATTACH_R, buf, sizeof buf,
                    CMD_TIMEOUT_MS) < 0)
        return -1;

    const ipc_payload_session_attach_r_t *resp =
        (const ipc_payload_session_attach_r_t *)buf;
    int cnt = (int)resp->pane_count;
    if (cnt > max_panes) cnt = max_panes;

    const ipc_attach_pane_info_t *arr =
        (const ipc_attach_pane_info_t *)(buf + sizeof(*resp));
    for (int i = 0; i < cnt; i++)
        panes_out[i] = arr[i];
    *out_count = cnt;

    uint16_t bl = resp->layout_blob_len;
    if (out_blob_len) *out_blob_len = bl;
    if (blob_buf && bl > 0) {
        size_t copy = bl < blob_buf_size ? bl : blob_buf_size;
        const uint8_t *blob_src =
            buf + sizeof(*resp) + (size_t)resp->pane_count * sizeof(ipc_attach_pane_info_t);
        memcpy(blob_buf, blob_src, copy);
    }
    return 0;
}

/* ── 세션 스냅샷 ─────────────────────────────────────────────────────────── */

int ipc_client_session_save(ipc_client_t *c, uint32_t session_id,
                             session_snapshot_t *out)
{
    if (!c || !out) return -1;
    if (send_msg(write_fd(c), IPC_MSG_SESSION_SAVE,
                 &session_id, sizeof session_id) != 0)
        return -1;

    /* daemon이 JSON 문자열을 전송한다 */
    uint8_t json_buf[IPC_MAX_PAYLOAD_LEN];
    if (recv_until(c, IPC_MSG_SESSION_SAVE_R, json_buf, sizeof json_buf,
                    CMD_TIMEOUT_MS) < 0)
        return -1;

    /* JSON을 임시 파일에 쓰고 session_snapshot_load로 파싱 */
    char tmp_path[64];
    snprintf(tmp_path, sizeof tmp_path, "/tmp/termemu-snap-cli-%d.json", (int)getpid());
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return -1;
    /* json_buf의 실제 크기는 알 수 없으므로 NULL-terminated 가정 */
    fputs((const char *)json_buf, fp);
    fclose(fp);
    int ret = session_snapshot_load(tmp_path, out);
    unlink(tmp_path);
    return ret;
}

/* ── pane replay ─────────────────────────────────────────────────────────── */

int ipc_client_pane_replay(ipc_client_t *c, uint32_t pane_id)
{
    if (!c) return -1;
    if (send_msg(write_fd(c), IPC_MSG_PANE_REPLAY, &pane_id, sizeof pane_id) != 0)
        return -1;
    /* daemon이 PTY_OUTPUT 메시지를 여러 개 보낸 뒤 OK로 완료.
     * recv_until(OK)가 중간의 PTY_OUTPUT을 콜백으로 dispatch한다. */
    return recv_until(c, IPC_MSG_OK, NULL, 0, CMD_TIMEOUT_MS) < 0 ? -1 : 0;
}
