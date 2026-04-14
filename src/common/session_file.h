#ifndef TERMEMU_SESSION_FILE_H
#define TERMEMU_SESSION_FILE_H

/*
 * 세션 스냅샷 직렬화/역직렬화
 *
 * 세션의 구조(윈도우, pane, 크기, 작업 디렉토리)를 JSON으로 저장/복원한다.
 * 프로세스 상태(실행 중인 명령 등)는 복원하지 않는다.
 */

#include <stdint.h>
#include <limits.h>

#define SNAP_MAX_WINDOWS  16
#define SNAP_MAX_PANES    64
#define SNAP_NAME_MAX     64
#define SNAP_CWD_MAX      256

typedef struct {
    uint16_t cols, rows;
    char     cwd[SNAP_CWD_MAX];
} snap_pane_t;

typedef struct {
    char        name[SNAP_NAME_MAX];
    int         pane_count;
    int         active_pane;    /* 0-based index */
    snap_pane_t panes[SNAP_MAX_PANES];
} snap_window_t;

typedef struct {
    char           name[SNAP_NAME_MAX];
    int            window_count;
    int            active_window;  /* 0-based index */
    snap_window_t  windows[SNAP_MAX_WINDOWS];
} session_snapshot_t;

/*
 * 스냅샷을 JSON 파일로 저장한다.
 * @return 0 성공, -1 실패.
 */
int session_snapshot_save(const char *path, const session_snapshot_t *snap);

/*
 * JSON 파일에서 스냅샷을 로드한다.
 * @return 0 성공, -1 실패.
 */
int session_snapshot_load(const char *path, session_snapshot_t *snap);

#endif /* TERMEMU_SESSION_FILE_H */
