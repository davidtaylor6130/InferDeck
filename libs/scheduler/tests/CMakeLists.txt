cmake_minimum_required(VERSION 3.27)

add_executable(scheduler_tests
    test_scheduler.cpp
)

target_link_libraries(scheduler_tests
    PRIVATE
        scheduler
        model
        engine
        foundation
        Catch2::Catch2WithMain
)

set_target_properties(scheduler_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(scheduler_tests PRIVATE /W4 /utf-8 /FS)
endif()

add_test(NAME scheduler_tests COMMAND scheduler_tests)
set_tests_properties(scheduler_tests PROPERTIES LABELS "unit")
