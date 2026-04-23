# 알려진 이슈

## Open

### [BUG] Nuklear 오버레이(설정/확인 팝업)에서 한글 깨짐 (2026-04-22)

**증상**: `Ctrl+,` 설정창, 닫기 확인 팝업, 우클릭 컨텍스트 메뉴 등 Nuklear 로 그리는 UI 레이어의 한글 문자열이 깨져서 (□ / 공백) 표시됨. 터미널 셀 렌더링(`gl_renderer`) 쪽 한글은 정상.

**원인 추정**: `nk_impl_init()` 에서 `nk_font_atlas_add_from_file()` 로 TTF 를 올릴 때 기본 ASCII glyph range(`nk_font_default_glyph_ranges()`, 0x20–0xFF) 만 baked 됨. CJK/한글 코드포인트는 아틀라스에 없어 렌더 시 .notdef 로 폴백. 또한 현재 UI 폰트는 `resolve_font_path("monospace")` 로 받은 첫 매칭 — 한글 지원이 없는 라틴 전용 폰트일 가능성이 큼.

**해결 방향(미착수)**:
- `nk_font_config` 에 한글 glyph range 추가 — `{0x0020, 0x00FF, 0xAC00, 0xD7A3, 0x3000, 0x303F, 0x3130, 0x318F, 0}` 정도. 아틀라스 크기도 늘려야 함 (현재 atlas 는 Nuklear 기본값).
- UI 폰트 폴백 체인: 한글 지원 폰트(Noto Sans CJK KR, Nanum Gothic 등) 가 있으면 그걸로 베이킹, 없으면 경고 + 라틴만.
- 설정에 `ui.font_family` 추가해 사용자가 한글 폰트 지정 가능하게.

**영향 파일**: `src/client/ui/nk_impl.c` (init · 폰트 베이킹), `src/client/main.c` (`nk_impl_init` 호출부 폰트 경로).

**우선순위**: 중 — 기능 자체는 동작하지만 UX 품질에 큰 영향. 한국어 시스템에서 설정창/확인 팝업/메뉴 문구가 본문 텍스트에서도 영향 받음.

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
