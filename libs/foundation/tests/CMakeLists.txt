cmake_minimum_required(VERSION 3.27)

add_executable(foundation_tests
    test_foundation.cpp
)

target_link_libraries(foundation_tests PRIVATE
    foundation
    Catch2::Catch2WithMain
)

set_target_properties(foundation_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(foundation_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(foundation_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME foundation_tests COMMAND foundation_tests)
set_tests_properties(foundation_tests PROPERTIES LABELS "unit")
