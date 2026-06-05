cmake_minimum_required(VERSION 3.27)
project(inferdeck_benchmark_runner LANGUAGES CXX)

add_executable(inferdeck-bench
    src/main.cpp
)

find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

target_link_libraries(inferdeck-bench PRIVATE
    optimize
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
)

target_include_directories(inferdeck-bench PRIVATE include)

set_target_properties(inferdeck-bench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(inferdeck-bench PRIVATE /W4 /utf-8 /FS)
    target_compile_definitions(inferdeck-bench PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(inferdeck-bench PRIVATE -Wall -Wextra)
endif()
