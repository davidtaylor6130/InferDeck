cmake_minimum_required(VERSION 3.27)
project(backends LANGUAGES CXX)

add_library(backends STATIC
    src/GpuResourceManager.cpp
    src/BackendRegistry.cpp
)

target_include_directories(backends PUBLIC
    include
)

target_link_libraries(backends PUBLIC
    core
    llama_cpp_wrapper
    nlohmann_json::nlohmann_json
    ${Vulkan_LIBRARIES}
)

target_compile_definitions(backends PRIVATE
    GGML_VULKAN=1
)

if(MSVC)
    target_compile_options(backends PRIVATE /W4 /utf-8)
else()
    target_compile_options(backends PRIVATE -Wall -Wextra -Wpedantic)
endif()
