# Rendering & Media Specification

## 1. GPU 가속 렌더링

- **API:** OpenGL 3.3 Core Profile (크로스 플랫폼 기준선).
  - macOS: OpenGL Deprecated 이슈로 향후 Metal 래퍼 고려 가능하나, 현재는 OpenGL로 확정.
  - Windows: WGL, Linux: GLX / EGL 을 통해 컨텍스트 생성.
- **렌더링 파이프라인:**
  - 글리프 아틀라스(Glyph Atlas): 래스터화된 글리프를 GPU 텍스처에 캐싱.
  - 셀(Cell) 단위 배치: 터미널 그리드 각 셀을 쿼드(Quad)로 렌더링.
  - 더티 리전(Dirty Region) 최적화: 변경된 셀만 재렌더링하여 GPU 부하 최소화.
- **색상 및 시각 효과:**
  - True-color (24-bit RGB) 지원.
  - 배경 투명도(Alpha blending) 지원.
  - 커서 애니메이션 (블링크) 지원.

## 2. 폰트 렌더링

- **래스터화:** FreeType 라이브러리로 `.ttf` / `.otf` 폰트 파일을 픽셀 비트맵으로 변환.
- **셰이핑(Shaping):** HarfBuzz 라이브러리로 ligature(합자) 및 복합 글리프 처리.
  - 예: Fira Code / JetBrains Mono 의 `!=`, `->`, `=>` 등 프로그래밍 ligature 지원.
- **폴백 폰트:** 주 폰트에 없는 문자(이모지, CJK 등)를 시스템 폴백 폰트로 대체 렌더링.
- **Nerd Fonts:** 아이콘 폰트(PUA 영역) 글리프 렌더링 지원.
- **서브픽셀 힌팅:** FreeType LCD 필터 적용으로 가독성 향상 (설정으로 on/off 가능).

## 3. 리치 미디어 (Rich Media) 인라인 출력

- **이미지 프로토콜:** Kitty Image Protocol 구현.
  - 셀 기반 정확한 이미지 배치, 투명도(Alpha) 지원.
  - 이미지 데이터: Base64 인코딩 후 OSC 이스케이프 시퀀스로 전송.
  - 지원 포맷: PNG, JPEG, (확장 고려: GIF 첫 프레임).
- **PDF 인라인 출력:** 미지원. 외부 툴에서 이미지 변환 후 Kitty Protocol로 출력하는 방식 권장.

## 4. 의존성 요약

| 라이브러리 | 용도 | 라이선스 |
|---|---|---|
| FreeType | 폰트 래스터화 | FTL (BSD-like) |
| HarfBuzz | 글리프 셰이핑 / ligature | MIT |
| OpenGL (glad/glew) | GPU 렌더링 | MIT / 시스템 제공 |
