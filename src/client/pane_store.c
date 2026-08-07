#include "pane_store.h"

static pane_slot_t g_panes[MAX_PANES];

pane_slot_t *pane_slot_find(uint32_t pane_id)
{
    for (int i = 0; i < MAX_PANES; i++)
        if (g_panes[i].used && g_panes[i].pane_id == pane_id)
            return &g_panes[i];
    return NULL;
}

pane_slot_t *pane_slot_alloc(uint32_t pane_id, int cols, int rows, int scrollback_lines)
{
    /* 같은 pane_id 가 이미 있으면 재사용 — 중복 할당 방지 */
    pane_slot_t *existing = pane_slot_find(pane_id);
    if (existing) return existing;

    int sb = scrollback_lines > 0 ? scrollback_lines : 1000;
    for (int i = 0; i < MAX_PANES; i++) {
        if (!g_panes[i].used) {
            g_panes[i].pane_id        = pane_id;
            g_panes[i].parent_pane_id = 0;
            g_panes[i].used           = 1;
            screen_init(&g_panes[i].screen, cols, rows, sb);
            return &g_panes[i];
        }
    }
    return NULL;
}

void pane_slot_free(uint32_t pane_id)
{
    pane_slot_t *s = pane_slot_find(pane_id);
    if (!s) return;
    screen_destroy(&s->screen);
    s->used = 0;
}

pane_slot_t *pane_slot_at(int index)
{
    if (index < 0 || index >= MAX_PANES) return NULL;
    return &g_panes[index];
}
