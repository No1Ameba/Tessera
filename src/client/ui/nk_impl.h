#ifndef TERMEMU_NK_IMPL_H
#define TERMEMU_NK_IMPL_H

/*
 * Nuklear GLFW + OpenGL 3.3 Core 백엔드
 *
 * 터미널 위에 설정 오버레이를 렌더링한다.
 * GLFW 콜백과 통합하여 입력을 Nuklear에 전달한다.
 */

struct GLFWwindow;
struct nk_context;

/*
 * Nuklear 컨텍스트를 초기화하고 GL 자원을 할당한다.
 * font_path: TTF 파일 경로 (UI 폰트, 터미널 폰트와 별도)
 * font_size: UI 폰트 크기 (px)
 * @return nk_context 포인터, 실패 시 NULL
 */
struct nk_context *nk_impl_init(struct GLFWwindow *win,
                                 const char *font_path, float font_size);

/*
 * 새 프레임 시작. glfwPollEvents() 이후, UI 코드 이전에 호출.
 */
void nk_impl_new_frame(void);

/*
 * Nuklear 커맨드 버퍼를 GL로 렌더링한다.
 * 터미널 렌더링 이후, glfwSwapBuffers 이전에 호출.
 */
void nk_impl_render(void);

/*
 * GLFW 이벤트를 Nuklear에 전달한다.
 * main.c의 GLFW 콜백에서 호출.
 * @return 1 if Nuklear consumed the event, 0 otherwise
 */
int nk_impl_handle_key(int key, int scancode, int action, int mods);
int nk_impl_handle_char(unsigned int codepoint);
int nk_impl_handle_mouse_button(int button, int action, int mods);
int nk_impl_handle_scroll(double xoff, double yoff);
void nk_impl_handle_cursor_pos(double x, double y);

/*
 * 이름으로 Nuklear 윈도우를 보이기/숨기기.
 */
void nk_impl_show_window(const char *name, int show);

/*
 * 마우스 입력 상태를 리셋한다.
 * 오버레이 열기 시 이전 클릭 상태가 남아 드래그되는 버그 방지.
 */
void nk_impl_reset_input(void);

/*
 * 자원 해제.
 */
void nk_impl_shutdown(void);

#endif /* TERMEMU_NK_IMPL_H */
