#ifndef TERMEMU_SESSION_H
#define TERMEMU_SESSION_H

/*
 * 세션 관리 — Session / Window / Pane 3계층 트리
 *
 * 계층 구조:
 *   session_manager_t        (데몬 전역, 세션 목록 보유)
 *   └── session_t  "work"    (이름 지정 가능)
 *       └── window_t  [0]    (탭 단위)
 *           └── pane_t  [0]  (실제 PTY 인스턴스)
 *
 * PTY 스폰은 platform 레이어가 담당하며, 이 모듈은
 * pane_t.pty_fd / pane_t.pid 슬롯만 제공한다.
 * 값이 -1 이면 아직 스폰되지 않은 상태다.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ─── 상수 ──────────────────────────────────────────────────────────────── */

#define SESSION_NAME_MAX  64
#define WINDOW_NAME_MAX   64

/* ─── Pane ──────────────────────────────────────────────────────────────── */

typedef struct pane {
    uint32_t       id;
    int            pty_fd;   /* PTY 파일 디스크립터. 스폰 전: -1 */
    int            pid;      /* 자식 프로세스 PID.    스폰 전: -1 */
    uint16_t       cols;     /* 열 수 */
    uint16_t       rows;     /* 행 수 */

    struct window *parent;
    struct pane   *next;     /* 같은 윈도우 내 다음 페인 (연결 리스트) */
} pane_t;

/* ─── Window ────────────────────────────────────────────────────────────── */

typedef struct window {
    uint32_t        id;
    char            name[WINDOW_NAME_MAX];

    pane_t         *panes;        /* 페인 연결 리스트 헤드 */
    pane_t         *active_pane;  /* 현재 포커스된 페인 */
    size_t          pane_count;

    /* 클라이언트가 마지막으로 업로드한 layout tree blob (재접속 복원용).
     * 데몬은 포맷을 해석하지 않고 바이트 그대로 저장/재전송한다. */
    uint8_t        *layout_blob;
    uint16_t        layout_blob_len;

    struct session *parent;
    struct window  *next;         /* 같은 세션 내 다음 윈도우 */
} window_t;

/* ─── Session ───────────────────────────────────────────────────────────── */

typedef struct session {
    uint32_t        id;
    char            name[SESSION_NAME_MAX];

    window_t       *windows;        /* 윈도우 연결 리스트 헤드 */
    window_t       *active_window;  /* 현재 포커스된 윈도우 */
    size_t          window_count;

    /* 수명 정책: 모든 클라이언트가 detach 된 후 일정 시간(IPC_SERVER_SESSION_IDLE_MS)
     * 동안 재접속이 없으면 데몬이 자동으로 session_destroy 한다. */
    int             attach_count;     /* 현재 세션에 attach 된 클라이언트 수 */
    int64_t         last_detach_ms;   /* attach_count 가 0으로 떨어진 시각 (ms) */

    struct session *next;           /* 매니저 내 다음 세션 */
} session_t;

/* ─── Session Manager ───────────────────────────────────────────────────── */

typedef struct {
    session_t *head;     /* 세션 연결 리스트 헤드 */
    size_t     count;    /* 전체 세션 수 */
    uint32_t   next_id;  /* 단조 증가 ID 카운터 */
} session_manager_t;

/* ─── 매니저 API ─────────────────────────────────────────────────────────── */

void session_manager_init(session_manager_t *mgr);

/* 모든 세션/윈도우/페인을 해제한다. */
void session_manager_destroy(session_manager_t *mgr);

/* ─── 세션 API ───────────────────────────────────────────────────────────── */

/* 세션을 생성하고 매니저에 등록한다. 실패 시 NULL. */
session_t *session_create(session_manager_t *mgr, const char *name);

/* 이름으로 세션 검색 (없으면 NULL). */
session_t *session_find_by_name(const session_manager_t *mgr, const char *name);

/* ID로 세션 검색 (없으면 NULL). */
session_t *session_find_by_id(const session_manager_t *mgr, uint32_t id);

/* 세션을 매니저에서 제거하고 하위 트리 전체를 해제한다. */
void session_destroy(session_manager_t *mgr, session_t *s);

/* ─── 윈도우 API ─────────────────────────────────────────────────────────── */

/* 윈도우를 생성하고 세션에 추가한다. 실패 시 NULL. */
window_t *window_create(session_t *s, const char *name);

/* ID로 윈도우 검색 (없으면 NULL). */
window_t *window_find_by_id(const session_t *s, uint32_t id);

/* 윈도우를 세션에서 제거하고 하위 페인 전체를 해제한다. */
void window_destroy(session_t *s, window_t *w);

/* 해당 윈도우를 세션의 active_window 로 설정한다. */
void window_set_active(session_t *s, window_t *w);

/* ─── 페인 API ───────────────────────────────────────────────────────────── */

/* 페인을 생성하고 윈도우에 추가한다. 실패 시 NULL. */
pane_t *pane_create(window_t *w, uint16_t cols, uint16_t rows);

/* ID로 페인 검색 (없으면 NULL). */
pane_t *pane_find_by_id(const window_t *w, uint32_t id);

/* 페인을 윈도우에서 제거하고 해제한다. */
void pane_destroy(window_t *w, pane_t *p);

/* 해당 페인을 윈도우의 active_pane 으로 설정한다. */
void pane_set_active(window_t *w, pane_t *p);

/* 페인 크기를 변경한다. */
void pane_resize(pane_t *p, uint16_t cols, uint16_t rows);

#endif /* TERMEMU_SESSION_H */
