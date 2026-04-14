# Architecture & System Specification

## 1. 데몬-클라이언트 아키텍처 (Client-Server Model)
- **Daemon (Server):** 백그라운드 프로세스로 동작.
  - PTY(Pseudo-Terminal) 스폰 및 생명주기 관리.
  - 터미널 스크롤백 버퍼 및 화면 상태(State) 메모리 유지.
  - 세션/윈도우/페인 트리 구조 관리.
- **Client (UI):** 사용자 입력을 받아 데몬으로 전송.
  - 데몬으로부터 화면 상태 업데이트를 수신하여 렌더링.

## 2. 세션 관리 (Session Management)

### 계층 구조
```
Daemon
└── Session (이름 지정 가능, 예: "work", "personal")
    └── Window (탭 단위, 번호/이름 지정)
        └── Pane (실제 PTY 인스턴스, 타일링 분할 단위)
```

### 데몬 생명주기
- **시작 방식 (On-demand):** 클라이언트가 IPC 소켓에 연결 시도 시 데몬이 없으면 자동 spawn.
  - 클라이언트가 fork 후 데몬 프로세스를 백그라운드로 분리.
  - 소켓 준비 완료까지 클라이언트가 대기 후 재연결.
- **종료 조건:** 모든 세션이 소멸되면 데몬 자동 종료.
- **명시적 제어 CLI:**
  - `termemu daemon start` / `termemu daemon stop`
  - `termemu session list` / `termemu session attach <name>`

### 스크롤백 버퍼
- **기본값:** 세션당 10,000줄.
- **최대값:** 100,000줄 (설정 파일 `config.json`의 `scrollback_lines` 항목으로 조정).
- 버퍼 초과 시 가장 오래된 줄부터 순환 삭제(Ring Buffer 구조).

### TODO (향후 구현)
- [ ] **크래시 복구:** 데몬 비정상 종료 시 세션 상태를 디스크에 저장 후 재시작 시 복구.
- [ ] **무제한 스크롤백:** `scrollback_lines: 0` 설정 시 메모리 허용 범위 내 무제한 버퍼.

## 3. 크로스 플랫폼 IPC (Inter-Process Communication)
- **POSIX (Linux/macOS 등):** Unix Domain Sockets 사용.
- **Windows:** Named Pipes 사용 (ACL로 현재 사용자만 접근 허용).
- **Protocol:** 자체 경량 바이너리 프로토콜 (C 구조체 직렬화 기반). FlatBuffers/Protobuf는 의존성 최소화 원칙에 따라 사용하지 않음.

## 4. PTY (Pseudo-Terminal) 제어
- **POSIX (Linux/macOS 등):** `pty.h`, `forkpty` 기반 구현.
- **Windows:** Windows Pseudo Console (ConPTY) API 사용.
- **추상화 레이어:** `pty_spawn()`, `pty_write()`, `pty_resize()` 등 플랫폼별 구현을 공통 인터페이스로 래핑.

## 5. VT 파서 (ANSI/VT 이스케이프 시퀀스 처리)

- **구현 방식:** Paul Williams의 VT 파서 상태 머신 다이어그램 기반.
  - 상태: `Ground → Escape → CSI Entry → CSI Param → CSI Final → dispatch`
- **단계별 구현 범위:**

### 1단계 (MVP)
| 분류 | 항목 |
|------|------|
| C0 제어문자 | `\r`, `\n`, `\t`, `\a`(BEL), `\b`(BS) |
| CSI 커서 이동 | `ESC[H`, `ESC[A/B/C/D` (절대/상대 이동) |
| CSI 화면 지우기 | `ESC[J` (화면), `ESC[K` (라인) |
| SGR 색상/스타일 | 굵기/이탤릭/밑줄, 16색, 256색, True-color (38;2;R;G;B) |
| 대체 화면 버퍼 | `ESC[?1049h/l` |
| 스크롤 영역 | `ESC[r` |
| 문자셋 | UTF-8 (한글, 이모지, CJK) |
| 마우스 프로토콜 | `ESC[?1000h`, SGR 마우스 (`ESC[?1006h`) |
| 브라켓 페이스트 | `ESC[?2004h/l` |
| OSC 타이틀 | `OSC 0;2` — 창/탭 타이틀 변경 |

### 2단계
| 분류 | 항목 |
|------|------|
| OSC 52 | 클립보드 읽기/쓰기 (neovim 연동) |
| OSC 8 | 클릭 가능한 하이퍼링크 |
| 커서 스타일 | `ESC[1-6 q` (블록/언더라인/바 + 블링크) |

### 3단계 (선택)
| 분류 | 항목 |
|------|------|
| Synchronized Output | `ESC[?2026h` — 화면 깜빡임 제거 |
| Kitty Keyboard Protocol | `ESC[?u` — 키 입력 정밀도 향상 (neovim 0.10+ 권장) |
| OSC 7 | 현재 디렉토리 알림 (탭 경로 표시) |

## 6. 설정 파일 구조
- **위치:** OS별 표준 설정 디렉토리 사용.
  - Linux/macOS: `$XDG_CONFIG_HOME/termemu/` (기본값: `~/.config/termemu/`)
  - Windows: `%APPDATA%\termemu\`
- **파일 구성:**
  - `config.json` — 전체 동작 설정 (폰트, 단축키, 동작 옵션)
  - `themes/` 디렉토리 — 테마 JSON 파일 모음 (Windows Terminal 스키마 호환)
- **핫 리로드:** 설정 파일 변경 감지 시 재시작 없이 즉시 적용 (inotify / ReadDirectoryChangesW).
