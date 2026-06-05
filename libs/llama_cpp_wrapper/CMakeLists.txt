cmake_minimum_required(VERSION 3.27)

if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

add_library(llama_cpp_wrapper STATIC
    src/llama_cpp_model.cpp
)

target_include_directories(llama_cpp_wrapper PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    ${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/include
    ${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/common
    ${CMAKE_SOURCE_DIR}/libs/model/include
    ${CMAKE_SOURCE_DIR}/libs/foundation/include
)

target_link_libraries(llama_cpp_wrapper
    PUBLIC
        model
        foundation
        llama
)

if(MSVC)
    target_compile_options(llama_cpp_wrapper PRIVATE /W4 /utf-8 /FS)
    target_compile_definitions(llama_cpp_wrapper PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(llama_cpp_wrapper PRIVATE -Wall -Wextra)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
