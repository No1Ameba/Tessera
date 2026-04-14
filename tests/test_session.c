#include "../src/daemon/session.h"
#include <stdio.h>
#include <string.h>

/* ─── 미니 테스트 프레임워크 ─────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do {                                     \
    if (cond) { printf("  PASS: %s\n", msg); g_pass++;            \
    } else    { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

#define TEST(name) printf("\n[%s]\n", name)

/* ─── session_manager ────────────────────────────────────────────────────── */

static void test_manager_init(void) {
    TEST("session_manager_init");
    session_manager_t mgr;
    session_manager_init(&mgr);

    ASSERT(mgr.head    == NULL, "head = NULL");
    ASSERT(mgr.count   == 0,   "count = 0");
    ASSERT(mgr.next_id == 0,   "next_id = 0");
}

static void test_manager_destroy_empty(void) {
    TEST("session_manager_destroy (빈 상태)");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_manager_destroy(&mgr);  /* 크래시 없어야 함 */
    ASSERT(mgr.count == 0, "destroy 후 count = 0");
}

/* ─── session CRUD ───────────────────────────────────────────────────────── */

static void test_session_create(void) {
    TEST("session_create");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_t *s = session_create(&mgr, "work");

    ASSERT(s != NULL,                          "반환값 != NULL");
    ASSERT(s->id != 0,                         "id != 0");
    ASSERT(strcmp(s->name, "work") == 0,       "name = \"work\"");
    ASSERT(s->window_count == 0,               "window_count = 0");
    ASSERT(s->windows == NULL,                 "windows = NULL");
    ASSERT(s->active_window == NULL,           "active_window = NULL");
    ASSERT(mgr.count == 1,                     "mgr.count = 1");

    session_manager_destroy(&mgr);
}

static void test_session_create_multiple(void) {
    TEST("session_create: 여러 세션, ID 고유성");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_t *s1 = session_create(&mgr, "alpha");
    session_t *s2 = session_create(&mgr, "beta");
    session_t *s3 = session_create(&mgr, "gamma");

    ASSERT(mgr.count == 3,     "count = 3");
    ASSERT(s1->id != s2->id,   "s1.id != s2.id");
    ASSERT(s2->id != s3->id,   "s2.id != s3.id");
    ASSERT(s1->id != s3->id,   "s1.id != s3.id");

    session_manager_destroy(&mgr);
}

static void test_session_find_by_name(void) {
    TEST("session_find_by_name");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_create(&mgr, "work");
    session_create(&mgr, "personal");

    ASSERT(session_find_by_name(&mgr, "work")     != NULL, "\"work\" 찾음");
    ASSERT(session_find_by_name(&mgr, "personal") != NULL, "\"personal\" 찾음");
    ASSERT(session_find_by_name(&mgr, "missing")  == NULL, "없는 이름 → NULL");
    ASSERT(session_find_by_name(&mgr, NULL)        == NULL, "NULL → NULL");

    session_manager_destroy(&mgr);
}

static void test_session_find_by_id(void) {
    TEST("session_find_by_id");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_t *s = session_create(&mgr, "s1");
    uint32_t id  = s->id;

    ASSERT(session_find_by_id(&mgr, id)  == s,    "ID로 찾음");
    ASSERT(session_find_by_id(&mgr, 999) == NULL, "없는 ID → NULL");

    session_manager_destroy(&mgr);
}

static void test_session_destroy(void) {
    TEST("session_destroy");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_t *s1 = session_create(&mgr, "keep");
    session_t *s2 = session_create(&mgr, "remove");
    uint32_t   id2 = s2->id;

    session_destroy(&mgr, s2);

    ASSERT(mgr.count == 1,                         "count = 1");
    ASSERT(session_find_by_id(&mgr, id2) == NULL,  "삭제된 세션 못 찾음");
    ASSERT(session_find_by_name(&mgr, "keep") == s1, "남은 세션 유지");

    session_manager_destroy(&mgr);
}

/* ─── window CRUD ────────────────────────────────────────────────────────── */

static void test_window_create(void) {
    TEST("window_create");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");

    window_t *w = window_create(s, "main");

    ASSERT(w != NULL,                       "반환값 != NULL");
    ASSERT(strcmp(w->name, "main") == 0,    "name = \"main\"");
    ASSERT(w->parent == s,                  "parent = s");
    ASSERT(w->pane_count == 0,              "pane_count = 0");
    ASSERT(s->window_count == 1,            "s.window_count = 1");
    ASSERT(s->active_window == w,           "첫 윈도우 → active");

    session_manager_destroy(&mgr);
}

static void test_window_create_multiple(void) {
    TEST("window_create: 여러 윈도우, active 추적");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");

    window_t *w1 = window_create(s, "w1");
    window_t *w2 = window_create(s, "w2");
    window_t *w3 = window_create(s, "w3");

    ASSERT(s->window_count == 3,  "window_count = 3");
    ASSERT(s->active_window == w1, "active_window = w1 (첫 번째)");

    window_set_active(s, w3);
    ASSERT(s->active_window == w3, "active_window = w3 (변경 후)");

    (void)w2;
    session_manager_destroy(&mgr);
}

static void test_window_find_by_id(void) {
    TEST("window_find_by_id");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");

    window_t *w  = window_create(s, "win");
    uint32_t  id = w->id;

    ASSERT(window_find_by_id(s, id)  == w,    "ID로 찾음");
    ASSERT(window_find_by_id(s, 999) == NULL, "없는 ID → NULL");

    session_manager_destroy(&mgr);
}

static void test_window_destroy(void) {
    TEST("window_destroy: active_window 자동 갱신");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s  = session_create(&mgr, "s");
    window_t  *w1 = window_create(s, "w1");
    window_t  *w2 = window_create(s, "w2");

    window_set_active(s, w1);
    window_destroy(s, w1);

    ASSERT(s->window_count == 1,        "window_count = 1");
    ASSERT(s->active_window != w1,      "active != 삭제된 w1");
    ASSERT(s->active_window == w2,      "active = w2 (다음 윈도우)");

    session_manager_destroy(&mgr);
}

static void test_window_destroy_last(void) {
    TEST("window_destroy: 마지막 윈도우 삭제 → active = NULL");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");
    window_t  *w = window_create(s, "only");

    window_destroy(s, w);

    ASSERT(s->window_count == 0,      "window_count = 0");
    ASSERT(s->active_window == NULL,  "active_window = NULL");
    ASSERT(s->windows == NULL,        "windows = NULL");

    session_manager_destroy(&mgr);
}

/* ─── pane CRUD ──────────────────────────────────────────────────────────── */

static void test_pane_create(void) {
    TEST("pane_create");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");
    window_t  *w = window_create(s, "w");

    pane_t *p = pane_create(w, 80, 24);

    ASSERT(p != NULL,           "반환값 != NULL");
    ASSERT(p->pty_fd == -1,     "pty_fd = -1 (스폰 전)");
    ASSERT(p->pid    == -1,     "pid    = -1 (스폰 전)");
    ASSERT(p->cols   == 80,     "cols = 80");
    ASSERT(p->rows   == 24,     "rows = 24");
    ASSERT(p->parent == w,      "parent = w");
    ASSERT(w->pane_count == 1,  "w.pane_count = 1");
    ASSERT(w->active_pane == p, "첫 페인 → active");

    session_manager_destroy(&mgr);
}

static void test_pane_create_multiple(void) {
    TEST("pane_create: 여러 페인, active 추적");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s  = session_create(&mgr, "s");
    window_t  *w  = window_create(s, "w");

    pane_t *p1 = pane_create(w, 80, 24);
    pane_t *p2 = pane_create(w, 40, 24);
    pane_t *p3 = pane_create(w, 40, 24);

    ASSERT(w->pane_count == 3,   "pane_count = 3");
    ASSERT(w->active_pane == p1, "active = p1 (첫 번째)");

    pane_set_active(w, p2);
    ASSERT(w->active_pane == p2, "active = p2 (변경 후)");

    (void)p3;
    session_manager_destroy(&mgr);
}

static void test_pane_find_by_id(void) {
    TEST("pane_find_by_id");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");
    window_t  *w = window_create(s, "w");

    pane_t   *p  = pane_create(w, 80, 24);
    uint32_t  id = p->id;

    ASSERT(pane_find_by_id(w, id)  == p,    "ID로 찾음");
    ASSERT(pane_find_by_id(w, 999) == NULL, "없는 ID → NULL");

    session_manager_destroy(&mgr);
}

static void test_pane_destroy(void) {
    TEST("pane_destroy: active_pane 자동 갱신");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s  = session_create(&mgr, "s");
    window_t  *w  = window_create(s, "w");
    pane_t    *p1 = pane_create(w, 80, 24);
    pane_t    *p2 = pane_create(w, 80, 24);

    pane_set_active(w, p1);
    pane_destroy(w, p1);

    ASSERT(w->pane_count == 1,       "pane_count = 1");
    ASSERT(w->active_pane == p2,     "active = p2 (다음 페인)");

    session_manager_destroy(&mgr);
}

static void test_pane_destroy_last(void) {
    TEST("pane_destroy: 마지막 페인 삭제 → active = NULL");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");
    window_t  *w = window_create(s, "w");
    pane_t    *p = pane_create(w, 80, 24);

    pane_destroy(w, p);

    ASSERT(w->pane_count == 0,     "pane_count = 0");
    ASSERT(w->active_pane == NULL, "active_pane = NULL");
    ASSERT(w->panes == NULL,       "panes = NULL");

    session_manager_destroy(&mgr);
}

static void test_pane_resize(void) {
    TEST("pane_resize");
    session_manager_t mgr;
    session_manager_init(&mgr);
    session_t *s = session_create(&mgr, "s");
    window_t  *w = window_create(s, "w");
    pane_t    *p = pane_create(w, 80, 24);

    pane_resize(p, 120, 40);

    ASSERT(p->cols == 120, "cols = 120");
    ASSERT(p->rows == 40,  "rows = 40");

    session_manager_destroy(&mgr);
}

/* ─── 계층 통합 테스트 ───────────────────────────────────────────────────── */

static void test_cascade_destroy(void) {
    TEST("session_destroy: 하위 윈도우/페인 연쇄 해제 (메모리 누수 없어야 함)");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_t *s  = session_create(&mgr, "big");
    window_t  *w1 = window_create(s, "w1");
    window_t  *w2 = window_create(s, "w2");
    pane_create(w1, 80, 24);
    pane_create(w1, 80, 24);
    pane_create(w2, 80, 24);

    session_destroy(&mgr, s);

    ASSERT(mgr.count == 0, "세션 제거됨");
    /* 여기까지 크래시 없으면 연쇄 해제 성공 */
    ASSERT(1, "연쇄 해제 크래시 없음");

    session_manager_destroy(&mgr);
}

static void test_full_tree(void) {
    TEST("전체 트리: 2세션 × 2윈도우 × 2페인");
    session_manager_t mgr;
    session_manager_init(&mgr);

    session_t *sa = session_create(&mgr, "alpha");
    session_t *sb = session_create(&mgr, "beta");

    window_t *wa1 = window_create(sa, "alpha-w1");
    window_t *wa2 = window_create(sa, "alpha-w2");
    window_t *wb1 = window_create(sb, "beta-w1");

    pane_create(wa1, 80, 24);
    pane_create(wa1, 80, 24);
    pane_create(wa2, 120, 40);
    pane_create(wb1, 80, 24);

    ASSERT(mgr.count == 2,        "세션 수 = 2");
    ASSERT(sa->window_count == 2, "alpha 윈도우 수 = 2");
    ASSERT(sb->window_count == 1, "beta 윈도우 수 = 1");
    ASSERT(wa1->pane_count == 2,  "alpha-w1 페인 수 = 2");
    ASSERT(wa2->pane_count == 1,  "alpha-w2 페인 수 = 1");

    /* 이름으로 세션 검색 */
    ASSERT(session_find_by_name(&mgr, "alpha") == sa, "alpha 검색");
    ASSERT(session_find_by_name(&mgr, "beta")  == sb, "beta 검색");

    session_manager_destroy(&mgr);
    ASSERT(1, "전체 해제 크래시 없음");
}

static void test_null_safety(void) {
    TEST("NULL 안전성");
    session_manager_t mgr;
    session_manager_init(&mgr);

    ASSERT(session_create(&mgr, NULL)  == NULL, "session_create NULL name → NULL");
    ASSERT(session_create(NULL, "x")   == NULL, "session_create NULL mgr → NULL");
    ASSERT(session_find_by_name(NULL, "x") == NULL, "find NULL mgr → NULL");
    ASSERT(window_create(NULL, "w")    == NULL, "window_create NULL session → NULL");
    ASSERT(pane_create(NULL, 80, 24)   == NULL, "pane_create NULL window → NULL");

    /* NULL 에 destroy 해도 크래시 없어야 함 */
    session_destroy(&mgr, NULL);
    window_destroy(NULL, NULL);
    pane_destroy(NULL, NULL);
    pane_resize(NULL, 80, 24);
    ASSERT(1, "NULL destroy/resize 크래시 없음");

    session_manager_destroy(&mgr);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Session 관리 테스트 ===\n");

    test_manager_init();
    test_manager_destroy_empty();

    test_session_create();
    test_session_create_multiple();
    test_session_find_by_name();
    test_session_find_by_id();
    test_session_destroy();

    test_window_create();
    test_window_create_multiple();
    test_window_find_by_id();
    test_window_destroy();
    test_window_destroy_last();

    test_pane_create();
    test_pane_create_multiple();
    test_pane_find_by_id();
    test_pane_destroy();
    test_pane_destroy_last();
    test_pane_resize();

    test_cascade_destroy();
    test_full_tree();
    test_null_safety();

    printf("\n결과: %d 통과 / %d 실패\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
