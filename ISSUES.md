# 알려진 이슈

## Open

### [UX] 우클릭 컨텍스트 메뉴 · 설정창 반응형 레이아웃 (2026-04-22)

**현상**:
- 우클릭 컨텍스트 메뉴(`main.c` 의 `"ctx_menu"` Nuklear 창) 가 고정 크기 `180x250` 로 생성됨. 폰트 크기·언어 에 따라 버튼 라벨이 잘리거나 메뉴가 화면 밖으로 나갈 수 있음.
- 설정창(`settings_ui_draw`) 도 `nk_rect(50, 50, 480, 520)` 고정. 작은 화면에서는 잘리고, 큰 화면에서는 상대적으로 작아 읽기 어려움. 내용에 따라 세로 스크롤이 필요할 때도 고정 높이라 일부 필드가 안 보임.
- 창 리사이즈(`framebuffer_size_callback`) 시에도 두 UI 는 초기 rect 를 유지하기만 함.

**기대 동작**: 확인 팝업(`ui/confirm_dialog.c`) 에 적용한 것처럼
1. 기준 크기를 화면 비율로 계산 (min/max clamp).
2. 내용 area 는 `nk_group` + 스크롤로 감싸 오버플로우 시 스크롤.
3. 컨텍스트 메뉴는 클릭 위치가 화면 밖으로 벗어나지 않도록 `(x, y)` 를 재위치시키는 로직 추가.
4. 설정창은 탭 내용물 전체를 스크롤 group 으로 감싸 세로 리플로우 보장.

**영향 파일**:
- `src/client/main.c` (컨텍스트 메뉴 rect 계산)
- `src/client/ui/settings_ui.c` (`nk_begin` rect / 각 탭의 group 구성)
- 필요 시 공통 헬퍼를 `ui/` 에 추출 (`ui_overlay_rect(...)` 같은 것)

**우선순위**: 중 — 기능은 동작하지만 UX 품질 이슈. 확인 팝업 작업에서 같은 패턴을 검증한 상태이므로 재사용 가능.

---

## Resolved

### [FIXED] Nuklear 오버레이(설정/확인 팝업)에서 한글 깨짐 (2026-04-22)

**증상**: `Ctrl+,` 설정창, 닫기 확인 팝업, 우클릭 컨텍스트 메뉴 등 Nuklear UI 레이어의 한글 문자열이 □/공백 으로 표시.

**원인**:
1. `nk_impl_init` 이 `nk_font_atlas_add_from_file(..., NULL)` 로 폰트를 추가 → 기본 Latin glyph range 만 베이킹, 한글 코드포인트는 atlas 부재로 .notdef 폴백.
2. UI 폰트로 받은 `monospace` 가 `fc-match` 에서 한글 미지원 라틴 전용 폰트(DejaVu 등) 로 해석.

**수정**:
- `nk_impl.c` — `nk_font_config.range` 에 한글/CJK glyph range 지정 (`Basic Latin, Hangul Syllables, Hangul Jamo, CJK Punctuation` 등). 실패 시 경고 로그 후 기본 폰트로 폴백.
- `main.c` — `resolve_ui_font_path()` 추가. TTF 후보(NanumGothic → NanumBarunGothic → SourceHanSans → Malgun Gothic) 를 우선 탐색하고, `fc-match :lang=ko` 폴백도 TTC 는 배제 (Nuklear 내부의 `stbtt_InitFont` 가 TTC 첫 face 만 로드하므로 JP face 를 집어 한글이 또 깨질 위험). 해석 결과를 `nk_impl_init` 에 별도로 전달.

**영향 파일**: `src/client/ui/nk_impl.c`, `src/client/main.c`.

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
