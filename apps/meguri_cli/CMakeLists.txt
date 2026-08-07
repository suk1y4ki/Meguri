if(MSVC)
    add_compile_options(/W4)
endif()

add_executable(meguri_cli
    main.cpp
    app.rc
)
# .ico 差し替え時に .rc の再コンパイルを促す (OBJECT_DEPENDS の罠対策)
set_source_files_properties(app.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/app.ico)
target_link_libraries(meguri_cli PRIVATE meguri_app stb)
# stb_image_write が sprintf を使うため (HDR 出力経路のみ。PNG では未使用)
target_compile_definitions(meguri_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
# 配布物・実行ファイル名は Meguri_CLI.exe
set_target_properties(meguri_cli PROPERTIES OUTPUT_NAME "Meguri_CLI")
if(MSVC)
    # wmain を使う
    target_link_options(meguri_cli PRIVATE /ENTRY:wmainCRTStartup)
endif()
