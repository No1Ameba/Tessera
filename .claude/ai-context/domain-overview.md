# Domain Overview: Terminal Emulator

## 1. 프로젝트 목적
POSIX 호환 OS(Linux, macOS 등) 및 Windows(10/11)를 포함한 다중 플랫폼을 지원하는 고성능 타일링 터미널 에뮬레이터 개발.
단순한 화면 출력을 넘어, Tmux 수준의 백그라운드 세션 관리와 Tilix 스타일의 직관적인 창 분할 기능을 네이티브 데스크탑 앱으로 제공한다.

## 2. 기술 스택
- **언어:** C (C11 표준) — 최대 이식성과 직접적인 시스템 API 접근을 위해 선택.
- **빌드 시스템:** CMake (크로스 플랫폼 빌드 지원).
- **의존성 최소화 원칙:** 외부 라이브러리는 렌더링(OpenGL/Vulkan)과 JSON 파싱(cJSON 등) 용도로 한정.

## 3. 핵심 기능
- **터미널 에뮬레이션:** 표준 ANSI 이스케이프 시퀀스 (VT100/VT220/xterm) 완벽 지원.
- **세션 관리:** UI가 종료되어도 셸 프로세스와 상태를 유지하는 데몬(Daemon) 구조.
- **타일링 UI:** 트리 기반(Tree-based) 데이터 구조를 활용한 화면 수직/수평 분할.
- **환경 설정:** JSON 기반의 핫 리로드(Hot-reload) 설정 파일 지원.
- **테마 시스템:** Windows Terminal / iTerm2 호환 JSON 포맷으로 import/export 지원.

## 4. 테마 시스템 상세
- **기준 포맷:** Windows Terminal `schemes` JSON 스키마를 주(主) 포맷으로 채택.
  - `name`, `background`, `foreground`, `cursorColor`, `selectionBackground`
  - ANSI 16색 (`black`, `red`, `green`, ... `brightWhite`)
- **iTerm2 호환:** `.itermcolors` → JSON 변환 import 지원.
- **목표:** Dracula, Catppuccin, Gruvbox 등 커뮤니티 테마를 수정 없이 import 가능.
- **플러그인/스크립팅 확장 시스템은 제공하지 않음.** 확장은 테마/설정 파일로만 대응.
