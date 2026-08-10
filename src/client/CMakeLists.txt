# 클라이언트 실행 파일은 미구현 파일이 완성된 이후에만 빌드
if(TERMEMU_BUILD_APPS)
    add_executable(termemu
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
    )

    target_include_directories(termemu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/nuklear
    )

    # OpenGL (GLFW creates the context, but we still need to link the GL library)
    find_package(OpenGL REQUIRED)

    target_link_libraries(termemu
        PRIVATE termemu_common
        PRIVATE termemu_platform
        PRIVATE termemu_ipc
        PRIVATE glad
        PRIVATE glfw
        PRIVATE OpenGL::GL
        PRIVATE Freetype::Freetype
        PRIVATE termemu_harfbuzz
    )

    install(TARGETS termemu RUNTIME DESTINATION bin)
endif()
