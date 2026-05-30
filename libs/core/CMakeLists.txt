cmake_minimum_required(VERSION 3.27)
project(core LANGUAGES CXX)

add_library(core STATIC
    src/Logger.cpp
    src/Metrics.cpp
    src/Config.cpp
    src/JobQueue.cpp
    src/WorkerPool.cpp
    src/Base64.cpp
)

target_include_directories(core PUBLIC
    include
)

target_link_libraries(core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    fmt::fmt
    yaml-cpp::yaml-cpp
)

if(MSVC)
    target_compile_options(core PRIVATE /W4 /utf-8)
else()
    target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic)
endif()
