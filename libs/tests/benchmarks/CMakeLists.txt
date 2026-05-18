# Google Benchmarks for InferDeck backend components
cmake_minimum_required(VERSION 3.27)
project(backend_benchmarks LANGUAGES CXX)

add_executable(backend_benchmarks
    benchmarks.cpp
)

target_include_directories(backend_benchmarks PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/backends/include
    ${CMAKE_SOURCE_DIR}/libs/vector_store/include
    ${CMAKE_SOURCE_DIR}/libs/core/include
    ${CMAKE_SOURCE_DIR}/libs/llama_cpp_wrapper/include
)

target_link_libraries(backend_benchmarks PRIVATE
    core
    backends
    vector_store
    llama_cpp_wrapper
    benchmark::benchmark
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_compile_options(backend_benchmarks PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O3
)
