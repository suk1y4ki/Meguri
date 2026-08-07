cmake_minimum_required(VERSION 3.25)

project(Meguri
    VERSION 0.1.0
    DESCRIPTION "Meguri - Animated WebP / MP4 bulk viewer & organizer"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /W4 は自前コードのディレクトリ側で付与する (third_party に警告を出さないため)
if(MSVC)
    add_compile_options(/utf-8 /EHsc)
else()
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

add_subdirectory(third_party)
add_subdirectory(src)
add_subdirectory(apps/meguri_cli)
if(WIN32)
    add_subdirectory(apps/meguri_gui)
endif()
add_subdirectory(tests)

# ---- 配布用ビルド ----
# scripts/make_dist.ps1 が Release ビルド + 全テスト + install を一括で行う
install(TARGETS meguri_cli RUNTIME DESTINATION .)
if(WIN32)
    install(TARGETS meguri_gui RUNTIME DESTINATION .)
endif()
install(FILES README.md README.en.md TECHNICAL.md LICENSE THIRD-PARTY-NOTICES.md DESTINATION .)
install(FILES third_party/libwebp/COPYING DESTINATION licenses RENAME libwebp-LICENSE.txt)
install(FILES third_party/nlohmann/LICENSE.MIT DESTINATION licenses RENAME nlohmann-json-LICENSE.txt)
install(FILES third_party/stb/LICENSE DESTINATION licenses RENAME stb-LICENSE.txt)
