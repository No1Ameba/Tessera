# termemu 사용 가이드

## 목차

1. [요구 사항](#1-요구-사항)
2. [빌드](#2-빌드)
3. [실행 구조](#3-실행-구조)
4. [termemu-daemon](#4-termemu-daemon)
5. [termemu (클라이언트)](#5-termemu-클라이언트)
6. [설정 파일](#6-설정-파일)
7. [테마](#7-테마)
8. [키 입력 레퍼런스](#8-키-입력-레퍼런스)
9. [마우스 지원](#9-마우스-지원)
10. [알려진 제한사항](#10-알려진-제한사항)

---

## 1. 요구 사항

### 런타임

| 패키지 | 용도 |
|--------|------|
| X11 (`libX11`) | 디스플레이 서버 |
| OpenGL 3.3 Core + GLX | GPU 렌더링 |
| FreeType 2 (`libfreetype`) | 글리프 래스터화 |
| HarfBuzz (`libharfbuzz`) | 합자(ligature) 셰이핑 |
| cJSON (`libcjson`) | 설정/테마 JSON 파싱 |

### 빌드 도구

```
cmake >= 3.20
ninja (권장) 또는 make
C11 컴파일러 (gcc / clang)
```

### 의존성 설치 (Ubuntu / Debian)

```bash
sudo apt install \
  libx11-dev libgl-dev libfreetype-dev \
  libharfbuzz-dev libcjson-dev \
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
build/src/client/termemu          # X11 터미널 클라이언트
```

### 설치 (선택)

```bash
# 프로젝트 루트에서
sudo cmake --install build --prefix /usr/local

# 또는 build/ 디렉토리 안에서
cd build && sudo cmake --install . --prefix /usr/local

# 이후 PATH가 /usr/local/bin을 포함하면 termemu, termemu-daemon 으로 실행 가능
```

---

## 3. 실행 구조

```
┌──────────────────────────────────────────┐
│  termemu (클라이언트, X11 창)             │
│  - 키 입력 → IPC → 데몬                  │
│  - PTY 출력 ← IPC ← 데몬                 │
│  - VT 파싱 → 셀 그리드 → OpenGL 렌더링   │
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

**on-demand spawn**: 클라이언트가 소켓에 연결을 시도할 때 데몬이 없으면
`/proc/self/exe` 경로로 데몬을 자동 스폰한 뒤 재연결한다.
별도로 데몬을 먼저 실행할 필요 없이 **`termemu` 하나만 실행하면 된다.**

---

## 4. termemu-daemon

### 직접 실행

```bash
# 포그라운드 실행 (디버그용, 로그가 stderr에 출력됨)
./termemu-daemon

# 백그라운드 데몬으로 실행
./termemu-daemon --daemon

# 도움말
./termemu-daemon --help
```

### 소켓 경로

```
/tmp/termemu-<uid>.sock
```

예: UID 1000이면 `/tmp/termemu-1000.sock`

### 수동 종료

```bash
kill $(pgrep termemu-daemon)
# 또는
pkill termemu-daemon
```

SIGTERM / SIGINT 수신 시 열린 세션과 PTY를 모두 정리하고 종료한다.

### 세션 계층 구조

데몬은 다음 계층 구조로 상태를 관리한다:

```
Session  (이름 지정 가능, 예: "work")
  └── Window  (번호/이름 지정 가능, 예: "1")
        └── Pane  (실제 PTY 인스턴스)
```

클라이언트가 연결되면 자동으로 `"default"` 세션 → 윈도우 `"1"` → pane 1개가 생성된다.

---

## 5. termemu (클라이언트)

### 실행

```bash
./termemu
```

데몬이 없으면 자동으로 스폰하고 연결한다. `$DISPLAY` 환경변수가 설정된 X11 세션에서 실행해야 한다.

### 시작 순서

1. `~/.config/termemu/config.json` 로드 (없으면 기본값 사용)
2. X11 창 생성 (1280×720, OpenGL 3.3 Core)
3. 데몬 연결 (없으면 자동 스폰 후 재시도)
4. 세션/윈도우/pane 생성 → `$SHELL` 실행
5. 이벤트 루프 진입

### 창 크기 조정

창 크기를 바꾸면 자동으로 PTY 크기(`TIOCSWINSZ`)와 화면 버퍼가 함께 조정된다.

### 종료

- 셸에서 `exit` 입력
- 창 닫기 버튼 클릭 (WM_DELETE_WINDOW)

---

## 6. 설정 파일

### 위치

```
~/.config/termemu/config.json
```

파일이 없으면 아래 기본값이 적용된다.

### 전체 옵션

```jsonc
{
  // 폰트
  "font_family": "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
  "font_size": 12.0,
  "font_ligatures": false,

  // 창 모양
  "opacity": 1.0,           // 0.0(완전 투명) ~ 1.0(불투명)
  "padding_x": 4,           // 좌우 여백 (픽셀)
  "padding_y": 4,           // 상하 여백 (픽셀)

  // 스크롤백
  "scrollback_lines": 1000, // 1 ~ 100000

  // 커서
  "cursor_style": "block",  // "block" | "underline" | "bar"
  "cursor_blink": true,

  // 테마
  "theme_name": "dark",     // themes/ 디렉토리의 JSON 파일명 (확장자 제외)

  // 기타
  "bell_visual": false      // 벨 소리 대신 화면 번쩍임
}
```

### 설정 예시

```jsonc
{
  "font_family": "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
  "font_size": 14.0,
  "font_ligatures": true,
  "scrollback_lines": 5000,
  "cursor_style": "bar",
  "cursor_blink": true,
  "theme_name": "catppuccin-mocha"
}
```

---

## 7. 테마

### 위치

```
~/.config/termemu/themes/<이름>.json
```

### 포맷 (Windows Terminal `schemes` 호환)

```jsonc
{
  "name": "My Theme",
  "background": "#1e1e2e",
  "foreground": "#cdd6f4",
  "cursorColor": "#f5e0dc",
  "selectionBackground": "#45475a",

  "black":         "#45475a",
  "red":           "#f38ba8",
  "green":         "#a6e3a1",
  "yellow":        "#f9e2af",
  "blue":          "#89b4fa",
  "purple":        "#f5c2e7",
  "cyan":          "#94e2d5",
  "white":         "#bac2de",

  "brightBlack":   "#585b70",
  "brightRed":     "#f38ba8",
  "brightGreen":   "#a6e3a1",
  "brightYellow":  "#f9e2af",
  "brightBlue":    "#89b4fa",
  "brightPurple":  "#f5c2e7",
  "brightCyan":    "#94e2d5",
  "brightWhite":   "#a6adc8"
}
```

`config.json`의 `"theme_name"` 필드에 파일명(확장자 제외)을 지정하면 적용된다.

---

## 8. 키 입력 레퍼런스

### 커서 / 탐색

| 키 | 시퀀스 |
|----|--------|
| ↑ ↓ ← → | `ESC[A` `ESC[B` `ESC[C` `ESC[D` |
| Home | `ESC[H` |
| End | `ESC[F` |
| PgUp | `ESC[5~` |
| PgDn | `ESC[6~` |
| Insert | `ESC[2~` |
| Delete | `ESC[3~` |

### 기능키

| 키 | 시퀀스 |
|----|--------|
| F1–F4 | `ESC OP` `OQ` `OR` `OS` |
| F5–F12 | `ESC[15~` `17~` `18~` `19~` `20~` `21~` `23~` `24~` |

### 수정자 조합

| 조합 | 동작 |
|------|------|
| `Ctrl + 알파벳` | `0x01`–`0x1A` (예: Ctrl+C = `0x03`) |
| `Alt + 키` | `ESC` + 일반 바이트 |
| `Ctrl + [` | `ESC` |
| `Ctrl + \` | `0x1C` |
| `Ctrl + ]` | `0x1D` |

### 붙여넣기

| 방법 | 동작 |
|------|------|
| 마우스 중간 버튼 클릭 | X11 PRIMARY 선택 영역 붙여넣기 |

현재 앱이 브라켓 페이스트 모드(`?2004h`)를 활성화한 경우 붙여넣기 내용이
`ESC[200~` … `ESC[201~` 로 감싸져 전달된다.

---

## 9. 마우스 지원

터미널 앱이 마우스 추적 모드를 활성화한 경우 지원된다.

| 모드 | 시퀀스 | 설명 |
|------|--------|------|
| `ESC[?1000h` | X10 기본 | 버튼 클릭 보고 |
| `ESC[?1006h` | SGR 확장 | 버튼 + 위치를 숫자로 보고 (권장) |
| `ESC[?1000l` / `ESC[?1006l` | — | 모드 해제 |

마우스 추적이 활성화된 상태에서:
- 좌/우 클릭 및 릴리스가 PTY로 전달된다.
- 스크롤 휠 (버튼 4/5) 이벤트가 전달된다.
- SGR 형식: `ESC[<btn;col;rowM` (press) / `ESC[<btn;col;rowm` (release)

---

## 10. 알려진 제한사항

| 항목 | 상태 |
|------|------|
| 창 분할 (타일링) | 자료구조 구현 완료, UI 단축키 미연결 |
| 스크롤백 뷰어 | 버퍼 구현 완료, `Shift+PgUp` 탐색 미구현 |
| 커서 렌더링 | 화면에 커서가 표시되지 않음 |
| 테마 색상 반영 | 설정 파서 완료, 렌더러 연결 미완 — **기본 색상 가시성 낮음** |
| 폰트 렌더링 품질 | 기본 폰트(`DejaVuSansMono`) 사용 중, 힌팅/안티앨리어싱 미세조정 필요 |
| `exit` 시 창 미닫힘 | PTY EOF → `PANE_EXITED` 전달 구현됨, 클라이언트 수신 타이밍 문제로 미동작 |
| OSC 52 클립보드 | 미구현 |
| OSC 8 하이퍼링크 | 미구현 |
| macOS / Windows | POSIX/X11 전용, 타 플랫폼 미지원 |
| 다중 pane PTY 출력 | 현재 pane 1개만 렌더링 |

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
├── platform/posix/  # POSIX 플랫폼 추상화
│   ├── pty_posix.c  # forkpty / TIOCSWINSZ
│   └── ipc_posix.c  # Unix Domain Socket 래퍼
│
├── daemon/
│   ├── session.c    # Session/Window/Pane 트리
│   ├── ipc_server.c # epoll 이벤트 루프
│   └── main.c       # 엔트리포인트, 신호 처리
│
└── client/
    ├── screen.c        # VT 파서 → cell grid (화면 버퍼)
    ├── ipc_client.c    # 데몬 연결, on-demand spawn
    ├── ui/layout.c     # 이진 트리 타일링 레이아웃
    ├── ui/input.c      # X11 KeySym → VT 시퀀스
    ├── renderer/
    │   ├── font.c         # FreeType + HarfBuzz
    │   ├── glyph_atlas.c  # GPU 텍스처 아틀라스 (shelf packing)
    │   └── gl_renderer.c  # OpenGL 3.3 인스턴싱 렌더러
    └── main.c          # X11 + GLX 이벤트 루프
```
