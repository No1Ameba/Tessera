#ifndef TERMEMU_IPC_SERVER_H
#define TERMEMU_IPC_SERVER_H

/*
 * IPC 서버 — epoll 기반 이벤트 루프
 *
 * 클라이언트 연결 수락, ipc_proto.h 메시지 파싱/디스패치,
 * PTY I/O 통합을 담당한다.
 *
 * 사용 흐름:
 *   1. ipc_server_create()  — 서버 객체 할당
 *   2. ipc_server_listen()  — 소켓 바인드 + epoll 준비
 *   3. ipc_server_run()     — 블로킹 이벤트 루프 (SIGTERM/SIGINT 시 반환)
 *   4. ipc_server_destroy() — 자원 해제
 */

#include "session.h"

/* ─── 불투명 타입 ─────────────────────────────────────────────────────────── */

typedef struct ipc_server ipc_server_t;

/* ─── API ────────────────────────────────────────────────────────────────── */

/*
 * 서버 객체를 할당하고 session_mgr 에 연결한다.
 * 실패 시 NULL.
 */
ipc_server_t *ipc_server_create(session_manager_t *session_mgr);

/*
 * 소켓을 바인드하고 epoll 인스턴스를 초기화한다.
 * @return  0 성공, -1 실패.
 */
int ipc_server_listen(ipc_server_t *srv);

/*
 * 블로킹 epoll 이벤트 루프. SIGTERM/SIGINT 수신 또는
 * ipc_server_shutdown() 호출 시 반환한다.
 * @return  0 정상 종료, -1 오류.
 */
int ipc_server_run(ipc_server_t *srv);

/*
 * 이벤트 루프를 종료하도록 플래그를 설정한다.
 * 시그널 핸들러 또는 다른 스레드에서 안전하게 호출 가능.
 */
void ipc_server_shutdown(ipc_server_t *srv);

/*
 * 서버 소켓/epoll fd 를 닫고 객체를 해제한다.
 * ipc_server_run() 반환 후 호출해야 한다.
 */
void ipc_server_destroy(ipc_server_t *srv);

#endif /* TERMEMU_IPC_SERVER_H */
