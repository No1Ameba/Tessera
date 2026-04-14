# session 관리 라이브러리 — 항상 빌드 (테스트에서 직접 링크 가능)
add_library(termemu_session STATIC session.c)

target_include_directories(termemu_session
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(termemu_session
    PUBLIC termemu_common
)

# IPC 서버 라이브러리 — 항상 빌드 (테스트에서 직접 링크 가능)
add_library(termemu_ipc_server STATIC ipc_server.c)

target_include_directories(termemu_ipc_server
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(termemu_ipc_server
    PUBLIC termemu_session
    PUBLIC termemu_ipc
    PUBLIC termemu_pty
)

# 데몬 실행 파일 — 앱 빌드 시에만
if(TERMEMU_BUILD_APPS)
    add_executable(termemu-daemon main.c)

    target_link_libraries(termemu-daemon
        PRIVATE termemu_ipc_server
    )

    install(TARGETS termemu-daemon RUNTIME DESTINATION bin)
endif()
