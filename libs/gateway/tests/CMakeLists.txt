cmake_minimum_required(VERSION 3.27)

add_executable(sanitizer_tests
    test_streaming_sanitizer.cpp
    test_auth.cpp
    test_routes.cpp
)

target_link_libraries(sanitizer_tests
    PRIVATE
        gateway
        scheduler
        model
        engine
        foundation
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
)

set_target_properties(sanitizer_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(sanitizer_tests PRIVATE /W4 /utf-8 /FS)
endif()

add_test(NAME sanitizer_tests COMMAND sanitizer_tests)
set_tests_properties(sanitizer_tests PROPERTIES LABELS "unit")
