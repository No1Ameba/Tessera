add_library(tessera_common STATIC
    vt_parser.c
    config.c
    theme_presets.c
    utf8.c
    session_file.c
)

target_include_directories(tessera_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(tessera_common
    PRIVATE tessera_cjson
)
