cmake_minimum_required(VERSION 3.27)

add_executable(model_tests
    test_model.cpp
    test_swap_cancellation.cpp
)

target_link_libraries(model_tests PRIVATE
    model
    foundation
    Catch2::Catch2WithMain
)

set_target_properties(model_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(model_tests PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(model_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME model_tests COMMAND model_tests)
set_tests_properties(model_tests PROPERTIES LABELS "unit")
