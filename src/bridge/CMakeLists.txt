if(TERMEMU_BUILD_APPS)
    add_executable(termemu-bridge main.c)

    target_link_libraries(termemu-bridge
        PRIVATE termemu_ipc
        PRIVATE termemu_common
    )

    install(TARGETS termemu-bridge RUNTIME DESTINATION bin)
endif()
