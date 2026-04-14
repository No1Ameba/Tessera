# termemu

데몬 기반 세션 관리와 GPU 가속 렌더링을 지원하는 크로스 플랫폼 타일링 터미널 에뮬레이터.

## 특징

- **데몬 아키텍처** — 창을 닫아도 셸 세션이 유지됨 (tmux 방식)
- **타일링 레이아웃** — 이진 트리 기반 수직/수평 분할
- **GPU 렌더링** — OpenGL 3.3 Core 인스턴싱, FreeType + HarfBuzz
- **풀 VT 지원** — SGR 256색/트루컬러, CJK 와이드 문자, 마우스 프로토콜, 대체 화면
- **JSON 설정** — Windows Terminal 호환 테마 포맷

## 빠른 시작

```bash
# 의존성 (Ubuntu)
sudo apt install libx11-dev libgl-dev libfreetype-dev libharfbuzz-dev libcjson-dev cmake ninja-build

# 빌드
cmake -B build -G Ninja -DTERMEMU_BUILD_APPS=ON
cmake --build build

# 실행 (데몬 자동 스폰)
./build/src/client/termemu
```

## 문서

- **[사용 가이드](docs/usage.md)** — 빌드, 설정, 키 레퍼런스, 알려진 제한사항

## 기술 스택

| 레이어 | 기술 |
|--------|------|
| 언어 | C11 |
| 빌드 | CMake 3.20 + Ninja |
| 렌더링 | OpenGL 3.3 Core / GLX |
| 폰트 | FreeType 2 + HarfBuzz |
| 설정 | cJSON |
| IPC | Unix Domain Socket (epoll) |
| PTY | `forkpty` (POSIX) |

## 개발 현황

| Phase | 내용 | 상태 |
|-------|------|------|
| 1 | 공통 기반 (VT 파서, UTF-8, 설정) | 완료 |
| 2 | 데몬 (세션, PTY, IPC 서버) | 완료 |
| 3 | 렌더러 (폰트, 아틀라스, OpenGL) | 완료 |
| 4 | 클라이언트 UI (레이아웃, 입력, IPC) | 완료 |
| 5 | 화면 버퍼 (VT→cell grid) | 완료 |
| 6 | MVP 완성 (Wide char, 마우스, OSC) | 완료 |
| 7 | 통합 빌드 검증 | 완료 |
| 8 | 타일링 UI 완성 | 진행 중 |
| 9 | 스크롤백 뷰어 | 예정 |
| 10 | 테마 시스템 연결 | 예정 |

## 라이선스

MIT
