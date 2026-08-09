if(MSVC)
    add_compile_options(/W4)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/app.rc
    @ONLY)
set(MEGURI_CLI_RC ${CMAKE_CURRENT_BINARY_DIR}/app.rc)

add_executable(meguri_cli
    main.cpp
    ${MEGURI_CLI_RC}
)
# .ico 差し替え時に .rc の再コンパイルを促す (OBJECT_DEPENDS の罠対策)
set_source_files_properties(${MEGURI_CLI_RC} PROPERTIES
    OBJECT_DEPENDS ${MEGURI_ICON_RC_PATH})
target_link_libraries(meguri_cli PRIVATE meguri_app stb)
# stb_image_write が sprintf を使うため (HDR 出力経路のみ。PNG では未使用)
target_compile_definitions(meguri_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
# 配布物・実行ファイル名は Meguri_CLI.exe
set_target_properties(meguri_cli PROPERTIES OUTPUT_NAME "Meguri_CLI")
if(MSVC)
    # wmain を使う
    target_link_options(meguri_cli PRIVATE /ENTRY:wmainCRTStartup)
endif()
