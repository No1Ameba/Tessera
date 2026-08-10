cmake_minimum_required(VERSION 3.20)
project(termemu VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# 빌드 옵션
# -----------------------------------------------------------------------------
option(TERMEMU_BUILD_APPS "데몬/클라이언트 실행 파일 빌드 (미구현 모듈 완성 후 활성화)" OFF)
option(TERMEMU_SANITIZE "AddressSanitizer + UBSan 계측 빌드 (디버그/CI 권장)" OFF)

# -----------------------------------------------------------------------------
# 플랫폼 감지
# -----------------------------------------------------------------------------
include(cmake/platform.cmake)

# -----------------------------------------------------------------------------
# 컴파일 옵션
# -----------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

# 메모리/UB 계측: VT 파서·IPC·config 파싱 등 신뢰할 수 없는 입력 경로의
# 오버런/UAF/정수 UB 를 런타임에 검출한다. 기본 OFF, CI/디버그에서 -DTERMEMU_SANITIZE=ON.
if(TERMEMU_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# -----------------------------------------------------------------------------
# 시스템 의존성 (apt / brew 로 설치)
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# 의존성 탐지: Linux/macOS 는 pkg-config, Windows(vcpkg)는 CMake config 패키지를
# 쓴다. 어느 경로로 찾든 나머지 빌드는 아래 일관된 INTERFACE 타깃으로만 링크한다
# (termemu_cjson / termemu_harfbuzz / Freetype::Freetype) — 탐지 방식과 분리.
add_library(termemu_cjson    INTERFACE)
add_library(termemu_harfbuzz INTERFACE)

if(WIN32)
    # vcpkg: `vcpkg install cjson harfbuzz freetype` + -DCMAKE_TOOLCHAIN_FILE=…
    # (타깃명은 포트 버전에 따라 다를 수 있어 Windows CI 에서 최종 검증 필요.)
    find_package(cJSON CONFIG REQUIRED)
    if(TARGET cjson::cjson)
        target_link_libraries(termemu_cjson INTERFACE cjson::cjson)
    else()
        target_link_libraries(termemu_cjson INTERFACE cjson)
    endif()
    find_package(harfbuzz CONFIG REQUIRED)
    target_link_libraries(termemu_harfbuzz INTERFACE harfbuzz::harfbuzz)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CJSON    REQUIRED IMPORTED_TARGET libcjson)  # apt: libcjson-dev | brew: cjson
    pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)  # apt: libharfbuzz-dev | brew: harfbuzz
    target_link_libraries(termemu_cjson    INTERFACE PkgConfig::CJSON)
    target_link_libraries(termemu_harfbuzz INTERFACE PkgConfig::HARFBUZZ)
endif()

# 렌더링 의존성(테스트 test_font 에서도 필요하므로 항상 탐색).
# FreeType 은 표준 CMake 모듈 + vcpkg 모두 Freetype::Freetype 타깃을 제공.
find_package(Freetype REQUIRED)         # apt: libfreetype6-dev | brew: freetype | vcpkg: freetype

# -----------------------------------------------------------------------------
# Vendored: glad (OpenGL 3.3 Core, MIT) — 클라이언트 빌드 시에만 필요
# 생성 옵션: https://glad.dav1d.de/ → gl, core, 3.3, C/C++
# -----------------------------------------------------------------------------
if(TERMEMU_BUILD_APPS)
    add_subdirectory(third_party/glad)

    # GLFW3 — 크로스 플랫폼 창/입력/GL 컨텍스트
    include(FetchContent)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glfw)
    # GLFW 소스는 -Werror로 컴파일하면 실패 → 해제
    if(TARGET glfw)
        target_compile_options(glfw PRIVATE -Wno-error)
    endif()
endif()

# -----------------------------------------------------------------------------
# 공통 라이브러리 (daemon + client 공유)
# -----------------------------------------------------------------------------
add_subdirectory(src/common)

# -----------------------------------------------------------------------------
# 플랫폼 추상화 라이브러리 (termemu_pty 는 항상 빌드, IPC/fswatch 는 앱 빌드 시)
# -----------------------------------------------------------------------------
add_subdirectory(src/platform)

# -----------------------------------------------------------------------------
# 데몬 실행 파일
# -----------------------------------------------------------------------------
add_subdirectory(src/daemon)

# -----------------------------------------------------------------------------
# 브릿지 (원격 세션 동기화)
# -----------------------------------------------------------------------------
add_subdirectory(src/bridge)

# -----------------------------------------------------------------------------
# 클라이언트 실행 파일
# -----------------------------------------------------------------------------
add_subdirectory(src/client)

# -----------------------------------------------------------------------------
# 테스트
# -----------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
