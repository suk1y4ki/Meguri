if(MSVC)
    add_compile_options(/W4)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/app.rc
    @ONLY)
set(MEGURI_GUI_RC ${CMAKE_CURRENT_BINARY_DIR}/app.rc)

add_executable(meguri_gui WIN32
    main.cpp
    main_window.cpp
    grid_view.cpp
    playback_engine.cpp
    dark_mode.cpp
    strings.cpp
    ${MEGURI_GUI_RC}
)
# .ico 差し替え時に .rc の再コンパイルを促す (OBJECT_DEPENDS の罠対策)
set_source_files_properties(${MEGURI_GUI_RC} PROPERTIES
    OBJECT_DEPENDS ${MEGURI_ICON_RC_PATH})
target_link_libraries(meguri_gui PRIVATE meguri_app)
target_compile_definitions(meguri_gui PRIVATE UNICODE _UNICODE NOMINMAX)
# 配布物・実行ファイル名は Meguri.exe (GUI がメインのため)
set_target_properties(meguri_gui PROPERTIES OUTPUT_NAME "Meguri")
