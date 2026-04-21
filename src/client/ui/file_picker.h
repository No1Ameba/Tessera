#ifndef TERMEMU_FILE_PICKER_H
#define TERMEMU_FILE_PICKER_H

/*
 * 네이티브 파일 선택 다이얼로그 — zenity / kdialog 외부 프로세스를 popen 으로
 * 실행해 사용자에게 파일 경로를 받는다. 시스템에 해당 프로그램이 없거나
 * 사용자가 취소한 경우 실패로 반환된다.
 *
 * 반환:
 *   1  성공 (out_path 채워짐)
 *   0  사용자 취소
 *  -1  picker 프로그램 부재 또는 실행 실패
 */

#include <stddef.h>

/* 기존 파일 열기 다이얼로그 */
int file_picker_open(char *out_path, size_t out_size,
                      const char *title,
                      const char *filter /* nullable, 예: "*.json" */);

/* 저장 다이얼로그. default_name 은 기본 파일명 제안 (선택, nullable). */
int file_picker_save(char *out_path, size_t out_size,
                      const char *title,
                      const char *default_name,
                      const char *filter);

#endif /* TERMEMU_FILE_PICKER_H */
