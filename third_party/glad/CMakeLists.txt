# glad - OpenGL 3.3 Core loader (MIT License, vendored)
# 파일 출처: https://glad.dav1d.de/
#   - Language: C/C++
#   - Specification: gl
#   - Profile: Core
#   - API gl: Version 3.3
#   - Extensions: none
#
# 아래 파일 2개를 직접 다운로드하여 배치:
#   include/glad/glad.h
#   src/glad.c

add_library(glad STATIC src/glad.c)

# glad 생성 코드는 -Werror/-pedantic과 호환되지 않음
target_compile_options(glad PRIVATE -Wno-error -Wno-pedantic)

target_include_directories(glad
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# glad가 dlopen으로 GL 함수를 로드하므로 dl 라이브러리 필요 (Linux)
if(UNIX AND NOT APPLE)
    target_link_libraries(glad PRIVATE dl)
endif()
