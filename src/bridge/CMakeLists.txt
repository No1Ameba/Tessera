if(TESSERA_BUILD_APPS)
    add_executable(tessera-bridge main.c)

    target_link_libraries(tessera-bridge
        PRIVATE tessera_ipc
        PRIVATE tessera_common
    )

    install(TARGETS tessera-bridge RUNTIME DESTINATION bin)
endif()
