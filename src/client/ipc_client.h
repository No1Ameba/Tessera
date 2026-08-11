#ifndef TESSERA_IPC_CLIENT_H
#define TESSERA_IPC_CLIENT_H

/*
 * IPC client — connects to the tessera daemon over a Unix domain socket.
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
#include "../common/session_file.h"
#include "../common/ipc_proto.h"

typedef struct ipc_client ipc_client_t;

/* Callback invoked for each IPC_MSG_PTY_OUTPUT message. */
typedef void (*pty_output_cb_t)(uint32_t pane_id,
                                const uint8_t *data, size_t len,
                                void *user);

/* Callback invoked when a pane's PTY process has exited (IPC_MSG_PANE_EXITED). */
typedef void (*pane_exited_cb_t)(uint32_t pane_id, void *user);

/* Callback invoked when another client splits a pane (IPC_MSG_PANE_SPLIT_NOTIFY). */
typedef void (*pane_split_cb_t)(uint32_t session_id, uint32_t window_id,
                                uint32_t parent_pane_id, uint32_t new_pane_id,
                                uint16_t cols, uint16_t rows,
                                uint8_t direction, float ratio,
                                void *user);

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

ipc_client_t *ipc_client_create(pty_output_cb_t cb, void *user);
void          ipc_client_set_pane_exited_cb(ipc_client_t *c,
                                             pane_exited_cb_t cb, void *user);
void          ipc_client_set_pane_split_cb(ipc_client_t *c,
                                            pane_split_cb_t cb, void *user);
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
 * 원격에 tessera-bridge가 설치되어 있어야 한다.
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

/* ── 세션 attach ─────────────────────────────────────────────────────────── */

#define IPC_CLIENT_MAX_ATTACH_PANES  64

/* attach 응답에서 뽑아낸 window 하나. layout blob 은 결과의 blobs[] 안에 있다. */
typedef struct {
    uint32_t window_id;
    char     name[64];
    uint32_t blob_off;   /* ipc_attach_result_t.blobs 내 시작 오프셋 */
    uint16_t blob_len;   /* 0 = layout 정보 없음 */
} ipc_client_win_t;

/*
 * attach 결과 일체 — 세션의 모든 pane 과 window(+layout blob).
 * 호출자가 스택에 두고 쓸 수 있는 크기(약 12 KB)다.
 */
typedef struct {
    uint32_t session_id;
    char     session_name[64];
    uint32_t active_window_id;   /* 0 = 지정 없음 */

    int      pane_count;
    ipc_attach_pane_info_t panes[IPC_CLIENT_MAX_ATTACH_PANES];

    int      window_count;
    ipc_client_win_t windows[IPC_MAX_WINDOWS];

    uint8_t  blobs[IPC_ATTACH_BLOB_MAX];
    uint32_t blobs_len;
} ipc_attach_result_t;

/*
 * 기존 세션에 attach한다.
 * 세션의 전체 window 구성과 pane 목록을 받는다. PTY 출력 히스토리는
 * pane_slot 생성 후 ipc_client_pane_replay() 로 따로 요청한다.
 * @return 0 success, -1 failure.
 */
int ipc_client_session_attach(ipc_client_t *c, uint32_t session_id,
                               ipc_attach_result_t *out);

/*
 * pane의 PTY 출력 히스토리를 요청한다.
 * daemon이 링 버퍼 내용을 PTY_OUTPUT으로 전송한다.
 * pane_slot이 생성된 후에 호출해야 데이터가 정상 수신된다.
 */
int ipc_client_pane_replay(ipc_client_t *c, uint32_t pane_id);

/*
 * 세션 스냅샷을 daemon에서 가져온다.
 * daemon이 각 pane의 /proc/PID/cwd를 읽어 cwd를 채운다.
 */
int ipc_client_session_save(ipc_client_t *c, uint32_t session_id,
                             session_snapshot_t *out);

/* ── Window ──────────────────────────────────────────────────────────────── */

int ipc_client_window_create(ipc_client_t *c, uint32_t session_id,
                              const char *name, uint32_t *out_id);
int ipc_client_window_destroy(ipc_client_t *c, uint32_t session_id,
                               uint32_t window_id);
int ipc_client_window_focus(ipc_client_t *c, uint32_t session_id,
                             uint32_t window_id);

/* 현재 layout tree 를 직렬화한 blob 을 데몬에 업로드한다.
 * 데몬은 저장해 두었다가 재접속 시 SESSION_ATTACH_R 에 동봉한다. */
int ipc_client_window_layout(ipc_client_t *c, uint32_t session_id,
                              uint32_t window_id,
                              const uint8_t *blob, uint16_t blob_len);

/* ── Pane ────────────────────────────────────────────────────────────────── */

int ipc_client_pane_create(ipc_client_t *c, uint32_t session_id,
                            uint32_t window_id, uint16_t cols, uint16_t rows,
                            uint32_t *out_id);
/* split 컨텍스트와 함께 pane을 생성한다. parent_pane_id != 0 이면 데몬이
 * 다른 클라이언트들에 PANE_SPLIT_NOTIFY 를 브로드캐스트한다. */
int ipc_client_pane_create_split(ipc_client_t *c, uint32_t session_id,
                                  uint32_t window_id,
                                  uint16_t cols, uint16_t rows,
                                  uint32_t parent_pane_id,
                                  uint8_t direction, float ratio,
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

#endif /* TESSERA_IPC_CLIENT_H */
