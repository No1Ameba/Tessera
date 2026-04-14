if(TERMEMU_PLATFORM STREQUAL "windows")
    set(PTY_SOURCE     windows/pty_win.c)
    set(IPC_SOURCE     windows/ipc_win.c)
    set(FSWATCH_SOURCE windows/fs_watch_win.c)
else()
    set(PTY_SOURCE     posix/pty_posix.c)
    set(IPC_SOURCE     posix/ipc_posix.c)
    set(FSWATCH_SOURCE posix/fs_watch_posix.c)
endif()

# PTY 추상화 — 항상 빌드 (테스트에서 직접 링크 가능)
add_library(termemu_pty STATIC ${PTY_SOURCE})

target_include_directories(termemu_pty
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

if(NOT WIN32)
    target_link_libraries(termemu_pty PRIVATE util)
endif()

# IPC 소켓 래퍼 — 항상 빌드 (테스트에서 직접 링크 가능)
add_library(termemu_ipc STATIC ${IPC_SOURCE})

target_include_directories(termemu_ipc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(termemu_ipc
    PUBLIC termemu_common
)

# 전체 플랫폼 라이브러리 (fswatch 포함) — 앱 빌드 시에만
if(TERMEMU_BUILD_APPS)
    add_library(termemu_platform STATIC ${FSWATCH_SOURCE})

    target_include_directories(termemu_platform
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(termemu_platform
        PUBLIC termemu_pty
        PUBLIC termemu_ipc
    )
endif()
