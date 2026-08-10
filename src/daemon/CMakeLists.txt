# session 관리 라이브러리 — 항상 빌드 (테스트에서 직접 링크 가능)
add_library(tessera_session STATIC session.c)

target_include_directories(tessera_session
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(tessera_session
    PUBLIC tessera_common
)

# IPC 서버 라이브러리 — 항상 빌드 (테스트에서 직접 링크 가능)
add_library(tessera_ipc_server STATIC ipc_server.c)

target_include_directories(tessera_ipc_server
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(tessera_ipc_server
    PUBLIC tessera_session
    PUBLIC tessera_ipc
    PUBLIC tessera_pty
)

# 데몬 실행 파일 — 앱 빌드 시에만
if(TESSERA_BUILD_APPS)
    add_executable(tessera-daemon main.c)

    target_link_libraries(tessera-daemon
        PRIVATE tessera_ipc_server
    )

    install(TARGETS tessera-daemon RUNTIME DESTINATION bin)
endif()
