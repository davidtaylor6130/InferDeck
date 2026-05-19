cmake_minimum_required(VERSION 3.27)
project(llama_cpp_wrapper LANGUAGES CXX)

add_library(llama_cpp_wrapper STATIC
    src/LlamaEngine.cpp
    src/VulkanDevice.cpp
    src/GGUFParser.cpp
)

target_include_directories(llama_cpp_wrapper PUBLIC
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Vulkan-Headers/include
)

target_link_libraries(llama_cpp_wrapper PUBLIC
    core
    llama
    ${Vulkan_LIBRARIES}
)

target_include_directories(llama_cpp_wrapper PRIVATE
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(llama_cpp_wrapper PRIVATE
    GGML_VULKAN=1
)

if(MSVC)
    target_compile_options(llama_cpp_wrapper PRIVATE /W4 /utf-8)
else()
    target_compile_options(llama_cpp_wrapper PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS llama_cpp_wrapper
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
