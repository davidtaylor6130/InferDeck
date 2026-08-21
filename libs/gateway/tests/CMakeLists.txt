cmake_minimum_required(VERSION 3.27)

add_executable(sanitizer_tests
    test_streaming_sanitizer.cpp
)

add_executable(route_tests
    test_routes.cpp
    test_model_store.cpp
    test_dashboard_config_routes.cpp
    test_config_repository.cpp
)

add_executable(anthropic_route_tests
    test_anthropic_routes.cpp
)

add_executable(auth_tests
    test_auth.cpp
)

target_link_libraries(auth_tests
    PRIVATE
        gateway
        foundation
        Catch2::Catch2WithMain
)

target_link_libraries(anthropic_route_tests
    PRIVATE
        gateway
        model
        foundation
        nlohmann_json::nlohmann_json
        httplib::httplib
        Catch2::Catch2WithMain
)

set_target_properties(anthropic_route_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
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

target_compile_definitions(route_tests PRIVATE
    INFERDECK_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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
    target_compile_options(anthropic_route_tests PRIVATE /W4 /utf-8 /FS)
    target_compile_options(auth_tests PRIVATE /W4 /utf-8 /FS)
endif()

add_test(NAME sanitizer_tests COMMAND sanitizer_tests)
add_test(NAME route_tests COMMAND route_tests)
add_test(NAME anthropic_route_tests COMMAND anthropic_route_tests)
add_test(NAME auth_tests COMMAND auth_tests)
set_tests_properties(sanitizer_tests PROPERTIES LABELS "unit")
set_tests_properties(route_tests PROPERTIES LABELS "unit")
set_tests_properties(anthropic_route_tests PROPERTIES LABELS "unit")
set_tests_properties(auth_tests PROPERTIES LABELS "unit")
