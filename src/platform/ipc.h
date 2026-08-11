#ifndef TESSERA_IPC_H
#define TESSERA_IPC_H

/*
 * IPC 소켓 추상화 — 플랫폼 저수준 래퍼
 *
 * 프로토콜 무관. OS별 소켓/파이프 설정만 담당.
 *
 * 플랫폼별 구현:
 *   POSIX   — posix/ipc_posix.c  (Unix Domain Socket)
 *   Windows — windows/ipc_win.c  (Named Pipe)
 */

#include <stddef.h>

#ifdef _WIN32
#  include <basetsd.h>
#  ifndef TESSERA_SSIZE_T_DEFINED
#    define TESSERA_SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;   /* MSVC 에는 ssize_t 가 없다 */
#  endif
#else
#  include <sys/types.h>   /* ssize_t */
#endif

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
 * 대기 중인 클라이언트 연결을 수락한다.
 *
 * @param listen_fd  [in/out] listen fd. **Windows 에서는 값이 바뀔 수 있다.**
 * @return           연결된 클라이언트 fd (>= 0), 실패 시 -1.
 *
 * POSIX 는 accept(2) 가 새 fd 를 주고 listen fd 는 그대로다 — *listen_fd 는
 * 변하지 않는다.
 *
 * Windows 의 Named Pipe 는 모델이 다르다. 연결이 도착하면 **대기하던 인스턴스
 * 자체가** 그 연결이 되므로, 그것을 클라이언트 fd 로 돌려주고 다음 연결을 받을
 * 새 인스턴스를 만들어 *listen_fd 에 넣는다.
 *
 * 따라서 호출자는 수락 후 *listen_fd 가 바뀌었는지 보고, 바뀌었다면
 *   - 반환된 fd(= 이전 listen fd)를 evloop_mod(..., EV_READ) 로 전환하고
 *   - 새 *listen_fd 를 evloop_add(..., EV_ACCEPT) 로 등록해야 한다.
 * 바뀌지 않았다면(POSIX) 반환된 fd 를 EV_READ 로 새로 등록하면 된다.
 */
int ipc_accept_client(int *listen_fd);

/*
 * 소켓 fd 를 닫고 path 의 소켓 파일을 삭제한다.
 * path 가 NULL 이면 unlink 를 건너뛴다.
 */
void ipc_close_socket(int fd, const char *path);

/* ─── 클라이언트 연결 ───────────────────────────────────────────────────── */

/*
 * 데몬에 연결한다.
 *
 * @param path  ipc_socket_path() 가 준 소켓 경로 / 파이프 이름.
 * @return      연결된 fd (>= 0), 실패 시 -1.
 */
int ipc_connect(const char *path);

/*
 * 소켓/파이프가 접속 가능한 상태가 될 때까지 기다린다.
 * 데몬을 막 스폰한 뒤 준비될 때까지 대기하는 용도.
 *
 * @return  1 준비됨, 0 타임아웃, -1 오류.
 */
int ipc_wait_ready(const char *path, int timeout_ms);

/* ─── 연결 I/O ──────────────────────────────────────────────────────────── */

/*
 * 연결된 fd 를 읽고 쓴다.
 *
 * read()/write() 를 직접 부르지 말고 반드시 이 함수를 쓸 것. Windows 의 Named
 * Pipe 는 FILE_FLAG_OVERLAPPED 로 열리는데, overlapped 핸들에 OVERLAPPED 인자
 * 없이 ReadFile/WriteFile 을 부르는 것은 정의되지 않은 동작이다(CRT 의 _read/
 * _write 가 정확히 그렇게 한다). 이 래퍼가 플랫폼별 처리를 감춘다.
 *
 * ipc_read  @return  >0 읽은 바이트 수, 0 상대측 연결 종료(EOF),
 *                    -1 오류 (errno==EAGAIN 이면 아직 데이터 없음 — 재시도).
 * ipc_write @return  >0 쓴 바이트 수(부분 쓰기 가능), -1 오류
 *                    (errno==EAGAIN 이면 지금은 못 씀 — 재시도).
 */
ssize_t ipc_read(int fd, void *buf, size_t len);
ssize_t ipc_write(int fd, const void *buf, size_t len);

/*
 * fd 가 쓰기 가능해질 때까지 기다린다(백프레셔 처리용).
 *
 * @return  1 쓰기 가능, 0 타임아웃, -1 오류.
 *
 * Windows 에서는 대기가 ipc_write 내부의 overlapped 완료 대기로 이미 이뤄지므로
 * 항상 즉시 1 을 반환한다.
 */
int ipc_wait_writable(int fd, int timeout_ms);

/*
 * fd 에 읽을 데이터가 생길 때까지 기다린다.
 *
 * @param timeout_ms  음수면 무한 대기.
 * @return  1 읽기 가능, 0 타임아웃, -1 오류(상대측 종료 포함).
 */
int ipc_wait_readable(int fd, int timeout_ms);

/* 연결 fd 를 닫는다 (listen fd 가 아닌 클라이언트 연결용). */
void ipc_close_conn(int fd);

#endif /* TESSERA_IPC_H */
