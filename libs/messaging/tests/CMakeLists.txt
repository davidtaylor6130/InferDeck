cmake_minimum_required(VERSION 3.27)

add_executable(messaging_tests
    test_messaging.cpp
)

target_link_libraries(messaging_tests PRIVATE
    messaging
    Catch2::Catch2WithMain
)

set_target_properties(messaging_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(messaging_tests PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(messaging_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME messaging_tests COMMAND messaging_tests)
set_tests_properties(messaging_tests PROPERTIES LABELS "unit")
