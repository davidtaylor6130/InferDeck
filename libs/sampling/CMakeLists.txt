cmake_minimum_required(VERSION 3.27)
project(sampling LANGUAGES CXX)

add_library(sampling STATIC
    src/sampler.cpp
    src/family_defaults.cpp
)

target_include_directories(sampling PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp
)

target_link_libraries(sampling PUBLIC
    foundation
    llama-common
    llama
)

set_target_properties(sampling PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(sampling PRIVATE /W4 /utf-8 /permissive- /FS)
else()
    target_compile_options(sampling PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
