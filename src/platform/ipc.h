#ifndef TESSERA_IPC_H
#define TESSERA_IPC_H

/*
 * IPC 소켓 추상화 — 플랫폼 저수준 래퍼
 *
 * 프로토콜 무관. OS별 소켓/파이프 설정만 담당.
 *
 * 플랫폼별 구현:
 *   POSIX   — posix/ipc_posix.c  (Unix Domain Socket)
 *   Windows — windows/ipc_win.c  (Named Pipe, 미구현)
 */

#include <stddef.h>

/* ─── 소켓 경로 ──────────────────────────────────────────────────────────── */

/*
 * 현재 사용자 uid 기반 소켓 경로를 buf 에 쓴다.
 * POSIX: "/tmp/tessera-<uid>.sock"
 *
 * @return  0 성공, -1 실패 (buf 너무 짧음 등)
 */
int ipc_socket_path(char *buf, size_t buflen);

/* ─── 서버 소켓 ──────────────────────────────────────────────────────────── */

/*
 * Unix domain socket(또는 Named Pipe)을 생성, 바인드, listen 한다.
 *
 * @param path  소켓 파일 경로 (POSIX) 또는 파이프 이름 (Windows).
 * @return      listening fd (>= 0), 실패 시 -1 (errno 설정).
 */
int ipc_listen_socket(const char *path);

/*
 * 대기 중인 클라이언트 연결을 수락하고 O_NONBLOCK 을 설정한다.
 *
 * @return  연결된 클라이언트 fd (>= 0), 실패 시 -1.
 */
int ipc_accept_client(int listen_fd);

/*
 * 소켓 fd 를 닫고 path 의 소켓 파일을 삭제한다.
 * path 가 NULL 이면 unlink 를 건너뛴다.
 */
void ipc_close_socket(int fd, const char *path);

#endif /* TESSERA_IPC_H */
