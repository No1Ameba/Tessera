add_executable(test_vt_parser test_vt_parser.c)
target_link_libraries(test_vt_parser PRIVATE termemu_common)
add_test(NAME vt_parser COMMAND test_vt_parser)

add_executable(test_utf8 test_utf8.c)
target_link_libraries(test_utf8 PRIVATE termemu_common)
add_test(NAME utf8 COMMAND test_utf8)

add_executable(test_config test_config.c)
target_link_libraries(test_config PRIVATE termemu_common termemu_cjson)
add_test(NAME config COMMAND test_config)

add_executable(test_session test_session.c)
target_link_libraries(test_session PRIVATE termemu_session)
add_test(NAME session COMMAND test_session)

add_executable(test_pty test_pty.c)
target_link_libraries(test_pty PRIVATE termemu_pty)
add_test(NAME pty COMMAND test_pty)

add_executable(test_ipc test_ipc.c)
target_link_libraries(test_ipc PRIVATE termemu_ipc_server Threads::Threads)
add_test(NAME ipc COMMAND test_ipc)

# ── Phase 3/4 tests (no OpenGL/display needed) ────────────────────────────

add_executable(test_layout test_layout.c
    ${CMAKE_SOURCE_DIR}/src/client/ui/layout.c)
target_include_directories(test_layout PRIVATE
    ${CMAKE_SOURCE_DIR}/src/client)
add_test(NAME layout COMMAND test_layout)

add_executable(test_font test_font.c
    ${CMAKE_SOURCE_DIR}/src/client/renderer/font.c)
target_include_directories(test_font PRIVATE
    ${CMAKE_SOURCE_DIR}/src/client/renderer
    ${CMAKE_SOURCE_DIR}/src/client)
target_link_libraries(test_font PRIVATE
    Freetype::Freetype
    termemu_harfbuzz)
add_test(NAME font COMMAND test_font)

add_executable(test_screen test_screen.c
    ${CMAKE_SOURCE_DIR}/src/client/screen.c
    ${CMAKE_SOURCE_DIR}/src/common/vt_parser.c
    ${CMAKE_SOURCE_DIR}/src/common/utf8.c)
target_include_directories(test_screen PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/client
    ${CMAKE_SOURCE_DIR}/src/common)
add_test(NAME screen COMMAND test_screen)
