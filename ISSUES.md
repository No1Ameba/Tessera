# 알려진 이슈

## Open

(현재 없음 — pane close crash/freeze 는 2026-04-21 에 sync RPC async 전환으로 해결 후 재현되지 않음)

---

## Resolved

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
