cmake_minimum_required(VERSION 3.27)
project(llama_cpp_wrapper LANGUAGES CXX)

add_library(llama_cpp_wrapper STATIC
    src/LlamaServerManager.cpp
    src/LlamaEngine.cpp
    src/GGUFParser.cpp
)

target_include_directories(llama_cpp_wrapper PUBLIC
    include
    ${llama_cpp_SOURCE_DIR}/include
    ${llama_cpp_SOURCE_DIR}/ggml/include
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
)

target_link_libraries(llama_cpp_wrapper PUBLIC
    core
    nlohmann_json::nlohmann_json
    ws2_32
    winhttp
    iphlpapi
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
