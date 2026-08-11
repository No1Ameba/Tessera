# 클라이언트 실행 파일은 미구현 파일이 완성된 이후에만 빌드
if(TESSERA_BUILD_APPS)
    add_executable(tessera
        main.c
        font_resolve.c
        pane_store.c
        text_select.c
        client_util.c
        screen.c
        ipc_client.c
        renderer/gl_renderer.c
        renderer/glyph_atlas.c
        renderer/font.c
        ui/layout.c
        ui/input.c
        ui/nk_impl.c
        ui/settings_ui.c
        ui/file_picker.c
        ui/confirm_dialog.c
        ui/ui_overlay.c
        ui/status_bar.c
    )

    target_include_directories(tessera PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/nuklear
    )

    # 벤더링된 nuklear.h 는 /W4 를 통과하지 못한다(GCC -Wall -Wextra 에서는 조용함).
    # 헤더가 클라이언트 TU 대부분에 포함되므로 타깃 단위로 해당 경고만 끈다.
    #   C4116 괄호 없는 타입 정의 / C4127 상수 조건식
    #   C5287 서로 다른 열거형 간 연산 / C4701 초기화되지 않았을 수 있는 지역 변수
    if(MSVC)
        target_compile_options(tessera PRIVATE /wd4116 /wd4127 /wd5287 /wd4701)
    endif()

    # OpenGL (GLFW creates the context, but we still need to link the GL library)
    find_package(OpenGL REQUIRED)

    target_link_libraries(tessera
        PRIVATE tessera_common
        PRIVATE tessera_platform
        PRIVATE tessera_ipc
        PRIVATE glad
        PRIVATE glfw
        PRIVATE OpenGL::GL
        PRIVATE Freetype::Freetype
        PRIVATE tessera_harfbuzz
    )

    # comdlg32: ui/file_picker.c 의 GetOpenFileNameW / GetSaveFileNameW
    if(WIN32)
        target_link_libraries(tessera PRIVATE comdlg32)
    endif()

    install(TARGETS tessera RUNTIME DESTINATION bin)
endif()
