add_library(termemu_common STATIC
    vt_parser.c
    config.c
    utf8.c
    session_file.c
)

target_include_directories(termemu_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(termemu_common
    PRIVATE PkgConfig::CJSON
)
