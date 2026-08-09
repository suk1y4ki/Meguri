if(MSVC)
    add_compile_options(/W4)
endif()

add_executable(meguri_tests
    test_core.cpp
)
target_link_libraries(meguri_tests PRIVATE meguri_core)
target_link_libraries(meguri_tests PRIVATE meguri_io)
add_test(NAME core_tests COMMAND meguri_tests)
