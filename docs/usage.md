# termemu 사용 가이드

## 목차

1. [요구 사항](#1-요구-사항)
2. [빌드](#2-빌드)
3. [실행 구조](#3-실행-구조)
4. [termemu-daemon](#4-termemu-daemon)
5. [termemu (클라이언트)](#5-termemu-클라이언트)
6. [단축키 & 마우스](#6-단축키--마우스)
7. [설정 파일](#7-설정-파일)
8. [테마](#8-테마)
9. [설정 UI](#9-설정-ui)
10. [VT 호환성](#10-vt-호환성)
11. [알려진 제한사항](#11-알려진-제한사항)

---

## 1. 요구 사항

### 런타임

| 패키지 | 용도 |
|--------|------|
| OpenGL 3.3 Core | GPU 렌더링 |
| FreeType 2 (`libfreetype`) | 글리프 래스터화 |
| HarfBuzz (`libharfbuzz`) | 합자(ligature) 셰이핑 |
| cJSON (`libcjson`) | 설정/테마 JSON 파싱 |

GLFW 3.4는 빌드 시 FetchContent로 자동 다운로드됩니다.
Nuklear은 `vendor/nuklear/nuklear.h`에 벤더링되어 있습니다.

### 빌드 도구

```
cmake >= 3.20
ninja (권장) 또는 make
C11 컴파일러 (gcc / clang)
```

### 의존성 설치 (Ubuntu / Debian)

```bash
sudo apt install \
  libfreetype-dev libharfbuzz-dev libcjson-dev \
  libgl-dev libx11-dev libwayland-dev libxkbcommon-dev \
  cmake ninja-build
```

---

## 2. 빌드

### 테스트만 빌드 (기본)

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build        # 9개 테스트 실행
```

### 전체 빌드 (데몬 + 클라이언트 포함)

```bash
cmake -B build -G Ninja -DTERMEMU_BUILD_APPS=ON
cmake --build build
```

빌드 결과물:

```
build/src/daemon/termemu-daemon   # 백그라운드 세션 관리 데몬
build/src/client/termemu          # 터미널 클라이언트
```

### 설치 (선택)

```bash
sudo cmake --install build --prefix /usr/local
```

---

## 3. 실행 구조

```
┌──────────────────────────────────────────┐
│  termemu (클라이언트, GLFW 창)            │
│  - 키 입력 → IPC → 데몬                  │
│  - PTY 출력 ← IPC ← 데몬                 │
│  - VT 파싱 → 셀 그리드 → OpenGL 렌더링   │
│  - Nuklear 설정 오버레이                  │
└────────────────┬─────────────────────────┘
                 │  Unix Domain Socket
                 │  /tmp/termemu-<uid>.sock
┌────────────────┴─────────────────────────┐
│  termemu-daemon (백그라운드)              │
│  - Session / Window / Pane 트리 관리     │
│  - PTY spawn/read/write                  │
│  - epoll 기반 다중 클라이언트 처리        │
└──────────────────────────────────────────┘
```

**on-demand spawn**: `termemu` 하나만 실행하면 데몬이 없을 때 자동 스폰됩니다.

---

## 4. termemu-daemon

```bash
./termemu-daemon           # 포그라운드 (디버그용)
./termemu-daemon --daemon  # 백그라운드 데몬
./termemu-daemon --help    # 도움말
```

소켓 경로: `/tmp/termemu-<uid>.sock`

종료: `pkill termemu-daemon` (SIGTERM/SIGINT으로 정상 종료)

---

## 5. termemu (클라이언트)

```bash
termemu
```

1. `~/.config/termemu/config.json` 로드
2. GLFW 창 생성 (1280x720, OpenGL 3.3 Core)
3. 데몬 연결 (없으면 자동 스폰)
4. 기본 세션/윈도우/pane 생성 → `$SHELL` 실행

---

## 6. 단축키 & 마우스

### 타일링 (config.json `keybindings`에서 변경 가능)

| 기본 키 | 동작 |
|---------|------|
| `Alt + -` | 수직 분할 (좌/우) |
| `Alt + =` | 수평 분할 (상/하) |
| `Alt + h/j/k/l` | 인접 pane으로 포커스 이동 |
| `Ctrl + W` | 현재 pane 닫기 |
| `Shift + PgUp` | 스크롤백 위로 (한 페이지) |
| `Shift + PgDn` | 스크롤백 아래로 |
| `Ctrl + ,` | 설정 오버레이 토글 |

### 마우스

| 동작 | 설명 |
|------|------|
| **좌클릭 드래그** | 텍스트 선택 (자동 클립보드 복사) |
| **좌클릭** | 선택 해제 / pane 포커스 이동 |
| **중클릭** | 클립보드 붙여넣기 (브라켓 페이스트 지원) |
| **우클릭** | 컨텍스트 메뉴 (Settings, Split, Close) |
| **스크롤 휠** | 스크롤백 탐색 (마우스 모드 비활성 시) |

### 메뉴 버튼

창 우상단에 항상 표시되는 `=` 버튼 클릭 → 컨텍스트 메뉴:
- **Settings** — 설정 오버레이 열기
- **Split Vertical** — 좌우 분할
- **Split Horizontal** — 상하 분할
- **Close Pane** — 현재 pane 닫기

### 마우스 모드 (앱이 활성화한 경우)

마우스 모드(`ESC[?1000h` / `ESC[?1006h`)가 활성화되면 마우스 이벤트가 PTY로 전달됩니다.
이 경우 텍스트 선택과 우클릭 메뉴는 비활성됩니다.

---

## 7. 설정 파일

### 위치

```
~/.config/termemu/config.json
```

### 핫 리로드

설정 파일을 수정하면 inotify로 자동 감지되어 **재시작 없이 즉시 적용**됩니다.
수동 리로드: `kill -HUP $(pgrep termemu)`

### 전체 옵션

```jsonc
{
  "font": {
    "family": "monospace",        // 폰트 이름 또는 절대 경로
    "size": 14,                   // pt 단위
    "ligatures": true
  },
  "window": {
    "opacity": 1.0,               // 0.0(투명) ~ 1.0(불투명)
    "padding": { "x": 4, "y": 4 } // 여백 (픽셀)
  },
  "scrollback_lines": 10000,      // 1 ~ 100000
  "cursor": {
    "style": "block",             // "block" | "underline" | "bar"
    "blink": true
  },
  "theme": "custom",              // themes/ 디렉토리의 파일명 (확장자 제외)
  "bell": { "visual": false },
  "keybindings": {
    "split_vertical":   "Alt+minus",
    "split_horizontal": "Alt+equal",
    "focus_left":       "Alt+h",
    "focus_right":      "Alt+l",
    "focus_up":         "Alt+k",
    "focus_down":       "Alt+j",
    "close_pane":       "Ctrl+w",
    "scroll_up":        "Shift+Prior",
    "scroll_down":      "Shift+Next",
    "preferences":      "Ctrl+comma"
  }
}
```

---

## 8. 테마

### 위치

```
~/.config/termemu/themes/<이름>.json
```

### 포맷 (Windows Terminal `schemes` 호환)

```jsonc
{
  "name": "Catppuccin Mocha",
  "background": "#1e1e2e",
  "foreground": "#cdd6f4",
  "cursorColor": "#f5e0dc",
  "selectionBackground": "#45475a",
  "black":       "#45475a", "red":         "#f38ba8",
  "green":       "#a6e3a1", "yellow":      "#f9e2af",
  "blue":        "#89b4fa", "purple":      "#f5c2e7",
  "cyan":        "#94e2d5", "white":       "#bac2de",
  "brightBlack": "#585b70", "brightRed":   "#f38ba8",
  "brightGreen": "#a6e3a1", "brightYellow":"#f9e2af",
  "brightBlue":  "#89b4fa", "brightPurple":"#f5c2e7",
  "brightCyan":  "#94e2d5", "brightWhite": "#a6adc8"
}
```

---

## 9. 설정 UI

### 열기

- `Ctrl + ,` (키보드)
- 우상단 `=` 메뉴 버튼 → Settings
- 우클릭 → Settings

### 패널

| 섹션 | 설정 항목 |
|------|-----------|
| **Font** | 폰트 패밀리, 크기 |
| **Window** | 투명도 슬라이더, 패딩 |
| **Keybindings** | 10개 단축키 편집 |
| **Colors** | fg/bg 컬러 피커, ANSI 16색 팔레트 |

**Save & Apply** 버튼으로 config.json/theme.json에 저장하고 즉시 반영됩니다.

---

## 10. VT 호환성

### 지원 기능

| 분류 | 항목 |
|------|------|
| C0 제어문자 | CR, LF, TAB, BEL, BS |
| CSI 커서 이동 | CUU/CUD/CUF/CUB, CUP, CHA, VPA, CNL, CPL |
| CSI 지우기 | ED (0/1/2/3), EL (0/1/2), ECH, DCH, IL, DL |
| SGR 스타일 | Bold, Italic, Underline, Blink, Reverse |
| SGR 색상 | 16색 (`30-37;40-47;90-97;100-107`), 256색 (`38;5;N`), True-color (`38;2;R;G;B`) |
| 스크롤 | SU/SD, DECSTBM (`ESC[r`), 스크롤백 버퍼 (ring buffer) |
| 대체 화면 | `ESC[?1049h/l` (vim, less 등) |
| 마우스 | X10 (`?1000h`), SGR (`?1006h`) |
| 브라켓 페이스트 | `ESC[?2004h/l` |
| OSC 타이틀 | OSC 0/2 → 창 타이틀 변경 |
| OSC 52 | 클립보드 쓰기 (base64, vim `+clipboard` 연동) |
| 커서 스타일 | DECSCUSR `ESC[0-6 q` (block/underline/bar + blink) |
| Synchronized Output | `ESC[?2026h/l` — 빠른 출력 시 깜빡임 제거 |
| 문자 | UTF-8 (한글, 이모지, CJK 2칸 처리) |

---

## 11. 알려진 제한사항

| 항목 | 상태 |
|------|------|
| WSLg 마우스 커서 | WSLg X11에서 터미널 위 커서 안 보임 (WSLg 쪽 이슈) |
| Windows / macOS | GLFW 크로스 플랫폼 준비됨, IPC 레이어 미구현 |
| OSC 52 읽기 | 보안상 비활성 (쓰기만 지원) |
| OSC 8 하이퍼링크 | 미구현 |
| Kitty Image Protocol | 미구현 |
| 원격 세션 동기화 | 미구현 (계획됨) |

---

## 부록 — 소스 구조

```
src/
├── common/          # 공유 라이브러리 (데몬 + 클라이언트)
│   ├── vt_parser.c  # Paul Williams VT 상태 머신
│   ├── utf8.c       # UTF-8 인코딩 / 열 너비 (wcwidth)
│   ├── config.c     # JSON 설정 + 테마 파서
│   └── ipc_proto.h  # 데몬↔클라이언트 바이너리 프로토콜
│
├── platform/
│   ├── posix/
│   │   ├── pty_posix.c       # forkpty / TIOCSWINSZ
│   │   ├── ipc_posix.c       # Unix Domain Socket 래퍼
│   │   └── fs_watch_posix.c  # inotify 파일 감시
│   ├── fs_watch.h    # 파일 감시 추상화
│   ├── ipc.h         # IPC 소켓 추상화
│   └── termemu_pty.h # PTY 추상화
│
├── daemon/
│   ├── session.c    # Session/Window/Pane 트리
│   ├── ipc_server.c # epoll 이벤트 루프
│   └── main.c       # 엔트리포인트, 신호 처리
│
└── client/
    ├── screen.c           # VT 파서 → cell grid (화면 버퍼)
    ├── ipc_client.c       # 데몬 연결, on-demand spawn
    ├── ui/
    │   ├── layout.c       # 이진 트리 타일링 레이아웃
    │   ├── input.c        # GLFW key → VT 시퀀스
    │   ├── nk_impl.c      # Nuklear GL3 백엔드
    │   └── settings_ui.c  # Nuklear 설정 오버레이
    ├── renderer/
    │   ├── font.c         # FreeType + HarfBuzz
    │   ├── glyph_atlas.c  # GPU 텍스처 아틀라스 (shelf packing)
    │   └── gl_renderer.c  # OpenGL 3.3 인스턴싱 렌더러
    └── main.c             # GLFW 이벤트 루프
```
