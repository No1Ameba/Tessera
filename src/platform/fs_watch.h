#ifndef TERMEMU_FS_WATCH_H
#define TERMEMU_FS_WATCH_H

/*
 * 파일 변경 감시 — 설정 핫 리로드용
 *
 * POSIX: inotify
 * Windows: ReadDirectoryChangesW (미구현)
 */

/*
 * 감시 컨텍스트를 생성한다.
 * @return 불투명 핸들, 실패 시 NULL.
 */
void *fs_watch_create(void);

/*
 * 파일 경로를 감시 대상에 추가한다.
 * @return 0 성공, -1 실패.
 */
int fs_watch_add(void *handle, const char *path);

/*
 * 변경이 있는지 논블로킹으로 확인한다.
 * @return 1 변경 감지, 0 변경 없음, -1 에러.
 */
int fs_watch_poll(void *handle);

/*
 * 감시를 종료하고 자원을 해제한다.
 */
void fs_watch_destroy(void *handle);

#endif /* TERMEMU_FS_WATCH_H */
