#ifndef TESSERA_PATHS_H
#define TESSERA_PATHS_H

/*
 * OS별 표준 경로 조립 (architecture-spec.md §6).
 *
 *   POSIX   — $HOME/.config/tessera
 *   Windows — %APPDATA%\tessera
 *
 * 경로 규칙이 데몬/클라이언트 여러 곳에 흩어져 있으면 두 프로세스가 서로 다른
 * 디렉토리를 보게 되므로 여기 한 곳에서만 만든다.
 */

#include <stddef.h>

/*
 * 설정 디렉토리 경로를 buf 에 쓴다 (후행 구분자 없음).
 * @return 0 성공, -1 실패 (환경변수 없음 / buf 부족).
 */
int tessera_config_dir(char *buf, size_t buflen);

/*
 * 설정 디렉토리 기준 상대 경로를 조립한다.
 * 예: tessera_config_path("sessions/work.json", ...)
 * @return 0 성공, -1 실패.
 */
int tessera_config_path(const char *rel, char *buf, size_t buflen);

/*
 * 중간 디렉토리를 포함해 디렉토리를 만든다 (mkdir -p).
 * 이미 있으면 성공으로 본다.
 * @return 0 성공, -1 실패.
 */
int tessera_mkdir_p(const char *path);

/*
 * 프로세스별로 겹치지 않는 임시 파일 경로를 만든다.
 * POSIX 는 /tmp, Windows 는 %TEMP% 를 쓴다.
 * @return 0 성공, -1 실패.
 */
int tessera_temp_path(const char *prefix, char *buf, size_t buflen);

#endif /* TESSERA_PATHS_H */
