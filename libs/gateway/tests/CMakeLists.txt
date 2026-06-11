cmake_minimum_required(VERSION 3.27)

add_executable(sanitizer_tests
    test_streaming_sanitizer.cpp
)

add_executable(route_tests
    test_routes.cpp
)

target_link_libraries(sanitizer_tests
    PRIVATE
        gateway
        model
        foundation
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
)

target_link_libraries(route_tests
    PRIVATE
        gateway
        model
        foundation
        nlohmann_json::nlohmann_json
        httplib::httplib
        Catch2::Catch2WithMain
)

set_target_properties(sanitizer_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

set_target_properties(route_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(sanitizer_tests PRIVATE /W4 /utf-8 /FS)
    target_compile_options(route_tests PRIVATE /W4 /utf-8 /FS)
endif()

add_test(NAME sanitizer_tests COMMAND sanitizer_tests)
add_test(NAME route_tests COMMAND route_tests)
set_tests_properties(sanitizer_tests PROPERTIES LABELS "unit")
set_tests_properties(route_tests PROPERTIES LABELS "unit")
