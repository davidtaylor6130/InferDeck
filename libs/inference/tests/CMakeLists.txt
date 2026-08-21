cmake_minimum_required(VERSION 3.27)

add_executable(inference_tests
    test_domain.cpp
    test_dependency_policy.cpp
)

target_link_libraries(inference_tests PRIVATE
    inference
    Catch2::Catch2WithMain
)

target_compile_definitions(inference_tests PRIVATE
    INFERDECK_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(inference_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(inference_tests PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(inference_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME inference_tests COMMAND inference_tests)
set_tests_properties(inference_tests PROPERTIES LABELS unit)
