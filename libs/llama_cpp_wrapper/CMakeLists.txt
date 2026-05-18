# libs/llama_cpp_wrapper CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(llama_cpp_wrapper LANGUAGES CXX)

# Build llama.cpp as a static library from submodule
add_library(llama STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/src/llama.cpp
)

target_include_directories(llama PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/ggml/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/ggml/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/ggml/src/ggml-cpp
)

target_compile_definitions(llama PRIVATE
    GGML_VULKAN=1
    GGML_KV_OVERHEAD=1
    GGML_USE_CUDA=0
    GGML_USE_HIP=0
    GGML_USE_METAL=0
    GGML_USE_CANN=0
    GGML_USE_SYCL=0
    GGML_USE_BLAS=0
)

target_compile_options(llama PRIVATE
    -O3
    -march=native
)

# Vulkan-specific defines
target_compile_definitions(llama PRIVATE
    GGML_VULKAN_CHECK_RESULTS=0
    GGML_VULKAN_MEMORY_DEBUG=0
    GGML_VULKAN_DEBUG=0
)

# Create wrapper library
add_library(llama_cpp_wrapper STATIC
    src/LlamaEngine.cpp
    src/VulkanDevice.cpp
    src/GGUFParser.cpp
)

target_include_directories(llama_cpp_wrapper PUBLIC
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp/ggml/include
)

target_link_libraries(llama_cpp_wrapper PUBLIC
    core
    llama
    Vulkan::Vulkan
)

# Compiler settings
target_compile_options(llama_cpp_wrapper PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

# Install wrapper library
install(TARGETS llama_cpp_wrapper
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
