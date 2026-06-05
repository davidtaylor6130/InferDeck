cmake_minimum_required(VERSION 3.27)

add_executable(engine_tests
    test_engine.cpp
)

target_link_libraries(engine_tests PRIVATE
    engine
    foundation
    Catch2::Catch2WithMain
)

set_target_properties(engine_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(engine_tests PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(engine_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME engine_tests COMMAND engine_tests)
set_tests_properties(engine_tests PROPERTIES LABELS "unit")
