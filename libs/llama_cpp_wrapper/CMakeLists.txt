# libs/llama_cpp_wrapper CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(llama_cpp_wrapper LANGUAGES CXX)

# Add llama.cpp as a static library (vendored or submodule)
# For V1, we include llama.cpp headers directly from a known location
# In production, use: add_subdirectory(llama.cpp EXCLUDE_FROM_ALL)

add_library(llama_cpp_wrapper STATIC
    src/LlamaEngine.cpp
    src/VulkanDevice.cpp
    src/GGUFParser.cpp
)

target_include_directories(llama_cpp_wrapper PUBLIC
    include
    # llama.cpp includes (adjust path as needed)
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/ggml/include
)

target_link_libraries(llama_cpp_wrapper PUBLIC
    core
    Vulkan::Vulkan
)

# Compiler settings
target_compile_options(llama_cpp_wrapper PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# WASM build (optional, requires Emscripten)
option(INFERDECK_BUILD_WASM "Build WASM module" OFF)
if(INFERDECK_BUILD_WASM)
    add_executable(llama_cpp_wrapper_wasm
        src/LlamaEngine.cpp
        src/GGUFParser.cpp
    )
    set_target_properties(llama_cpp_wrapper_wasm PROPERTIES
        SUFFIX ".wasm"
        OUTPUT_NAME "llama_cpp_wrapper"
    )
    target_compile_options(llama_cpp_wrapper_wasm PRIVATE
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
        -sALLOW_MEMORY_GROWTH=1
    )
endif()
