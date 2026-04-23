# 알려진 이슈

## Open

### [BUG] 한글 입력/출력 전반 미지원 (2026-04-22, 갱신 2026-04-23)

**증상 (3가지 영역 모두 재현됨)**:
1. **터미널 셀 출력**: 셸에서 `echo 한글` 해도 □/공백으로 표시. 커서 폭·셀 경계는 유지되나 글리프가 그려지지 않음.
2. **터미널 입력**: IME(fcitx/ibus) 로 한글 입력 시 PTY 에 전혀 도달하지 않거나 깨진 바이트만 전달됨.
3. **Nuklear 오버레이(설정/확인 팝업/우클릭 메뉴)**: 한글 라벨·본문이 모두 □.

**원인 추정 (영역별)**:

1. **터미널 출력 (`gl_renderer`)** — `resolve_font_path("monospace")` 가 리눅스에서 보통 `DejaVuSansMono.ttf` 로 귀결되는데 여기에 Hangul Syllables(U+AC00–U+D7A3) 글리프 자체가 없음. 폰트에 없으므로 `font_face_load` 가 빈 글리프(.notdef) 를 반환 → 빈 셀로 출력. Fallback 체인(한글 폰트로 2차 lookup) 부재.

2. **터미널 입력 (`char_callback` 경로)** — `main.c:1238 char_callback()` 은 GLFW 가 올려주는 Unicode codepoint 를 그대로 UTF-8 인코딩해 PTY 에 넣는 구조. 리눅스(X11/Wayland/WSL) 에서는 IME(fcitx/ibus) 가 GLFW 와 직접 연동되지 않아 조합된 한글 codepoint 가 `char_callback` 까지 도달하지 않는 경우가 많음. GLFW 의 preedit/IME API(`glfwSetPreeditCallback` 등, GLFW 3.4+ 또는 확장) 미사용.

3. **Nuklear 오버레이** — `nk_impl_init()` 에서 `nk_font_atlas_add_from_file()` 의 기본 glyph range(0x20–0xFF) 만 baked 되어 CJK 코드포인트가 아틀라스에 없음. UI 폰트도 `resolve_font_path("monospace")` 첫 매칭이라 한글 미지원 폰트가 집힐 수 있음.

**해결 방향(미착수)**:

- **공통**: 설정에 `ui.font_family` / `terminal.font_family` 추가, 한글 지원 폰트(Noto Sans CJK KR, Nanum Gothic, Malgun Gothic) 폴백 체인 자동 탐색. TTC 파일은 내부 stbtt 가 첫 face 만 읽으므로 후보에서 배제하거나 face index 지정.
- **터미널 출력**: `font_face` 에 fallback chain 개념 추가 — 주 폰트에 글리프 없으면 한글 폰트로 shape. harfbuzz 가 없으면 codepoint-per-glyph 수준 fallback 만이라도 구현.
- **터미널 입력**: GLFW 최신(IME preedit 지원) 로 업그레이드 혹은 플랫폼별 IME 브릿지(XIM/IBus D-Bus/WSL WIN32 input) 도입. 최소한 preedit 상태 표시라도 필요.
- **Nuklear 오버레이**: `nk_font_config.range` 에 한글 glyph range 명시 (`0xAC00–0xD7A3` 등) + atlas 크기 확대. UI 폰트 경로도 별도 resolve.

**영향 파일**:
- 출력: `src/client/font_face.{c,h}`, `src/client/renderer/gl_renderer.c`, `src/client/main.c` (`resolve_font_path`).
- 입력: `src/client/main.c` (`char_callback`, `key_callback`), GLFW 버전 / 플랫폼별 IME 브릿지.
- 오버레이: `src/client/ui/nk_impl.c` (폰트 베이킹), `src/client/main.c` (`nk_impl_init` 호출부).

**참고 브랜치**: `fix/nk-overlay-cjk-font` 에서 Nuklear 오버레이 쪽(영역 3) 만 부분 해결 시도했으나 실제 환경에서 여전히 깨지는 것으로 보고됨(2026-04-23) — 추가 조사 필요. 미머지 상태.

**우선순위**: 상 — 한국어 사용자가 터미널 본연의 기능(한글 입력/출력) 을 쓸 수 없음. Nuklear 오버레이보다 터미널 입출력이 먼저.

---

## Resolved

### [FIXED] 우클릭 컨텍스트 메뉴 · 설정창 반응형 레이아웃 (2026-04-22)

**현상**:
- 우클릭 컨텍스트 메뉴 고정 `180x250` — 폰트 크기/라벨 변화 시 잘림, 화면 경계 clamp 도 마우스 콜백 하드코딩.
- 설정창 고정 `480x520` — 작은 화면에서 잘리고, 탭 내용이 많을 때 일부 필드가 가려짐.

**수정**:
- `ui/ui_overlay.{c,h}` 공통 헬퍼 추가 — `ui_overlay_centered_rect()` (비율 기반 중앙 정렬 + min/max clamp), `ui_overlay_popup_at()` (앵커 기준 팝업 + 화면 경계 flip/clamp).
- `main.c` — 컨텍스트 메뉴 높이를 버튼 수 × 행 높이 + 스타일 spacing 으로 동적 계산, 위치는 `ui_overlay_popup_at` 으로 일괄 처리. 마우스 콜백의 하드코딩 clamp 제거 (렌더 시점에서 처리하므로 창 리사이즈/폰트 변경에도 자동 재위치).
- `settings_ui.c` — 초기 rect 를 화면 비율(0.55×0.75, 520~820 / 520~880 clamp) 로 계산. 탭 내용을 `nk_group_begin(NK_WINDOW_BORDER)` 로 감싸 세로 스크롤 허용. 창 content region 에서 탭 헤더/버튼 영역을 뺀 나머지를 group 높이로 잡아, 사용자가 SCALABLE 로 크기 조정해도 리플로우됨. API 에 `win_w, win_h` 파라미터 추가.

**영향 파일**: `src/client/main.c`, `src/client/ui/settings_ui.{c,h}`, `src/client/ui/ui_overlay.{c,h}` (신규), `src/client/CMakeLists.txt`.

---

### [FIXED] pane split 단축키 동시 입력 시 새 pane 과 기존 pane 이 동기화됨 (2026-04-22)

**증상**: `Alt + -` 와 `Alt + =` 를 동시에(또는 잠깐이라도 누르고 있으면) 새로 생성된 pane 과 기존 pane 이 "동기화" 되어 동일한 입력/출력을 공유하는 것처럼 동작.

**원인**: `key_callback` 이 `GLFW_RELEASE` 만 필터링하고 `GLFW_PRESS` / `GLFW_REPEAT` 를 구분 없이 단축키 디스패치에 흘려보냄. 사용자가 키를 잠깐만 눌러도 OS 의 키리피트(~30Hz)가 `do_split` 을 rapid-fire 로 호출해 pane 이 폭발적으로 쪼개지고, 수많은 초소형 pane 이 동일한 쉘 프롬프트를 나란히 표시하면서 "동기화된 것처럼" 보였다. 보고서 가설 중 "input.c 의 동시 입력 coalesce 부재" 가 실제 원인.

**수정**: edge-triggered 이어야 하는 액션(`split_*`, `close_pane`, `focus_*`, `copy`, `paste`, `preferences`) 을 `GLFW_PRESS` 에만 디스패치하고 `GLFW_REPEAT` 는 consume 만 하고 drop. 반복이 자연스러운 `resize_*` / `scroll_*` 은 기존대로 유지. tmux/zellij/i3wm 의 컨벤션과 동일.

**영향 파일**: `src/client/main.c` (`key_callback`).

---

### [FIXED] pane 닫을 때 간헐적 crash 또는 터미널 freeze (2026-04-21)

**증상**: pane 을 닫을 때(Ctrl+W 또는 셸 `exit`) 간헐적으로 segfault 또는 입력 정지.

**원인**: 콜백(`on_pane_exited`) 안에서 sync IPC 호출이 `recv_until` 로 OK 응답을 기다리는 동안, 수신 버퍼에 쌓여있던 다른 메시지(PANE_EXITED, PTY_OUTPUT)가 dispatch 되며 콜백이 재진입. 깊은 재귀 + 전역 상태 스냅샷 불일치 + 드물게 use-after-free 로 이어짐.

**수정**:
- `ipc_client_window_layout` / `ipc_client_pane_resize` 를 비동기로 전환 (OK 대기 제거). 두 API 모두 UI 가 응답 값을 사용하지 않으므로 안전. 응답은 다음 poll 에서 자연스럽게 소비됨.
- `dispatch_one` 에 `ipc_msg_header.magic_ver` 검증 추가. 어긋나면 버퍼 비우고 연결 종료 경로로 유도 → garbage payload 가 콜백에 전달되지 않음.
- `on_pane_exited` 에서 죽는 pane_id 가 `g_sel_pane` 과 같으면 선택 state 전체 초기화 (stale reference 제거).

**영향 파일**: `src/client/ipc_client.c`, `src/client/main.c`.

---

### [FIXED] pane 닫기 후 영역이 사용 불가 (2026-04-21)

**증상**: window 를 분할한 뒤 pane 하나를 닫으면, 닫힌 pane 이 있던 영역이 빈 공간으로 남아 입력/출력이 들어가지 않음.

**원인**: `do_close_pane()` / `on_pane_exited()` 에서 `layout_remove()` 로 트리 rect 만 넓히고, 남은 pane 의 PTY · `screen_t` 크기 재조정을 생략함. 트리상 형제 노드는 부모 rect 를 상속받지만 실제 터미널 크기는 옛값 그대로라 확장된 영역이 dead-space 로 남음.

**수정**: `layout_remove()` 직후 `layout_each_leaf(..., resize_leaf_cb)` 를 호출해 모든 남은 leaf 의 PTY+screen 을 rect 에 맞춰 재조정.

**영향 파일**: `src/client/main.c` (`do_close_pane`, `on_pane_exited`).

### [FIXED] 셸 `exit` 시 pane 이 닫히지 않음 (2026-04-15)

**원인**: `ipc_server_run()` 의 epoll 디스패치가 `EPOLLHUP|EPOLLERR` 을 `else if` 로 먼저 검사해서, HUP 가 올 때 PTY fd 를 그냥 `pty_fd=-1` 로 마킹만 하고 `pty_output_read()` 를 호출하지 않음. 결과적으로:
- 셸의 마지막 출력이 드레인되지 않음
- `PANE_EXITED` 브로드캐스트 누락
- pane_t 가 session 트리에 그대로 남음 (클라이언트는 pane 유지, 입력만 멎음)

**수정**: PTY fd 에 대한 이벤트를 전부 `pty_output_read()` 로 라우팅. 이 함수가 drain + `EIO` 감지 → `pty_eof=1` → PANE_EXITED 브로드캐스트 + `pane_destroy` + `session_destroy_if_empty` 까지 일괄 처리함. 클라이언트 fd 는 별도로 EPOLLIN 과 EPOLLHUP 을 둘 다 처리하도록 분리.

**영향 파일**: `src/daemon/ipc_server.c` (`ipc_server_run()` 의 이벤트 디스패치 로직).
