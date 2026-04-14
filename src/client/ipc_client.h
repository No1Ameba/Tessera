#ifndef TERMEMU_IPC_CLIENT_H
#define TERMEMU_IPC_CLIENT_H

/*
 * IPC client — connects to the termemu daemon over a Unix domain socket.
 *
 * On-demand spawn:
 *   ipc_client_connect() tries to connect to the daemon socket. If the socket
 *   does not exist it forks the daemon binary and retries for up to 2 seconds.
 *
 * Threading model:
 *   All operations are synchronous and single-threaded.
 *   ipc_client_poll() must be called regularly from the event loop to receive
 *   asynchronous PTY output and dispatch it via the registered callback.
 */

#include <stdint.h>
#include <stddef.h>
#include "../common/ipc_proto.h"

typedef struct ipc_client ipc_client_t;

/* Callback invoked for each IPC_MSG_PTY_OUTPUT message. */
typedef void (*pty_output_cb_t)(uint32_t pane_id,
                                const uint8_t *data, size_t len,
                                void *user);

/* Callback invoked when a pane's PTY process has exited (IPC_MSG_PANE_EXITED). */
typedef void (*pane_exited_cb_t)(uint32_t pane_id, void *user);

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

ipc_client_t *ipc_client_create(pty_output_cb_t cb, void *user);
void          ipc_client_set_pane_exited_cb(ipc_client_t *c,
                                             pane_exited_cb_t cb, void *user);
void          ipc_client_destroy(ipc_client_t *c);

/*
 * Connect to the daemon (spawning it if necessary).
 * Sends HELLO and waits for HELLO_ACK.
 * @return 0 success, -1 failure.
 */
int  ipc_client_connect(ipc_client_t *c);

/*
 * SSH를 통해 원격 daemon에 연결한다.
 * ssh_target: "user@host" 형태.
 * 원격에 termemu-bridge가 설치되어 있어야 한다.
 * @return 0 success, -1 failure.
 */
int  ipc_client_connect_remote(ipc_client_t *c, const char *ssh_target);

void ipc_client_disconnect(ipc_client_t *c);

/* ── Session ─────────────────────────────────────────────────────────────── */

int ipc_client_session_create(ipc_client_t *c, const char *name,
                               uint32_t *out_id);
int ipc_client_session_destroy(ipc_client_t *c, uint32_t session_id);
int ipc_client_session_list(ipc_client_t *c,
                             ipc_session_info_t *buf, int max, int *out_count);

/*
 * 기존 세션에 attach한다.
 * pane 목록을 받고, daemon이 PTY 출력 히스토리를 replay한다.
 * @return 0 success, -1 failure.
 */
int ipc_client_session_attach(ipc_client_t *c, uint32_t session_id,
                               ipc_attach_pane_info_t *panes_out, int max_panes,
                               int *out_count);

/*
 * pane의 PTY 출력 히스토리를 요청한다.
 * daemon이 링 버퍼 내용을 PTY_OUTPUT으로 전송한다.
 * pane_slot이 생성된 후에 호출해야 데이터가 정상 수신된다.
 */
int ipc_client_pane_replay(ipc_client_t *c, uint32_t pane_id);

/* ── Window ──────────────────────────────────────────────────────────────── */

int ipc_client_window_create(ipc_client_t *c, uint32_t session_id,
                              const char *name, uint32_t *out_id);
int ipc_client_window_destroy(ipc_client_t *c, uint32_t session_id,
                               uint32_t window_id);
int ipc_client_window_focus(ipc_client_t *c, uint32_t session_id,
                             uint32_t window_id);

/* ── Pane ────────────────────────────────────────────────────────────────── */

int ipc_client_pane_create(ipc_client_t *c, uint32_t session_id,
                            uint32_t window_id, uint16_t cols, uint16_t rows,
                            uint32_t *out_id);
int ipc_client_pane_destroy(ipc_client_t *c, uint32_t session_id,
                             uint32_t window_id, uint32_t pane_id);
int ipc_client_pane_resize(ipc_client_t *c, uint32_t session_id,
                            uint32_t window_id, uint32_t pane_id,
                            uint16_t cols, uint16_t rows);
int ipc_client_pane_focus(ipc_client_t *c, uint32_t session_id,
                           uint32_t window_id, uint32_t pane_id);

/* ── Data ────────────────────────────────────────────────────────────────── */

int ipc_client_pty_input(ipc_client_t *c, uint32_t pane_id,
                          const uint8_t *data, size_t len);

/*
 * Receive pending messages (non-blocking when timeout_ms == 0).
 * Dispatches PTY output via the registered callback.
 * @return number of messages processed, -1 on connection error.
 */
int ipc_client_poll(ipc_client_t *c, int timeout_ms);

#endif /* TERMEMU_IPC_CLIENT_H */
