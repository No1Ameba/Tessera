# 개발 진행 상황

## 모듈 상태

| 모듈 | 파일 | 상태 | TC |
|------|------|------|----|
| 빌드 시스템 | `CMakeLists.txt`, `cmake/` | ✅ 완료 | — |
| VT 파서 | `src/common/vt_parser.c/h` | ✅ 완료 | 55/55 |
| UTF-8 유틸리티 | `src/common/utf8.c/h` | ✅ 완료 | 53/53 |
| 설정 파서 | `src/common/config.c/h` | ✅ 완료 | 67/67 |
| 세션 관리 | `src/daemon/session.c/h` | ✅ 완료 | 77/77 |
| PTY 추상화 | `src/platform/posix/pty_posix.c` | ✅ 완료 | 24/24 |
| IPC 프로토콜 | `src/common/ipc_proto.h` | ✅ 완료 | — |
| IPC 소켓 래퍼 | `src/platform/posix/ipc_posix.c` | ✅ 완료 | — |
| IPC 서버 | `src/daemon/ipc_server.c/h` | ✅ 완료 | 42/42 |
| IPC 클라이언트 | `src/client/ipc_client.c/h` | ✅ 완료 | — |
| 데몬 메인 루프 | `src/daemon/main.c` | ✅ 완료 | — |
| 폰트 렌더러 | `src/client/renderer/font.c/h` | ✅ 완료 | 14/14 |
| 글리프 아틀라스 | `src/client/renderer/glyph_atlas.c/h` | ✅ 완료 | — |
| OpenGL 렌더러 | `src/client/renderer/gl_renderer.c/h` | ✅ 완료 | — |
| 타일링 레이아웃 | `src/client/ui/layout.c/h` | ✅ 완료 | 29/29 |
| 입력 처리 | `src/client/ui/input.c/h` | ✅ 완료 (GLFW) | — |
| Nuklear 설정 UI | `src/client/ui/nk_impl.c/h`, `settings_ui.c/h` | ✅ 완료 | — |
| 클라이언트 메인 루프 | `src/client/main.c` | ✅ 완료 (GLFW) | — |
| 커서 렌더링 | `src/client/renderer/gl_renderer.c` | ✅ 완료 | — |
| 셀 그리드 정의 | `src/client/cell.h` | ✅ 완료 | — |
| 화면 버퍼 (VT→cell) | `src/client/screen.c/h` | ✅ 완료 | 15/15 |

---

## 구현 순서 (권장)

### Phase 1 — 공통 기반 (Common Layer)
1. ✅ `vt_parser` — VT 이스케이프 상태 머신
2. ✅ `utf8` — 인코딩/디코딩/열 너비
3. ✅ `config` — JSON 설정 및 테마 파싱
4. ✅ `ipc_proto.h` — 데몬↔클라이언트 메시지 구조체 정의

### Phase 2 — 데몬 (Daemon)
5. ✅ `session` — Session/Window/Pane 트리 자료구조
6. ✅ `platform/pty` — PTY spawn/read/write/resize 추상화
7. ✅ `ipc_server` — Unix Socket epoll 서버 루프 (42 TC)
8. ✅ `daemon/main` — 신호 처리, 생명주기 관리

### Phase 3 — 클라이언트 렌더링 (Client / Renderer)
9. ✅ `font` — FreeType + HarfBuzz 글리프 래스터화 (14 TC)
10. ✅ `glyph_atlas` — shelf-based GPU 텍스처 아틀라스 캐시
11. ✅ `gl_renderer` — OpenGL 3.3 인스턴싱 셀 렌더링 파이프라인

### Phase 4 — 클라이언트 UI (Client / UI)
12. ✅ `layout` — 이진 트리 타일링 (수직/수평 분할) (29 TC)
13. ✅ `input` — X11 KeySym → VT/ANSI 시퀀스 변환
14. ✅ `ipc_client` — 데몬 연결, 메시지 송수신, on-demand spawn
15. ✅ `client/main` — X11 GLX Core Profile 창 + 이벤트 루프

### Phase 5 — 화면 버퍼 (Screen Buffer)
16. ✅ `cell.h` — `term_cell_t` / `pane_rect_t` 공유 정의 (GL 분리)
17. ✅ `screen` — VT 파서 → cell grid (SGR/CSI/ESC/스크롤백/대체화면) (15 TC)

### Phase 7 — 통합 빌드 (`TERMEMU_BUILD_APPS=ON`)
- ✅ `fs_watch_posix.c` stub 채움 (빈 TU 에러 수정)
- ✅ `ipc_client.c` — `fcntl.h`, `sys/un.h` 누락 추가
- ✅ `src/client/CMakeLists.txt` — `screen.c` 소스 추가
- ✅ `termemu-daemon` (103KB) + `termemu` (235KB) 링크 성공
- ✅ 9/9 기존 테스트 여전히 통과

### Phase 6 — MVP 완성 (VT 1단계 나머지)
18. ✅ Wide character — CJK/이모지 2칸 처리, `CELL_ATTR_WIDE/WIDE_CONT`
19. ✅ 마우스 프로토콜 — `?1000h/?1006h` 추적 모드 + main.c 버튼/스크롤 이벤트
20. ✅ OSC 타이틀 — OSC 0/2 → `screen_t.title` + X11 `XStoreName`
21. ✅ 브라켓 페이스트 — `?2004h/l` + X11 PRIMARY 선택 붙여넣기 (`ESC[200~..201~`)
22. ✅ 커서 스타일 — DECSCUSR `ESC[N q` → `screen_t.cursor_style` (21 TC)

---

## 최근 개선 사항

### 다중 클라이언트 동기화 (2026-04-15 ~ 2026-04-21)
- **원격 실시간 동기화**: `ipc_client_poll()` fd 가드가 pipe 모드를 무시하던 문제 수정 (local fd 만 체크 → `read_fd(c)` 사용).
- **Split 브로드캐스트**: `IPC_MSG_PANE_SPLIT_NOTIFY` 추가. 한 클라이언트가 split 하면 동일 세션의 다른 클라이언트에도 layout 반영.
- **Pane close 브로드캐스트**: `PANE_EXITED` 를 요청자 외 모두에게 전파.
- **Layout 블롭 업로드**: `IPC_MSG_WINDOW_LAYOUT` 추가. 클라이언트가 직렬화된 layout tree 를 데몬에 보관 → 재접속 시 트리 복원.

### 세션 수명 정책 (2026-04-18)
- 모든 클라이언트 detach 후에도 pane 이 남아있으면 **5 분간 유예**, 그 뒤 destroy.
- pane 0 개로 닫히는 세션은 즉시 destroy (`session_destroy_if_empty`).
- 타임아웃 · autosave 주기는 config 로 튜닝 (`daemon.session_idle_timeout`, `daemon.autosave_interval`).

### 크래시 복구 (2026-04-19)
- 세션 스냅샷을 `~/.config/termemu/sessions/*.json` 에 주기 저장 (cJSON).
- 데몬 시작 시 `ipc_server_restore_sessions()` 가 스냅샷 스캔 → 세션·window·layout blob 복원.
- `save_all_sessions()` 를 autosave / 종료 시 공통 호출, destroy 경로에서 snapshot 삭제.

### 설정창 핫 리로드 확장 (2026-04-21)
- 기존에 theme / opacity 만 적용되던 `do_config_reload()` 에 다음 필드 추가:
  - 폰트 family / size (`gl_renderer_set_font()` 도입, atlas 재생성)
  - padding (`compute_layout_rect()` 헬퍼로 7 개 호출부 통일)
  - scrollback_lines (전역 `g_scrollback_lines`)
- 종전에는 설정 저장 후 재시작해야 반영되던 항목들이 즉시 적용됨.

### OSC 8 하이퍼링크 (2026-04-20)
- `term_cell_t.link_id` (16bit) 추가, `screen_t.links[64]` 링버퍼.
- OSC 8 파서: `ESC ] 8 ; params ; URI ST`, dedup, 자동 recycling.
- 렌더링: CELL_ATTR_UNDERLINE 자동 부여 + 렌더러 underline pass 추가.
- 클라이언트: Ctrl+Click → `xdg-open` / `open` 으로 URL 실행.

---

## TODO (우선순위 순)

1. [x] **선택 + 복사:** 마우스 드래그 텍스트 선택 → 클립보드 복사 ✅
2. [x] **OSC 52 클립보드:** 터미널 앱(vim 등)에서 클립보드 읽기/쓰기 ✅
3. [x] **Synchronized Output:** `?2026h` — 빠른 출력 시 깜빡임 제거 ✅
4. [x] **설정 핫 리로드:** inotify로 config.json 변경 감지 → 자동 적용 ✅
5. [x] **원격 세션 동기화:** SSH 터널 통한 원격 daemon attach (termemu-bridge + PTY 링 버퍼) ✅
6. [ ] **Windows IPC:** ConPTY + Named Pipe 구현
7. [x] **OSC 8 하이퍼링크:** 클릭 가능한 URL ✅
8. [ ] **Kitty Image Protocol:** 이미지 인라인 표시
9. [x] **크래시 복구:** 데몬 상태 저장/복원 ✅
10. [ ] **CI 파이프라인:** GitHub Actions Linux/Windows/macOS 빌드
11. [ ] **Kitty Keyboard Protocol:** 정밀 키 입력 (vim 미지원, 최하 우선순위)

---

## UX / UI 백로그 (2026-04-21 추가)

### 선택 동작 개선
12. [x] **드래그 + 스크롤 시 선택 영역 유지** ✅ (screen_t.scroll_epoch + 절대 행 인덱스 LI 앵커링, 2026-04-21)
    - 현상: 드래그 중 스크롤이 발생하면 선택 영역이 화면 좌표에 고정되어,
      스크롤되면서 컨텐츠가 내려가도 "처음 클릭한 글자" 가 아닌 "그 자리에
      지금 있는 글자" 가 선택 시작점으로 남는다.
    - 기대: 클릭 시점의 **절대 (스크롤백 포함) 위치**를 앵커로 저장하고,
      스크롤되어도 해당 글자가 앵커로 따라가도록 한다.
    - 영향 범위: `src/client/main.c` 의 `g_sel_sc/sr/ec/er` → 절대 행 좌표로 변경.
      `screen_t.sb_offset / sb_head / sb_count` 활용하여 view ↔ 절대 좌표 변환.
    - 렌더러(`gl_renderer_draw_cells`) 의 `sel_sc/sr/ec/er` 파라미터는
      현재 뷰 기준이므로, 호출부에서 변환해 넘긴다.

13. [x] **더블클릭 = 단어 선택, 트리플클릭 = 라인 선택** ✅ (앵커 + 드래그 union 확장 지원, 2026-04-21)
    - 현재: 드래그만 지원.
    - 기대: 더블클릭 시 word boundary 로 선택 영역 확장, 트리플클릭 시 해당
      라인 전체 선택. 선택 영역은 드래그 선택과 동일하게 클립보드 복사 처리.
    - 영향 범위: `mouse_button_callback` 에 클릭 카운팅 로직 추가
      (300ms 이내 같은 셀 연속 클릭). word boundary 는
      `[A-Za-z0-9_]` 연속 vs 공백/기호 경계로 판정.

### 테마 관리
14. [x] **샘플 테마 + 드롭다운 선택** ✅ (VS Code Dark+ / Solarized Dark/Light / Dracula / Nord / Gruvbox Dark + Custom 자동 전환)
    - 내장 샘플: VS Code Dark+ / Solarized Dark / Solarized Light /
      Dracula / Nord / Gruvbox 등 4~6 개.
    - 설정창 Colors 탭 상단에 Theme 드롭다운. 선택 시 전체 팔레트 교체.
    - 사용자가 팔레트 셀 하나라도 바꾸면 드롭다운이 자동으로 "Custom" 으로
      전환 (내장 테마와 한 군데라도 다르면 Custom).

15. [x] **테마 Import / Export UI** ✅ (#16 작업에서 settings Export 탭 버튼으로 함께 구현)
    - Export 버튼: 현재 테마를 파일로 저장. 파일 picker 로 경로 지정.
    - Import 버튼: 파일 picker 로 JSON 테마 로드 후 적용.
      (기존 Export 텍스트 입력 방식은 파일 picker 로 대체.)
    - 기존 `theme_save_file` / `theme_load_string` 재사용.

### 파일 picker (공통 인프라)
16. [x] **네이티브 파일 선택 다이얼로그** ✅ (zenity/kdialog popen 래퍼, Config/Theme Import·Export 에서 사용)
    - Nuklear 는 파일 picker 미제공.
    - 옵션 A: `zenity` / `kdialog` 외부 프로세스 호출
      (Linux 다수 환경에서 설치되어 있음, popen 으로 경로 문자열 수신).
    - 옵션 B: `tinyfiledialogs` 라이브러리 임베드 (크로스 플랫폼).
    - 먼저 옵션 A 로 간단히 구현, 실패(프로그램 없음) 시 텍스트 입력 폴백.
    - Config Import / Export, Theme Import / Export 에서 공통 사용.

### 포커스 / 창 관리
17. [x] **pane 닫힘 시 포커스를 부모(자신을 만든 pane)로 이동** ✅ (옵션 B, 2026-04-22)
    - `pane_slot_t` 에 `parent_pane_id` 필드 추가. `do_split` 은 `g_active_pane`,
      `on_pane_split` 은 notify 의 `parent_pane_id` 값을 기록.
    - `on_pane_exited` / `do_close_pane` 는 먼저 parent_pane_id 가 살아있으면 그쪽으로,
      아니면 기존 배열 순회 fallback.
    - attach/new/import 경로의 초기 pane 은 parent=0 (부모 없음) 으로 유지 —
      fallback 동작이 기존과 동일.

---

## 테스트 빌드 및 실행 방법

```bash
# CMake로 전체 테스트 빌드 + 실행 (권장)
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build

# 앱(daemon/client)까지 빌드할 때 (미구현 모듈 완성 후)
cmake -B build -G Ninja -DTERMEMU_BUILD_APPS=ON && cmake --build build
```

### 단독 gcc 빌드 (cmake 없이 빠르게 확인)

```bash
# VT 파서
gcc -std=c11 -I src/common src/common/vt_parser.c tests/test_vt_parser.c -o /tmp/test_vt && /tmp/test_vt

# UTF-8
gcc -std=c11 -I src/common src/common/utf8.c tests/test_utf8.c -o /tmp/test_utf8 && /tmp/test_utf8

# Config (libcjson-dev 필요)
gcc -std=c11 -I src/common src/common/utf8.c src/common/config.c tests/test_config.c -lcjson -o /tmp/test_config && /tmp/test_config

# 세션 관리
gcc -std=c11 -I src/daemon -I src/common src/daemon/session.c tests/test_session.c -o /tmp/test_session && /tmp/test_session

# PTY 추상화
gcc -std=c11 -I src/platform src/platform/posix/pty_posix.c tests/test_pty.c -lutil -o /tmp/test_pty && /tmp/test_pty
```
