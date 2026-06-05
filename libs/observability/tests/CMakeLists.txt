cmake_minimum_required(VERSION 3.27)

find_package(Catch2 REQUIRED)

set(TEST_SRCS
    test_logger.cpp
    test_gpu_telemetry.cpp
    test_stats_db.cpp
)

add_executable(observability_tests ${TEST_SRCS})
target_link_libraries(observability_tests PRIVATE
    observability
    Catch2::Catch2WithMain
)
target_include_directories(observability_tests PRIVATE .)

if(MSVC)
    target_compile_options(observability_tests PRIVATE /W4 /utf-8 /bigobj /Z7)
    target_compile_definitions(observability_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(observability_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
include(Catch)
catch_discover_tests(observability_tests
    TEST_PREFIX "observability_"
    LABELS "unit"
)
