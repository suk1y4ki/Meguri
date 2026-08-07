# vendored third-party ライブラリ。
# - libwebp (BSD-3-Clause): アニメーション WEBP のデコード/エンコード
# - nlohmann/json (MIT): 設定ファイル
# - stb_image_write (MIT/Public Domain): CLI のフレーム PNG 出力

# ---- libwebp ----
file(GLOB WEBP_SOURCES
    libwebp/src/dec/*.c
    libwebp/src/demux/*.c
    libwebp/src/dsp/*.c
    libwebp/src/enc/*.c
    libwebp/src/mux/*.c
    libwebp/src/utils/*.c
    libwebp/sharpyuv/*.c
)
add_library(webp STATIC ${WEBP_SOURCES})
target_include_directories(webp
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/src   # <webp/decode.h>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libwebp      # "src/..." / "sharpyuv/..."
)
if(MSVC)
    # third_party には警告を出さない
    target_compile_options(webp PRIVATE /W0)
endif()

# ---- nlohmann/json ----
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ---- stb ----
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
