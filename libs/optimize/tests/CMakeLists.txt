cmake_minimum_required(VERSION 3.27)

find_package(Catch2 REQUIRED)

add_executable(optimize_tests
    test_search_space.cpp
    test_study.cpp
    test_yaml_loader.cpp
)

target_link_libraries(optimize_tests PRIVATE
    optimize
    Catch2::Catch2WithMain
)

target_include_directories(optimize_tests PRIVATE .)

if(MSVC)
    target_compile_options(optimize_tests PRIVATE /W4 /utf-8 /FS)
    target_compile_definitions(optimize_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(optimize_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
include(Catch)
catch_discover_tests(optimize_tests
    TEST_PREFIX "optimize_"
    LABELS "unit"
)
