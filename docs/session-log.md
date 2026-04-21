# 작업 로그

세션별 주요 변경 사항, 결정, 이슈 해결 내역을 시간순으로 기록한다.
요약은 커밋 메시지가 담고, 이 문서는 **왜 그렇게 결정했는지**와 **무엇이 남았는지**를 남긴다.

---

## 2026-04-21

커밋 범위: `b29ee6a` ~ `aa5251b` (5 commits)

### 추가된 기능

#### 1. 다중 클라이언트 실시간 동기화
- **원격 실시간 반영 버그 수정**: 원격 attach 후 초기 ringbuffer 내용만 보이고 이후 업데이트가 반영되지 않던 문제. 원인은 `ipc_client_poll()` 이 local fd 만 체크(`c->fd < 0`)하던 가드였음. remote 모드는 pipe 를 쓰므로 `read_fd(c)` 기반으로 변경.
- **split 브로드캐스트** (`IPC_MSG_PANE_SPLIT_NOTIFY = 0x36`): 같은 세션을 보는 다른 클라이언트에 split 을 실시간 반영. 메시지에 `parent_pane_id / direction / ratio` 를 실어 수신 측이 동일한 split 을 수행.
- **pane close 브로드캐스트**: `handle_pane_destroy` 에서 요청자 외 모든 클라이언트에 `PANE_EXITED` 전파 (shell exit 경로와 동일 메시지 형식).
- **layout blob 업로드** (`IPC_MSG_WINDOW_LAYOUT = 0x24`): 클라이언트가 직렬화된 layout 트리를 데몬에 보관. 재접속 시 트리 구조 복원용.
- **layout 직렬화 포맷**: preorder 바이너리. `LEAF: u8(0) u32(pane_id)`, `SPLIT: u8(1=H,2=V) f32(ratio) <left> <right>`.

#### 2. 세션 수명 정책
- `session_t.attach_count / last_detach_ms` 추가. 모든 클라이언트 detach 후 `session_idle_timeout` (기본 5분) 경과하면 세션 destroy.
- pane 이 0개로 닫히는 세션은 `session_destroy_if_empty` 로 즉시 파기 (유예 없음).
- `ipc_server_configure()` 로 `autosave_interval`, `session_idle_timeout` 런타임 주입.
- `config.json` 에 `daemon` 섹션 추가.

#### 3. 크래시 복구 (세션 스냅샷)
- `~/.config/termemu/sessions/<name>.json` 에 cJSON 기반 스냅샷 저장.
- `save_all_sessions()` 를 autosave / shutdown 시 호출.
- 데몬 시작 시 `ipc_server_restore_sessions()` 가 스냅샷 디렉토리 스캔 → 세션/window/pane 복원 + PTY 재spawn + cwd 복원 (`cd <path>\nclear\n` 주입).
- 영구 destroy 시 snapshot 파일 삭제.

#### 4. OSC 8 하이퍼링크
- `term_cell_t.link_id` (uint16_t), `screen_t.links[64]` 링버퍼.
- OSC 8 파서: `ESC ] 8 ; params ; URI ST`, 중복 dedup, 링 replacement.
- 렌더링: 링크 셀에 `CELL_ATTR_UNDERLINE` 자동 부여, underline pass 에서 2px 두께 밑줄.
- Ctrl+Click 에서 `xdg-open` / `open` 으로 URL 실행 (fork + setsid + /dev/null redirect).

#### 5. 설정창 확장 + 핫 리로드
- Window 탭: Cursor Style 콤보 (Block / Hollow Block / Bar / Underline), Cursor Blink 체크박스.
- Keys 탭: `copy / paste / resize_left/right/up/down` 행 추가.
- `do_config_reload()` 확장:
  - font family/size 즉시 적용 (`gl_renderer_set_font()` 도입, atlas 재생성).
  - padding 즉시 적용 (`compute_layout_rect()` 헬퍼).
  - scrollback_lines 적용 (전역 `g_scrollback_lines`).

#### 6. 커서 렌더링 개선
- 기본 커서가 보이지 않던 문제 — 실제 원인은 BG 셰이더의 `i_cell * cell_size + v_pos * cell_size` 에서 cursor/underline 이 픽셀 좌표를 `i_cell` 에 직접 넣어 `px * cell_w` 가 되어 화면 밖으로 렌더되던 버그. `u_quad_size` 유니폼 도입으로 그리드 스텝과 쿼드 크기 분리해 해결.
- **Reverse video**: block 커서가 글자를 가리는 문제 해결. 블록을 fg 색으로 그린 뒤 해당 셀 글리프를 bg 색으로 덧그림.
- **Hollow block** 스타일: 4개 모서리 edge 쿼드로 속이 빈 블록. (7=blink, 8=solid)
- 스타일 코드 확장: 1/3/5/7 = blink, 2/4/6/8 = solid. DECSCUSR 미설정(0)일 때 config 값을 사용.

#### 7. 신규 단축키 (설정창에서 편집 가능)
| 키 | 동작 |
|-----|-----|
| `Ctrl+Shift+C` / `Ctrl+Insert` | 선택 복사 |
| `Ctrl+Shift+V` / `Shift+Insert` | 붙여넣기 (bracketed paste 감지) |
| `Alt+Shift+H/J/K/L` | pane 경계 이동 (방향 기반, focus 위치 무관) |

- `Ctrl+Insert` / `Shift+Insert` 는 상시 alias로 하드코딩.
- resize 로직: 활성 leaf 에서 부모 체인 타고 올라가 **원하는 방향의 split 조상**을 찾아 `split_ratio` ±0.03 조정 → 전체 leaf resize → 데몬에 blob 재업로드.

#### 8. 우클릭 컨텍스트 메뉴 자동 flip
- 메뉴 크기(180x250)가 창 가장자리에 걸릴 경우 반대 방향으로 flip + 0 이하 clamp.

### 해결한 버그

1. **셸 `exit` 시 pane 이 닫히지 않음**: `ipc_server_run()` epoll dispatch 가 `EPOLLHUP` 을 `else if` 로 먼저 처리해 PTY fd 를 drain 하지 않고 `pty_fd=-1` 로 마킹만 하던 문제. PTY fd 의 모든 이벤트를 `pty_output_read()` 로 라우팅하여 EIO drain + `PANE_EXITED` 브로드캐스트 + `pane_destroy` + `session_destroy_if_empty` 일괄 처리.
2. **pane 닫은 뒤 영역이 사용 불가**: `layout_remove()` 가 형제 노드 rect 만 확장하고 PTY/screen 크기를 재조정 안 함. `do_close_pane` / `on_pane_exited` 에서 `layout_each_leaf(resize_leaf_cb)` 추가.
3. **중복 pane_slot 상태에서 pane close 시 크래시**: `pane_slot_alloc` 이 중복 체크 없이 슬롯 할당. `pane_slot_find` 우선 시도 → 기존 슬롯 반환으로 dedupe. 동시에 `do_close_pane` / `on_pane_exited` 에서 같은 pane_id leaf 모두 제거하는 루프.
4. **exit 시 PANE_EXITED 재귀로 스택 오버플로**: `on_pane_exited` → `push_layout_to_daemon` → `ipc_client_window_layout` 이 **동기 응답 대기** 중 dispatch 루프에서 또 PANE_EXITED 처리 → 또 push → 재귀 폭발. `changed` 플래그로 실제 상태가 바뀐 경우에만 push 하도록 가드.
5. **커서 invisible**: 위의 쉐이더 좌표 버그와 동일 원인.

### 주요 결정

- **세션 수명**: 클라이언트 detach 후에도 pane 이 남아있으면 5분 유예, 비어있으면 즉시 파기. 근거: 우연한 client crash / 잠시 detach 후 복귀 시나리오 보호 + 의미 없는 빈 세션은 즉시 정리.
- **커서 reverse video vs 투명 블록**: 초기에 alpha=0.5 투명 블록 → 안 보임 호소 → alpha=1.0 → 글자 가려짐 호소 → reverse video 최종. 사용자 선택이지만 xterm 표준 방식.
- **resize 방향 의미론**: 초기엔 focus 기준 grow/shrink. 사용자가 "방향 기반 이동" 선호 → H/L/J/K = 경계선 좌/우/하/상 이동 (xterm/tmux 관례 아님, 하지만 사용자 직관 우선).
- **단축키 Ctrl+Ins/Shift+Ins 처리**: config 단일 binding 제약 때문에 primary 는 편집 가능(`Ctrl+Shift+C/V`), legacy alias 는 하드코딩 상시 지원.

### WSLg 특정 이슈 (미해결, 의도적 보류)

- 증상: 세션 닫은 후 새 termemu 실행 시 간헐적으로 창이 뜨지 않음. 작업표시줄에도 표시 안 됨. Ctrl+C 로 인터럽트 하고 재시도 2~3 번 필요.
- 진단 로그로 확인: `entering event loop` 까지 정상 도달. 프로세스는 살아있고 이벤트 루프도 돌지만 WM 에 창이 매핑되지 않음.
- 결론: **WSLg/Weston 컴포지터가 wl_surface commit 을 간헐적으로 놓치는 버그**로 추정. 네이티브 X11/Wayland 에선 재현 가능성 낮음.
- 시도했다 되돌린 대응:
  - daemon 재시도 로직 (`ipc_client_connect` 에 HELLO_ACK 타임아웃 시 socket unlink + respawn) — 증상 지속으로 되돌림.
  - `glfwShowWindow` + 3회 `swapBuffers` + `glfwFocusWindow` — 효과 미확인, 진단 프린트와 함께 되돌림.

### 문서

- `ISSUES.md` 신설: Resolved 항목 기록 시작.
- `PROGRESS.md` "최근 개선 사항" 섹션 추가.

### 커밋 분할 (5 개)

| 커밋 | 요약 |
|------|------|
| `b29ee6a` | feat(ipc): PANE_SPLIT_NOTIFY/WINDOW_LAYOUT 프로토콜 + 레이아웃 serialize |
| `569cb9b` | feat(daemon): 다중 클라이언트 브로드캐스트 + 세션 수명 + 크래시 복구 + EPOLLHUP |
| `4aae949` | feat(renderer): OSC 8 + 커서 스타일 확장 + 쉐이더 좌표 수정 |
| `e303857` | feat(client): 단축키 + 설정창 확장 + OSC 8 클릭 + 핫 리로드 + pane 안정화 |
| `aa5251b` | docs: ISSUES.md / PROGRESS.md 업데이트 |

### 남은 과제 (우선순위 순)

- [ ] **Windows IPC**: ConPTY + Named Pipe
- [ ] **Kitty Image Protocol**: 이미지 인라인 표시
- [ ] **CI 파이프라인**: GitHub Actions Linux/Windows/macOS 빌드
- [ ] **Kitty Keyboard Protocol**: 정밀 키 입력

추가로 관찰된 개선 여지:
- `font_ligatures` 설정이 UI 에 없음
- `cursor_style` DECSCUSR 경로는 반영되지만 config 값이 런타임 DECSCUSR 변경을 덮어쓰지 않음 (의도된 동작)
- `bell_visual` config 필드만 있고 UI/주 로직 미연결
- 다중 클라이언트 resize 충돌 (서로 다른 창 크기 → 누구 기준?) — 현재는 마지막 resize wins
- 활성 pane / focus 는 브로드캐스트 안 됨 (의도적 — 각 클라이언트가 독립적으로 focus 유지)
