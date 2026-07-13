cmake_minimum_required(VERSION 3.27)
project(inferdeck_gateway LANGUAGES CXX)

add_executable(inferdeck-gateway
    src/main.cpp
)

find_package(yaml-cpp REQUIRED)

target_link_libraries(inferdeck-gateway PRIVATE
    gateway
    model
    foundation
    llama_cpp_wrapper
    native_runtimes
    llama
    yaml-cpp::yaml-cpp
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(inferdeck-gateway PRIVATE dbghelp)
    target_link_options(inferdeck-gateway PRIVATE /STACK:33554432)
endif()

set_target_properties(inferdeck-gateway PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(inferdeck-gateway PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(inferdeck-gateway PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(INFERDECK_DASHBOARD_STATIC_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/static"
    CACHE PATH "Built InferDeck dashboard static asset directory")

add_custom_command(TARGET inferdeck-gateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:inferdeck-gateway>/static"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${INFERDECK_DASHBOARD_STATIC_DIR}"
            "$<TARGET_FILE_DIR:inferdeck-gateway>/static"
    COMMENT "Copying InferDeck dashboard static assets"
)

if(INFERDECK_BUILD_TESTS)
    add_executable(config_tests tests/test_config.cpp)
    target_include_directories(config_tests PRIVATE src)
    target_link_libraries(config_tests PRIVATE model foundation yaml-cpp::yaml-cpp Catch2::Catch2WithMain)
    set_target_properties(config_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    add_test(NAME config_tests COMMAND config_tests)
    set_tests_properties(config_tests PROPERTIES LABELS "unit")
endif()
