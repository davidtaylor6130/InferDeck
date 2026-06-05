cmake_minimum_required(VERSION 3.27)

find_package(Catch2 REQUIRED)

add_executable(parity_tests
    test_compare.cpp
    test_prompts.cpp
    test_runner.cpp
)

target_link_libraries(parity_tests PRIVATE
    parity
    Catch2::Catch2WithMain
)

target_include_directories(parity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

if(MSVC)
    target_compile_options(parity_tests PRIVATE /W4 /utf-8 /FS)
    target_compile_definitions(parity_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(parity_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
include(Catch)
catch_discover_tests(parity_tests
    TEST_PREFIX "parity_"
    LABELS "integration"
)
