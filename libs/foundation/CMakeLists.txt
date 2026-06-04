cmake_minimum_required(VERSION 3.27)
project(foundation LANGUAGES CXX)

add_library(foundation STATIC
    src/logging.cpp
    src/json_utils.cpp
)

target_include_directories(foundation PUBLIC
    include
)

target_link_libraries(foundation PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    fmt::fmt
)

set_target_properties(foundation PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(foundation PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(foundation PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
