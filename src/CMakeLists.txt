# コアと I/O 層。依存方向: apps -> app -> io -> core (一方向)。
# core は標準 C++ のみ。io はプラットフォーム API / vendored ライブラリを使ってよい。
if(MSVC)
    add_compile_options(/W4)
endif()

add_library(meguri_core STATIC
    core/layout.cpp
    core/scheduler.cpp
    core/selection.cpp
    core/playback.cpp
    core/item_filter.cpp
)
target_include_directories(meguri_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(meguri_io STATIC
    io/scanner.cpp
    io/webp_decoder.cpp
    io/mp4_decoder.cpp
    io/image_decoder.cpp
    io/d3d_video.cpp
    io/audio_player.cpp
    io/probe.cpp
    io/recycle.cpp
    io/sample_gen.cpp
)
target_include_directories(meguri_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(meguri_io PUBLIC meguri_core)
target_link_libraries(meguri_io PRIVATE webp)

add_library(meguri_app STATIC
    app/settings.cpp
    app/probe_cache.cpp
)
target_include_directories(meguri_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(meguri_app PUBLIC meguri_io)
target_link_libraries(meguri_app PRIVATE nlohmann_json)
