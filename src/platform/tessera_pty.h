#ifndef TESSERA_PTY_H
#define TESSERA_PTY_H

/*
 * PTY 추상화 레이어
 *
 * 플랫폼별 구현:
 *   POSIX  — posix/pty_posix.c  (forkpty 기반)
 *   Windows — windows/pty_win.c  (ConPTY 기반, 미구현)
 *
 * 사용 흐름:
 *   1. pty_spawn()   — 자식 셸을 새 PTY에 붙여 실행
 *   2. pty_read()    — 마스터 fd에서 출력 데이터 읽기 (논블로킹)
 *   3. pty_write()   — 마스터 fd로 입력 데이터 쓰기
 *   4. pty_resize()  — 터미널 크기(SIGWINCH) 변경
 *   5. pty_close()   — 자식 종료 대기 + fd 닫기
 */

#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#  include <basetsd.h>
typedef SSIZE_T ssize_t;   /* MSVC 에는 ssize_t 가 없다 */
#else
#  include <sys/types.h>   /* pid_t, ssize_t */
#endif

/* ─── 타입 ───────────────────────────────────────────────────────────────── */

typedef struct {
#ifdef _WIN32
    /* ConPTY 백엔드(pty_win.c). HANDLE 은 헤더에서 windows.h 를 강제하지 않도록
     * void* 로 보관한다. */
    void   *hpcon;      /* HPCON 의사 콘솔 */
    void   *hproc;      /* 자식 프로세스 HANDLE */
    void   *hin;        /* 자식 stdin 쓰기 핸들 (우리가 write) */
    void   *hout;       /* 자식 stdout/err 읽기 핸들 (우리가 read, overlapped) */
    void   *hev;        /* hout 의 overlapped read 완료 대기용 이벤트 */
    int     master_fd;  /* hout 을 감싼 CRT fd — 데몬 이벤트 루프 등록용.
                         * 소유권은 CRT 에 있으므로 pty_close 는 _close 로 닫는다. */
    int     child_pid;  /* dwProcessId */
#else
    int     master_fd;  /* PTY 마스터 파일 디스크립터 */
    pid_t   child_pid;  /* 스폰된 자식 프로세스 PID */
#endif
} pty_t;

/* ─── API ────────────────────────────────────────────────────────────────── */

/*
 * 자식 셸을 PTY와 함께 스폰한다.
 *
 * @param out    성공 시 master_fd, child_pid 가 채워진다.
 * @param shell  실행할 셸 경로 (NULL 이면 $SHELL 또는 /bin/sh 사용).
 * @param cols   초기 열 수.
 * @param rows   초기 행 수.
 * @return  0 성공, -1 실패 (errno 설정).
 */
int pty_spawn(pty_t *out, const char *shell, uint16_t cols, uint16_t rows);

/*
 * PTY 마스터 fd에서 데이터를 읽는다. (논블로킹 — EAGAIN 이면 0 반환)
 *
 * @return 읽은 바이트 수, 0 (EAGAIN), -1 (오류/EOF).
 */
ssize_t pty_read(pty_t *pty, void *buf, size_t len);

/*
 * PTY 마스터 fd로 데이터를 쓴다.
 *
 * @return 쓴 바이트 수, -1 (오류).
 */
ssize_t pty_write(pty_t *pty, const void *buf, size_t len);

/*
 * 터미널 크기를 변경하고 자식 프로세스에 SIGWINCH 를 보낸다.
 *
 * @return 0 성공, -1 실패.
 */
int pty_resize(pty_t *pty, uint16_t cols, uint16_t rows);

/*
 * 자식 프로세스 종료를 기다리고 마스터 fd 를 닫는다.
 * SIGTERM → 100ms 대기 → SIGKILL 순서로 종료를 요청한다.
 *
 * @param exit_status  NULL 이 아니면 waitpid() 로 얻은 종료 상태를 채운다.
 */
void pty_close(pty_t *pty, int *exit_status);

/*
 * 마스터 fd 를 논블로킹 모드로 설정한다.
 * pty_spawn() 내부에서 자동 호출된다.
 *
 * @return 0 성공, -1 실패.
 */
int pty_set_nonblocking(int fd);

#endif /* TESSERA_PTY_H */
