if(MSVC)
    add_compile_options(/W4)
endif()

add_executable(meguri_gui WIN32
    main.cpp
    main_window.cpp
    grid_view.cpp
    playback_engine.cpp
    dark_mode.cpp
    strings.cpp
    app.rc
)
# .ico 差し替え時に .rc の再コンパイルを促す (OBJECT_DEPENDS の罠対策)
set_source_files_properties(app.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/app.ico)
target_link_libraries(meguri_gui PRIVATE meguri_app)
target_compile_definitions(meguri_gui PRIVATE UNICODE _UNICODE NOMINMAX)
# 配布物・実行ファイル名は Meguri.exe (GUI がメインのため)
set_target_properties(meguri_gui PROPERTIES OUTPUT_NAME "Meguri")
