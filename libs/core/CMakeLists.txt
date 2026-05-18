# libs/core CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(core LANGUAGES CXX)

add_library(core STATIC
    src/Logger.cpp
    src/Metrics.cpp
    src/Config.cpp
)

target_include_directories(core PUBLIC
    include
)

target_link_libraries(core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    fmt::fmt
)

# Compiler warnings
target_compile_options(core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)
