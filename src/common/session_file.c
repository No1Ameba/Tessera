#include "session_file.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int session_snapshot_save(const char *path, const session_snapshot_t *snap)
{
    if (!path || !snap) return -1;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "name", snap->name);
    cJSON_AddNumberToObject(root, "active_window", snap->active_window);

    cJSON *windows = cJSON_AddArrayToObject(root, "windows");
    for (int wi = 0; wi < snap->window_count; wi++) {
        const snap_window_t *w = &snap->windows[wi];
        cJSON *jw = cJSON_CreateObject();
        cJSON_AddStringToObject(jw, "name", w->name);
        cJSON_AddNumberToObject(jw, "active_pane", w->active_pane);

        cJSON *panes = cJSON_AddArrayToObject(jw, "panes");
        for (int pi = 0; pi < w->pane_count; pi++) {
            const snap_pane_t *p = &w->panes[pi];
            cJSON *jp = cJSON_CreateObject();
            cJSON_AddNumberToObject(jp, "cols", p->cols);
            cJSON_AddNumberToObject(jp, "rows", p->rows);
            if (p->cwd[0])
                cJSON_AddStringToObject(jp, "cwd", p->cwd);
            cJSON_AddItemToArray(panes, jp);
        }
        cJSON_AddItemToArray(windows, jw);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return -1;

    FILE *f = fopen(path, "w");
    if (!f) { free(text); return -1; }
    fputs(text, f);
    fclose(f);
    free(text);
    return 0;
}

int session_snapshot_load(const char *path, session_snapshot_t *snap)
{
    if (!path || !snap) return -1;
    memset(snap, 0, sizeof *snap);

    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return -1; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    cJSON *item;
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item))
        strncpy(snap->name, item->valuestring, SNAP_NAME_MAX - 1);

    item = cJSON_GetObjectItemCaseSensitive(root, "active_window");
    if (cJSON_IsNumber(item))
        snap->active_window = item->valueint;

    cJSON *windows = cJSON_GetObjectItemCaseSensitive(root, "windows");
    if (cJSON_IsArray(windows)) {
        int wi = 0;
        cJSON *jw;
        cJSON_ArrayForEach(jw, windows) {
            if (wi >= SNAP_MAX_WINDOWS) break;
            snap_window_t *w = &snap->windows[wi];

            item = cJSON_GetObjectItemCaseSensitive(jw, "name");
            if (cJSON_IsString(item))
                strncpy(w->name, item->valuestring, SNAP_NAME_MAX - 1);

            item = cJSON_GetObjectItemCaseSensitive(jw, "active_pane");
            if (cJSON_IsNumber(item))
                w->active_pane = item->valueint;

            cJSON *panes = cJSON_GetObjectItemCaseSensitive(jw, "panes");
            if (cJSON_IsArray(panes)) {
                int pi = 0;
                cJSON *jp;
                cJSON_ArrayForEach(jp, panes) {
                    if (pi >= SNAP_MAX_PANES) break;
                    snap_pane_t *p = &w->panes[pi];

                    item = cJSON_GetObjectItemCaseSensitive(jp, "cols");
                    if (cJSON_IsNumber(item))
                        p->cols = (uint16_t)item->valueint;
                    item = cJSON_GetObjectItemCaseSensitive(jp, "rows");
                    if (cJSON_IsNumber(item))
                        p->rows = (uint16_t)item->valueint;
                    item = cJSON_GetObjectItemCaseSensitive(jp, "cwd");
                    if (cJSON_IsString(item))
                        strncpy(p->cwd, item->valuestring, SNAP_CWD_MAX - 1);

                    pi++;
                }
                w->pane_count = pi;
            }
            wi++;
        }
        snap->window_count = wi;
    }

    cJSON_Delete(root);
    return 0;
}
