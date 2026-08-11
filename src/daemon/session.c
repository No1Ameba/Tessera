#include "session.h"
#include "../common/mono_time.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t mono_ms_now(void) {
    return tessera_mono_ms();
}

/* ─── 내부 헬퍼: ID 발급 ─────────────────────────────────────────────────── */

static uint32_t next_id(session_manager_t *mgr) {
    return ++mgr->next_id;  /* 0은 "유효하지 않음"으로 예약 */
}

/* ─── 매니저 ─────────────────────────────────────────────────────────────── */

void session_manager_init(session_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
}

void session_manager_destroy(session_manager_t *mgr) {
    while (mgr->head)
        session_destroy(mgr, mgr->head);
}

/* ─── 세션 ───────────────────────────────────────────────────────────────── */

session_t *session_create(session_manager_t *mgr, const char *name) {
    if (!mgr || !name) return NULL;

    session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->id = next_id(mgr);
    strncpy(s->name, name, SESSION_NAME_MAX - 1);
    /* 생성 직후에도 5분 유예를 주기 위해 now 로 초기화. */
    s->last_detach_ms = mono_ms_now();
    s->attach_count   = 0;

    /* 리스트 앞에 삽입 */
    s->next   = mgr->head;
    mgr->head = s;
    mgr->count++;

    return s;
}

session_t *session_find_by_name(const session_manager_t *mgr, const char *name) {
    if (!mgr || !name) return NULL;
    for (session_t *s = mgr->head; s; s = s->next)
        if (strncmp(s->name, name, SESSION_NAME_MAX) == 0) return s;
    return NULL;
}

session_t *session_find_by_id(const session_manager_t *mgr, uint32_t id) {
    if (!mgr) return NULL;
    for (session_t *s = mgr->head; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

void session_destroy(session_manager_t *mgr, session_t *s) {
    if (!mgr || !s) return;

    /* 하위 윈도우 전체 해제 */
    while (s->windows)
        window_destroy(s, s->windows);

    /* 매니저 리스트에서 제거 */
    if (mgr->head == s) {
        mgr->head = s->next;
    } else {
        for (session_t *cur = mgr->head; cur; cur = cur->next) {
            if (cur->next == s) {
                cur->next = s->next;
                break;
            }
        }
    }

    mgr->count--;
    free(s);
}

/* ─── 윈도우 ─────────────────────────────────────────────────────────────── */

window_t *window_create(session_t *s, const char *name) {
    if (!s) return NULL;

    window_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    /* 윈도우 ID는 매니저를 통하지 않으므로 세션의 윈도우 수로 부여.
     * 실제 운영에서는 매니저의 next_id 를 써야 하나, 이 모듈은
     * 독립적으로 테스트 가능하도록 간단하게 처리한다. */
    w->id = (uint32_t)(s->window_count + 1) + (s->id << 16);
    if (name)
        strncpy(w->name, name, WINDOW_NAME_MAX - 1);
    w->parent = s;

    /* 리스트 끝에 추가 (윈도우는 순서가 중요) */
    if (!s->windows) {
        s->windows = w;
    } else {
        window_t *cur = s->windows;
        while (cur->next) cur = cur->next;
        cur->next = w;
    }

    /* 첫 윈도우면 자동으로 active */
    if (!s->active_window)
        s->active_window = w;

    s->window_count++;
    return w;
}

window_t *window_find_by_id(const session_t *s, uint32_t id) {
    if (!s) return NULL;
    for (window_t *w = s->windows; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

void window_destroy(session_t *s, window_t *w) {
    if (!s || !w) return;

    /* 하위 페인 전체 해제 */
    while (w->panes)
        pane_destroy(w, w->panes);

    free(w->layout_blob);
    w->layout_blob = NULL;
    w->layout_blob_len = 0;

    /* active_window 갱신 */
    if (s->active_window == w) {
        /* 다음 또는 이전 윈도우로 포커스 이동 */
        s->active_window = w->next ? w->next : NULL;
        if (!s->active_window && s->windows != w)
            s->active_window = s->windows;
    }

    /* 리스트에서 제거 */
    if (s->windows == w) {
        s->windows = w->next;
    } else {
        for (window_t *cur = s->windows; cur; cur = cur->next) {
            if (cur->next == w) {
                cur->next = w->next;
                break;
            }
        }
    }

    s->window_count--;
    free(w);
}

void window_set_active(session_t *s, window_t *w) {
    if (!s || !w) return;
    s->active_window = w;
}

/* ─── 페인 ───────────────────────────────────────────────────────────────── */

pane_t *pane_create(window_t *w, uint16_t cols, uint16_t rows) {
    if (!w) return NULL;

    pane_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->id     = (uint32_t)(w->pane_count + 1) + (w->id << 8);
    p->pty_fd = -1;
    p->pid    = -1;
    p->cols   = cols;
    p->rows   = rows;
    p->parent = w;

    /* 리스트 끝에 추가 */
    if (!w->panes) {
        w->panes = p;
    } else {
        pane_t *cur = w->panes;
        while (cur->next) cur = cur->next;
        cur->next = p;
    }

    /* 첫 페인이면 자동으로 active */
    if (!w->active_pane)
        w->active_pane = p;

    w->pane_count++;
    return p;
}

pane_t *pane_find_by_id(const window_t *w, uint32_t id) {
    if (!w) return NULL;
    for (pane_t *p = w->panes; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}

void pane_destroy(window_t *w, pane_t *p) {
    if (!w || !p) return;

    /* active_pane 갱신 */
    if (w->active_pane == p) {
        w->active_pane = p->next ? p->next : NULL;
        if (!w->active_pane && w->panes != p)
            w->active_pane = w->panes;
    }

    /* 리스트에서 제거 */
    if (w->panes == p) {
        w->panes = p->next;
    } else {
        for (pane_t *cur = w->panes; cur; cur = cur->next) {
            if (cur->next == p) {
                cur->next = p->next;
                break;
            }
        }
    }

    w->pane_count--;
    free(p);
}

void pane_set_active(window_t *w, pane_t *p) {
    if (!w || !p) return;
    w->active_pane = p;
}

void pane_resize(pane_t *p, uint16_t cols, uint16_t rows) {
    if (!p) return;
    p->cols = cols;
    p->rows = rows;
}
